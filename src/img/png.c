/* img/png.c — Sıfırdan PNG kodlayıcı/çözücü ve RFC 1950/1951 (zlib/DEFLATE) implementasyonu.
 * Kodlayıcı: LZ77 32K kayan pencere hash zinciri, kanonik Huffman ağacı inşası (dinamik + sabit
 *            blok boyutu karşılaştırmalı) ve Paeth/Sub/Up/Avg/None uyarlamalı satır filtresi seçimi.
 * Çözücü:    RFC 1951 uyumlu tam Inflate (BTYPE=00/01/10) ve 8-bit RGBA/RGB/Gri/Gri+A/Paletli PNG.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/img/png.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "img/png.h"
#include "raster/shade_abi.h"

/* --- Yardımcı bellek ve endian işlemleri --- */

static inline void mem_copy(void* dst, const void* src, usize n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    for (usize i = 0; i < n; i++) d[i] = s[i];
}

static inline void mem_zero(void* dst, usize n) {
    u8* d = (u8*)dst;
    for (usize i = 0; i < n; i++) d[i] = 0;
}

static inline u32 read_be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static inline void write_be32(u8* p, u32 v) {
    p[0] = (u8)((v >> 24) & 0xFFu);
    p[1] = (u8)((v >> 16) & 0xFFu);
    p[2] = (u8)((v >>  8) & 0xFFu);
    p[3] = (u8)((v >>  0) & 0xFFu);
}

/* --- CRC32 & Adler-32 --- */

u32 png_crc32(const u8* data, usize len) {
    u32 crc = 0xFFFFFFFFu;
    for (usize i = 0; i < len; i++) {
        crc ^= (u32)data[i];
        for (i32 b = 0; b < 8; b++) {
            u32 mask = -(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

u32 png_adler32(const u8* data, usize len) {
    const u32 MOD_ADLER = 65521u;
    u32 s1 = 1u;
    u32 s2 = 0u;
    usize i = 0;
    while (i < len) {
        usize chunk = len - i;
        if (chunk > 5552u) chunk = 5552u;
        for (usize j = 0; j < chunk; j++) {
            s1 += data[i + j];
            s2 += s1;
        }
        s1 %= MOD_ADLER;
        s2 %= MOD_ADLER;
        i += chunk;
    }
    return (s2 << 16u) | s1;
}

/* --- RFC 1951 Uzunluk (257..285) ve Mesafe (0..29) Tabloları --- */

static const u16 LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const u8 LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const u16 DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const u8 DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const u8 CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static inline u16 bit_reverse16(u16 x, u32 bits) {
    u16 r = 0;
    for (u32 i = 0; i < bits; i++) {
        r = (u16)((r << 1u) | (x & 1u));
        x >>= 1u;
    }
    return r;
}

/* ============================================================================
 * INFLATE (RFC 1951 Çözücü)
 * ============================================================================ */

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMS 288

typedef struct {
    u16 counts[HUFF_MAX_BITS + 1];
    u16 symbols[HUFF_MAX_SYMS];
} HuffTable;

static b32 huff_build(HuffTable* ht, const u8* lengths, u32 num_syms) {
    for (u32 i = 0; i <= HUFF_MAX_BITS; i++) ht->counts[i] = 0;
    for (u32 i = 0; i < num_syms; i++) {
        u8 l = lengths[i];
        if (l > HUFF_MAX_BITS) return FALSE;
        ht->counts[l]++;
    }
    if (ht->counts[0] == num_syms) {
        return TRUE; /* tamamen boş tablo (ör. mesafe kodu hiç kullanılmadığında) */
    }

    /* Aşırı abone (oversubscribed) kontrolü */
    i32 left = 1;
    for (u32 bits = 1; bits <= HUFF_MAX_BITS; bits++) {
        left <<= 1;
        left -= (i32)ht->counts[bits];
        if (left < 0) return FALSE;
    }

    u16 offs[HUFF_MAX_BITS + 1];
    offs[1] = 0;
    for (u32 bits = 1; bits < HUFF_MAX_BITS; bits++) {
        offs[bits + 1] = (u16)(offs[bits] + ht->counts[bits]);
    }

    for (u32 sym = 0; sym < num_syms; sym++) {
        u8 l = lengths[sym];
        if (l != 0) {
            ht->symbols[offs[l]++] = (u16)sym;
        }
    }
    return TRUE;
}

typedef struct {
    const u8* src;
    usize     len;
    usize     pos;
    u64       bitbuf;
    u32       bitcnt;
} BitReader;

static inline b32 br_ensure(BitReader* br, u32 n) {
    while (br->bitcnt < n) {
        if (br->pos >= br->len) return FALSE;
        br->bitbuf |= ((u64)br->src[br->pos++]) << br->bitcnt;
        br->bitcnt += 8u;
    }
    return TRUE;
}

static inline u32 br_read(BitReader* br, u32 n, b32* ok) {
    if (n == 0) return 0;
    if (!br_ensure(br, n)) {
        *ok = FALSE;
        return 0;
    }
    u32 val = (u32)(br->bitbuf & ((1ULL << n) - 1ULL));
    br->bitbuf >>= n;
    br->bitcnt -= n;
    return val;
}

static inline void br_align_byte(BitReader* br) {
    u32 drop = br->bitcnt & 7u;
    br->bitbuf >>= drop;
    br->bitcnt -= drop;
}

static i32 huff_decode(BitReader* br, const HuffTable* ht) {
    i32 code = 0;
    i32 first = 0;
    i32 index = 0;
    for (u32 len = 1; len <= HUFF_MAX_BITS; len++) {
        if (br->bitcnt == 0) {
            if (br->pos >= br->len) return -1;
            br->bitbuf = (u64)br->src[br->pos++];
            br->bitcnt = 8u;
        }
        code |= (i32)(br->bitbuf & 1ULL);
        br->bitbuf >>= 1u;
        br->bitcnt -= 1u;

        i32 count = (i32)ht->counts[len];
        if (code - count < first) {
            return (i32)ht->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

typedef struct {
    u8*    data;
    usize  len;
    usize  cap;
    Arena* arena;
} OutBuf;

static void outbuf_init(OutBuf* ob, Arena* arena, usize init_cap) {
    if (init_cap < 256) init_cap = 256;
    ob->arena = arena;
    ob->cap   = init_cap;
    ob->len   = 0;
    ob->data  = (u8*)arena_alloc(arena, init_cap, 16);
}

static void outbuf_reserve(OutBuf* ob, usize add) {
    if (ob->len + add <= ob->cap) return;
    usize ncap = ob->cap * 2u;
    while (ncap < ob->len + add) ncap *= 2u;
    u8* ndata = (u8*)arena_alloc(ob->arena, ncap, 16);
    mem_copy(ndata, ob->data, ob->len);
    ob->data = ndata;
    ob->cap  = ncap;
}

static inline void outbuf_push(OutBuf* ob, u8 b) {
    if (ob->len >= ob->cap) outbuf_reserve(ob, 1);
    ob->data[ob->len++] = b;
}

u8* png_deflate_decompress(const u8* src, usize src_len, usize* out_len, Arena* arena) {
    if (!src || !out_len || !arena) return NULL;
    *out_len = 0;

    BitReader br = { src, src_len, 0, 0, 0 };
    OutBuf ob;
    outbuf_init(&ob, arena, src_len * 2u + 256u);

    b32 ok = TRUE;
    u32 bfinal = 0;

    while (!bfinal && ok) {
        bfinal = br_read(&br, 1, &ok);
        u32 btype = br_read(&br, 2, &ok);
        if (!ok) return NULL;

        if (btype == 0u) {
            /* Stored (sıkıştırılmamış) blok */
            br_align_byte(&br);
            u32 len  = br_read(&br, 16, &ok);
            u32 nlen = br_read(&br, 16, &ok);
            if (!ok || (len ^ 0xFFFFu) != nlen) return NULL;
            outbuf_reserve(&ob, len);
            for (u32 i = 0; i < len; i++) {
                u32 b = br_read(&br, 8, &ok);
                if (!ok) return NULL;
                ob.data[ob.len++] = (u8)b;
            }
        } else if (btype == 1u || btype == 2u) {
            HuffTable lit_ht, dist_ht;
            u8 lengths[288 + 32];

            if (btype == 1u) {
                /* Sabit Huffman tablosu */
                for (u32 i = 0;   i <= 143; i++) lengths[i] = 8;
                for (u32 i = 144; i <= 255; i++) lengths[i] = 9;
                for (u32 i = 256; i <= 279; i++) lengths[i] = 7;
                for (u32 i = 280; i <= 287; i++) lengths[i] = 8;
                for (u32 i = 0;   i < 32;   i++) lengths[288 + i] = 5;
                if (!huff_build(&lit_ht, lengths, 288)) return NULL;
                if (!huff_build(&dist_ht, lengths + 288, 32)) return NULL;
            } else {
                /* Dinamik Huffman tablosu */
                u32 hlit  = br_read(&br, 5, &ok) + 257u;
                u32 hdist = br_read(&br, 5, &ok) + 1u;
                u32 hclen = br_read(&br, 4, &ok) + 4u;
                if (!ok || hlit > 286u || hdist > 30u) return NULL;

                u8 clen[19];
                for (u32 i = 0; i < 19; i++) clen[i] = 0;
                for (u32 i = 0; i < hclen; i++) {
                    clen[CLEN_ORDER[i]] = (u8)br_read(&br, 3, &ok);
                }
                if (!ok) return NULL;

                HuffTable code_ht;
                if (!huff_build(&code_ht, clen, 19)) return NULL;

                u32 total = hlit + hdist;
                u32 idx = 0;
                while (idx < total) {
                    i32 sym = huff_decode(&br, &code_ht);
                    if (sym < 0) return NULL;
                    if (sym <= 15) {
                        lengths[idx++] = (u8)sym;
                    } else if (sym == 16) {
                        if (idx == 0) return NULL;
                        u32 rep = br_read(&br, 2, &ok) + 3u;
                        if (!ok || idx + rep > total) return NULL;
                        u8 prev = lengths[idx - 1];
                        for (u32 r = 0; r < rep; r++) lengths[idx++] = prev;
                    } else if (sym == 17) {
                        u32 rep = br_read(&br, 3, &ok) + 3u;
                        if (!ok || idx + rep > total) return NULL;
                        for (u32 r = 0; r < rep; r++) lengths[idx++] = 0;
                    } else if (sym == 18) {
                        u32 rep = br_read(&br, 7, &ok) + 11u;
                        if (!ok || idx + rep > total) return NULL;
                        for (u32 r = 0; r < rep; r++) lengths[idx++] = 0;
                    } else {
                        return NULL;
                    }
                }
                if (!huff_build(&lit_ht, lengths, hlit)) return NULL;
                if (!huff_build(&dist_ht, lengths + hlit, hdist)) return NULL;
            }

            /* Sembol çözme döngüsü */
            for (;;) {
                i32 sym = huff_decode(&br, &lit_ht);
                if (sym < 0) return NULL;
                if (sym < 256) {
                    outbuf_push(&ob, (u8)sym);
                } else if (sym == 256) {
                    break; /* Blok sonu (EOB) */
                } else if (sym <= 285) {
                    u32 lidx = (u32)(sym - 257);
                    u32 match_len = (u32)LEN_BASE[lidx] + br_read(&br, LEN_EXTRA[lidx], &ok);
                    if (!ok) return NULL;

                    i32 dsym = huff_decode(&br, &dist_ht);
                    if (dsym < 0 || dsym >= 30) return NULL;
                    u32 match_dist = (u32)DIST_BASE[dsym] + br_read(&br, DIST_EXTRA[dsym], &ok);
                    if (!ok || match_dist == 0 || match_dist > ob.len) return NULL;

                    outbuf_reserve(&ob, match_len);
                    usize src_off = ob.len - match_dist;
                    for (u32 i = 0; i < match_len; i++) {
                        ob.data[ob.len++] = ob.data[src_off + i];
                    }
                } else {
                    return NULL;
                }
            }
        } else {
            return NULL; /* BTYPE == 3 geçersiz */
        }
    }

    *out_len = ob.len;
    return ob.data;
}

u8* png_zlib_decompress(const u8* src, usize src_len, usize* out_len, Arena* arena) {
    if (!src || src_len < 6 || !out_len || !arena) return NULL;
    u8 cmf = src[0];
    u8 flg = src[1];
    if ((cmf & 0x0Fu) != 8u) return NULL;           /* CM == 8 (DEFLATE) */
    if ((cmf >> 4u) > 7u) return NULL;              /* CINFO <= 7 (32K pencere) */
    if ((((u32)cmf << 8u) | (u32)flg) % 31u != 0u) return NULL;
    if (flg & 0x20u) return NULL;                   /* FDICT desteklenmez */

    usize raw_len = 0;
    u8* raw = png_deflate_decompress(src + 2, src_len - 6, &raw_len, arena);
    if (!raw) return NULL;

    u32 expected_adler = read_be32(src + src_len - 4);
    u32 actual_adler   = png_adler32(raw, raw_len);
    if (expected_adler != actual_adler) return NULL;

    *out_len = raw_len;
    return raw;
}

/* ============================================================================
 * DEFLATE KODLAYICI (LZ77 Hash Zinciri + Dinamik & Sabit Huffman)
 * ============================================================================ */

typedef struct {
    OutBuf ob;
    u64    bitbuf;
    u32    bitcnt;
} BitWriter;

static void bw_init(BitWriter* bw, Arena* arena, usize cap) {
    outbuf_init(&bw->ob, arena, cap);
    bw->bitbuf = 0;
    bw->bitcnt = 0;
}

static inline void bw_put(BitWriter* bw, u32 bits, u32 n) {
    bw->bitbuf |= ((u64)(bits & ((1ULL << n) - 1ULL))) << bw->bitcnt;
    bw->bitcnt += n;
    while (bw->bitcnt >= 8u) {
        outbuf_push(&bw->ob, (u8)(bw->bitbuf & 0xFFu));
        bw->bitbuf >>= 8u;
        bw->bitcnt -= 8u;
    }
}

static inline void bw_flush_byte(BitWriter* bw) {
    if (bw->bitcnt > 0u) {
        outbuf_push(&bw->ob, (u8)(bw->bitbuf & 0xFFu));
        bw->bitbuf = 0;
        bw->bitcnt = 0;
    }
}

/* LZ77 Sembol Kaydı: dist == 0 ise lit_len = literal (0..255); dist > 0 ise lit_len = eşleşme uzunluğu (3..258) */
typedef struct {
    u16 lit_len;
    u16 dist;
} LzToken;

static inline u32 len_to_code(u32 len, u32* extra_bits, u32* extra_val) {
    if (len == 258u) {
        *extra_bits = 0;
        *extra_val = 0;
        return 285u;
    }
    for (u32 i = 0; i < 28u; i++) {
        if (len >= LEN_BASE[i] && len < LEN_BASE[i + 1]) {
            *extra_bits = LEN_EXTRA[i];
            *extra_val  = len - LEN_BASE[i];
            return 257u + i;
        }
    }
    *extra_bits = 0;
    *extra_val = 0;
    return 257u;
}

static inline u32 dist_to_code(u32 dist, u32* extra_bits, u32* extra_val) {
    for (u32 i = 0; i < 29u; i++) {
        if (dist >= DIST_BASE[i] && dist < DIST_BASE[i + 1]) {
            *extra_bits = DIST_EXTRA[i];
            *extra_val  = dist - DIST_BASE[i];
            return i;
        }
    }
    *extra_bits = DIST_EXTRA[29];
    *extra_val  = dist - DIST_BASE[29];
    return 29u;
}

/* Derinlik sınırlı (max_bits <= 15) kanonik Huffman kod uzunluğu üretici */
typedef struct {
    u32 freq;
    i16 sym;
    i16 parent;
    u8  depth;
} HuffNode;

static void build_canonical_codes(const u32* freqs, u32 num_syms, u32 max_bits,
                                  u8* out_lens, u16* out_codes) {
    for (u32 i = 0; i < num_syms; i++) {
        out_lens[i] = 0;
        out_codes[i] = 0;
    }

    u32 active_syms[288];
    u32 active_cnt = 0;
    for (u32 i = 0; i < num_syms; i++) {
        if (freqs[i] > 0) {
            active_syms[active_cnt++] = i;
        }
    }

    if (active_cnt == 0) return;
    if (active_cnt == 1) {
        u32 s0 = active_syms[0];
        out_lens[s0] = 1;
        out_codes[s0] = 0;
        return;
    }

    /* Frekansa göre artan sırala (insertion sort, N <= 288) */
    for (u32 i = 1; i < active_cnt; i++) {
        u32 key = active_syms[i];
        u32 kf  = freqs[key];
        i32 j   = (i32)i - 1;
        while (j >= 0 && (freqs[active_syms[j]] > kf ||
                         (freqs[active_syms[j]] == kf && active_syms[j] > key))) {
            active_syms[j + 1] = active_syms[j];
            j--;
        }
        active_syms[j + 1] = key;
    }

    /* Standart Huffman ağacı kur (en fazla 2*288 düğüm) */
    HuffNode nodes[580];
    u32 leaf_cnt = active_cnt;
    for (u32 i = 0; i < leaf_cnt; i++) {
        nodes[i].freq   = freqs[active_syms[i]];
        nodes[i].sym    = (i16)active_syms[i];
        nodes[i].parent = -1;
        nodes[i].depth  = 0;
    }

    u32 next_leaf = 0;
    u32 next_int  = leaf_cnt;
    u32 num_nodes = leaf_cnt;

    while ((leaf_cnt - next_leaf) + (num_nodes - next_int) > 1u) {
        u32 c[2];
        for (u32 k = 0; k < 2; k++) {
            if (next_leaf < leaf_cnt &&
                (next_int >= num_nodes || nodes[next_leaf].freq <= nodes[next_int].freq)) {
                c[k] = next_leaf++;
            } else {
                c[k] = next_int++;
            }
        }
        u32 p = num_nodes++;
        nodes[p].freq   = nodes[c[0]].freq + nodes[c[1]].freq;
        nodes[p].sym    = -1;
        nodes[p].parent = -1;
        nodes[p].depth  = 0;
        nodes[c[0]].parent = (i16)p;
        nodes[c[1]].parent = (i16)p;
    }

    /* Kökten yapraklara derinlik hesapla */
    nodes[num_nodes - 1].depth = 0;
    for (i32 i = (i32)num_nodes - 2; i >= 0; i--) {
        nodes[i].depth = (u8)(nodes[nodes[i].parent].depth + 1u);
    }

    /* Uzunluk dağılımı ve max_bits kırpması (Kraft eşitsizliği onarımı) */
    u32 bl_count[16];
    for (u32 i = 0; i <= max_bits; i++) bl_count[i] = 0;

    u32 overflow = 0;
    for (u32 i = 0; i < leaf_cnt; i++) {
        u32 d = nodes[i].depth;
        if (d > max_bits) {
            d = max_bits;
            overflow++;
        }
        nodes[i].depth = (u8)d;
        bl_count[d]++;
    }

    if (overflow > 0) {
        /* Kraft toplamını (sum 2^(max_bits - l_i) == 2^max_bits) dengele */
        for (;;) {
            u32 kraft = 0;
            for (u32 b = 1; b <= max_bits; b++) {
                kraft += bl_count[b] << (max_bits - b);
            }
            if (kraft <= (1u << max_bits)) break;

            /* max_bits'ten küçük en derin düğümün uzunluğunu 1 artır */
            for (i32 b = (i32)max_bits - 1; b >= 1; b--) {
                if (bl_count[b] > 0) {
                    bl_count[b]--;
                    bl_count[b + 1]++;
                    break;
                }
            }
        }
        /* Düğümlere (en düşük frekanslı en uzun kodu alır) yeni uzunlukları ata */
        u32 idx = 0;
        for (u32 b = max_bits; b >= 1; b--) {
            for (u32 k = 0; k < bl_count[b]; k++) {
                out_lens[active_syms[idx++]] = (u8)b;
            }
        }
    } else {
        for (u32 i = 0; i < leaf_cnt; i++) {
            out_lens[nodes[i].sym] = nodes[i].depth;
        }
    }

    /* RFC 1951 §3.2.2 Kanonik kod ataması */
    u16 next_code[16];
    u16 code = 0;
    bl_count[0] = 0;
    for (u32 bits = 1; bits <= max_bits; bits++) {
        code = (u16)((code + bl_count[bits - 1]) << 1u);
        next_code[bits] = code;
    }

    for (u32 sym = 0; sym < num_syms; sym++) {
        u8 l = out_lens[sym];
        if (l != 0) {
            out_codes[sym] = bit_reverse16(next_code[l]++, l);
        }
    }
}

u8* png_deflate_compress(const u8* src, usize src_len, usize* out_len, Arena* arena) {
    if (!out_len || !arena) return NULL;
    *out_len = 0;

    usize mark = arena_mark(arena);

    /* LZ77 Hash Zinciri (32K pencere, 32K hash tablosu) */
    const u32 HASH_SIZE = 32768u;
    const u32 HASH_MASK = HASH_SIZE - 1u;
    const u32 WIN_SIZE  = 32768u;
    const u32 WIN_MASK  = WIN_SIZE - 1u;

    i32* head = ARENA_ARRAY(arena, i32, HASH_SIZE);
    i32* prev = ARENA_ARRAY(arena, i32, WIN_SIZE);
    for (u32 i = 0; i < HASH_SIZE; i++) head[i] = -1;

    LzToken* tokens = ARENA_ARRAY(arena, LzToken, src_len + 1u);
    usize tok_cnt = 0;

    u32 lit_freq[288];
    u32 dist_freq[32];
    mem_zero(lit_freq, sizeof(lit_freq));
    mem_zero(dist_freq, sizeof(dist_freq));

    usize pos = 0;
    while (pos < src_len) {
        u32 best_len  = 0;
        u32 best_dist = 0;

        if (pos + 3 <= src_len) {
            u32 h = (((u32)src[pos] << 10u) ^ ((u32)src[pos + 1] << 5u) ^ (u32)src[pos + 2]) & HASH_MASK;
            i32 match_pos = head[h];
            head[h] = (i32)pos;
            prev[pos & WIN_MASK] = match_pos;

            u32 max_len = (u32)MIN((usize)258, src_len - pos);
            u32 chain_limit = 48u;

            while (match_pos >= 0 && chain_limit-- > 0) {
                usize dist = pos - (usize)match_pos;
                if (dist > WIN_SIZE) break;

                if (src[(usize)match_pos + best_len] == src[pos + best_len] &&
                    src[(usize)match_pos] == src[pos] &&
                    src[(usize)match_pos + 1] == src[pos + 1]) {
                    u32 l = 2u;
                    while (l < max_len && src[(usize)match_pos + l] == src[pos + l]) {
                        l++;
                    }
                    if (l >= 3u && l > best_len) {
                        best_len  = l;
                        best_dist = (u32)dist;
                        if (l == max_len) break;
                    }
                }
                match_pos = prev[(usize)match_pos & WIN_MASK];
            }
        }

        if (best_len >= 3u) {
            tokens[tok_cnt].lit_len = (u16)best_len;
            tokens[tok_cnt].dist    = (u16)best_dist;
            tok_cnt++;

            u32 eb, ev;
            u32 lcode = len_to_code(best_len, &eb, &ev);
            u32 dcode = dist_to_code(best_dist, &eb, &ev);
            lit_freq[lcode]++;
            dist_freq[dcode]++;

            /* Atlanan baytları hash zincirine ekle */
            for (u32 k = 1; k < best_len; k++) {
                usize p = pos + k;
                if (p + 3 <= src_len) {
                    u32 h = (((u32)src[p] << 10u) ^ ((u32)src[p + 1] << 5u) ^ (u32)src[p + 2]) & HASH_MASK;
                    prev[p & WIN_MASK] = head[h];
                    head[h] = (i32)p;
                }
            }
            pos += best_len;
        } else {
            u8 lit = src[pos];
            tokens[tok_cnt].lit_len = (u16)lit;
            tokens[tok_cnt].dist    = 0;
            tok_cnt++;
            lit_freq[lit]++;
            pos++;
        }
    }
    lit_freq[256] = 1u; /* EOB */

    /* En az 1 mesafe sembolü bulunsun (dinamik tablo uyumluluğu için) */
    b32 any_dist = FALSE;
    for (u32 i = 0; i < 30; i++) {
        if (dist_freq[i] > 0) { any_dist = TRUE; break; }
    }
    if (!any_dist) dist_freq[0] = 1u;

    /* Sabit ve Dinamik Huffman kodlarını hazırla */
    u8  fix_lit_len[288], fix_dist_len[32];
    u16 fix_lit_code[288], fix_dist_code[32];
    u32 dummy_freq[288];
    for (u32 i = 0; i < 288; i++) dummy_freq[i] = 1;
    for (u32 i = 0;   i <= 143; i++) fix_lit_len[i] = 8;
    for (u32 i = 144; i <= 255; i++) fix_lit_len[i] = 9;
    for (u32 i = 256; i <= 279; i++) fix_lit_len[i] = 7;
    for (u32 i = 280; i <= 287; i++) fix_lit_len[i] = 8;
    for (u32 i = 0;   i < 32;   i++) fix_dist_len[i] = 5;

    /* Sabit kanonik kodları üret */
    {
        u16 next_c[10] = {0};
        u16 c = 0;
        u32 bl[10] = {0, 0, 0, 0, 0, 0, 0, 24, 152, 112};
        for (u32 b = 1; b <= 9; b++) {
            c = (u16)((c + bl[b - 1]) << 1u);
            next_c[b] = c;
        }
        for (u32 s = 0; s < 288; s++) {
            u8 l = fix_lit_len[s];
            fix_lit_code[s] = bit_reverse16(next_c[l]++, l);
        }
        for (u32 s = 0; s < 32; s++) {
            fix_dist_code[s] = bit_reverse16((u16)s, 5);
        }
    }

    u8  dyn_lit_len[288], dyn_dist_len[32];
    u16 dyn_lit_code[288], dyn_dist_code[32];
    build_canonical_codes(lit_freq,  286, 15, dyn_lit_len,  dyn_lit_code);
    build_canonical_codes(dist_freq,  30, 15, dyn_dist_len, dyn_dist_code);

    /* HLIT ve HDIST hesapla */
    u32 hlit = 286u;
    while (hlit > 257u && dyn_lit_len[hlit - 1] == 0) hlit--;
    u32 hdist = 30u;
    while (hdist > 1u && dyn_dist_len[hdist - 1] == 0) hdist--;

    /* Ağaç uzunluklarını (hlit + hdist) RLE kullanmadan doğrudan (0..15) kodla — basit, hatasız ve hızlı */
    u32 cl_freq[19];
    mem_zero(cl_freq, sizeof(cl_freq));
    for (u32 i = 0; i < hlit; i++)  cl_freq[dyn_lit_len[i]]++;
    for (u32 i = 0; i < hdist; i++) cl_freq[dyn_dist_len[i]]++;

    u8  cl_len[19];
    u16 cl_code[19];
    build_canonical_codes(cl_freq, 19, 7, cl_len, cl_code);

    u32 hclen = 19u;
    while (hclen > 4u && cl_len[CLEN_ORDER[hclen - 1]] == 0) hclen--;

    /* Bit maliyeti karşılaştırması: Sabit vs Dinamik */
    usize fix_bits = 3u;
    usize dyn_bits = 3u + 5u + 5u + 4u + (usize)hclen * 3u;
    for (u32 i = 0; i < hlit; i++)  dyn_bits += cl_len[dyn_lit_len[i]];
    for (u32 i = 0; i < hdist; i++) dyn_bits += cl_len[dyn_dist_len[i]];

    for (u32 s = 0; s < 286; s++) {
        if (lit_freq[s] > 0) {
            fix_bits += (usize)lit_freq[s] * fix_lit_len[s];
            dyn_bits += (usize)lit_freq[s] * dyn_lit_len[s];
        }
    }
    for (u32 s = 0; s < 30; s++) {
        if (dist_freq[s] > 0) {
            fix_bits += (usize)dist_freq[s] * fix_dist_len[s];
            dyn_bits += (usize)dist_freq[s] * dyn_dist_len[s];
        }
    }

    b32 use_dyn = (dyn_bits + 32u < fix_bits);

    /* Çıktı tamponunu ayırmak için geçici çalışma belleğini temizlemeden önce yaz */
    BitWriter bw;
    bw_init(&bw, arena, src_len / 2u + 128u);

    const u8*  use_lit_len   = use_dyn ? dyn_lit_len   : fix_lit_len;
    const u16* use_lit_code  = use_dyn ? dyn_lit_code  : fix_lit_code;
    const u8*  use_dist_len  = use_dyn ? dyn_dist_len  : fix_dist_len;
    const u16* use_dist_code = use_dyn ? dyn_dist_code : fix_dist_code;

    bw_put(&bw, 1u, 1u); /* BFINAL = 1 */
    if (use_dyn) {
        bw_put(&bw, 2u, 2u); /* BTYPE = 10 (Dinamik) */
        bw_put(&bw, hlit - 257u, 5u);
        bw_put(&bw, hdist - 1u,  5u);
        bw_put(&bw, hclen - 4u,  4u);
        for (u32 i = 0; i < hclen; i++) {
            bw_put(&bw, cl_len[CLEN_ORDER[i]], 3u);
        }
        for (u32 i = 0; i < hlit; i++) {
            u8 s = dyn_lit_len[i];
            bw_put(&bw, cl_code[s], cl_len[s]);
        }
        for (u32 i = 0; i < hdist; i++) {
            u8 s = dyn_dist_len[i];
            bw_put(&bw, cl_code[s], cl_len[s]);
        }
    } else {
        bw_put(&bw, 1u, 2u); /* BTYPE = 01 (Sabit) */
    }

    for (usize i = 0; i < tok_cnt; i++) {
        if (tokens[i].dist == 0) {
            u32 lit = tokens[i].lit_len;
            bw_put(&bw, use_lit_code[lit], use_lit_len[lit]);
        } else {
            u32 len  = tokens[i].lit_len;
            u32 dist = tokens[i].dist;
            u32 leb, lev, deb, dev;
            u32 lcode = len_to_code(len, &leb, &lev);
            u32 dcode = dist_to_code(dist, &deb, &dev);

            bw_put(&bw, use_lit_code[lcode], use_lit_len[lcode]);
            if (leb > 0) bw_put(&bw, lev, leb);

            bw_put(&bw, use_dist_code[dcode], use_dist_len[dcode]);
            if (deb > 0) bw_put(&bw, dev, deb);
        }
    }

    /* EOB (256) */
    bw_put(&bw, use_lit_code[256], use_lit_len[256]);
    bw_flush_byte(&bw);

    /* Sonucu mark noktasına sıkıca kopyala (geçici LZ77 tablolarını geri bırak) */
    usize final_len = bw.ob.len;
    u8* temp_ptr = bw.ob.data;
    arena_reset(arena, mark);
    u8* result = (u8*)arena_alloc(arena, final_len > 0 ? final_len : 1, 16);
    if (result != temp_ptr) {
        for (usize i = 0; i < final_len; i++) result[i] = temp_ptr[i];
    }

    *out_len = final_len;
    return result;
}

u8* png_zlib_compress(const u8* src, usize src_len, usize* out_len, Arena* arena) {
    if (!out_len || !arena) return NULL;
    usize mark = arena_mark(arena);

    usize def_len = 0;
    u8* def_data = png_deflate_compress(src, src_len, &def_len, arena);
    if (!def_data) {
        arena_reset(arena, mark);
        return NULL;
    }

    usize total = 2u + def_len + 4u;
    u8* zbuf = (u8*)arena_alloc(arena, total, 16);
    /* CMF = 0x78 (32K pencere, DEFLATE), FLG = 0x9C (varsayılan sıkıştırma, 0x789C % 31 == 0) */
    zbuf[0] = 0x78u;
    zbuf[1] = 0x9Cu;
    mem_copy(zbuf + 2, def_data, def_len);

    u32 adler = png_adler32(src, src_len);
    write_be32(zbuf + 2 + def_len, adler);

    *out_len = total;
    return zbuf;
}

/* ============================================================================
 * PNG FİLTRELERİ (None=0, Sub=1, Up=2, Avg=3, Paeth=4)
 * ============================================================================ */

static inline u8 paeth_predictor(i32 a, i32 b, i32 c) {
    i32 p  = a + b - c;
    i32 pa = p > a ? (p - a) : (a - p);
    i32 pb = p > b ? (p - b) : (b - p);
    i32 pc = p > c ? (p - c) : (c - p);
    if (pa <= pb && pa <= pc) return (u8)a;
    if (pb <= pc) return (u8)b;
    return (u8)c;
}

static void png_filter_row(u8 type, const u8* cur, const u8* prev, u8* out, usize row_bytes, usize bpp) {
    switch (type) {
        case 0: /* None */
            for (usize i = 0; i < row_bytes; i++) out[i] = cur[i];
            break;
        case 1: /* Sub */
            for (usize i = 0; i < row_bytes; i++) {
                u8 a = (i >= bpp) ? cur[i - bpp] : 0;
                out[i] = (u8)(cur[i] - a);
            }
            break;
        case 2: /* Up */
            for (usize i = 0; i < row_bytes; i++) {
                u8 b = prev ? prev[i] : 0;
                out[i] = (u8)(cur[i] - b);
            }
            break;
        case 3: /* Average */
            for (usize i = 0; i < row_bytes; i++) {
                u32 a = (i >= bpp) ? cur[i - bpp] : 0;
                u32 b = prev ? prev[i] : 0;
                out[i] = (u8)(cur[i] - (u8)((a + b) >> 1u));
            }
            break;
        case 4: /* Paeth */
            for (usize i = 0; i < row_bytes; i++) {
                i32 a = (i >= bpp) ? (i32)cur[i - bpp] : 0;
                i32 b = prev ? (i32)prev[i] : 0;
                i32 c = (prev && i >= bpp) ? (i32)prev[i - bpp] : 0;
                out[i] = (u8)(cur[i] - paeth_predictor(a, b, c));
            }
            break;
    }
}

static b32 png_unfilter_row(u8 type, const u8* in, const u8* prev, u8* out, usize row_bytes, usize bpp) {
    switch (type) {
        case 0: /* None */
            for (usize i = 0; i < row_bytes; i++) out[i] = in[i];
            return TRUE;
        case 1: /* Sub */
            for (usize i = 0; i < row_bytes; i++) {
                u8 a = (i >= bpp) ? out[i - bpp] : 0;
                out[i] = (u8)(in[i] + a);
            }
            return TRUE;
        case 2: /* Up */
            for (usize i = 0; i < row_bytes; i++) {
                u8 b = prev ? prev[i] : 0;
                out[i] = (u8)(in[i] + b);
            }
            return TRUE;
        case 3: /* Average */
            for (usize i = 0; i < row_bytes; i++) {
                u32 a = (i >= bpp) ? out[i - bpp] : 0;
                u32 b = prev ? prev[i] : 0;
                out[i] = (u8)(in[i] + (u8)((a + b) >> 1u));
            }
            return TRUE;
        case 4: /* Paeth */
            for (usize i = 0; i < row_bytes; i++) {
                i32 a = (i >= bpp) ? (i32)out[i - bpp] : 0;
                i32 b = prev ? (i32)prev[i] : 0;
                i32 c = (prev && i >= bpp) ? (i32)prev[i - bpp] : 0;
                out[i] = (u8)(in[i] + paeth_predictor(a, b, c));
            }
            return TRUE;
        default:
            return FALSE;
    }
}

/* ============================================================================
 * PNG KODLAYICI & ÇÖZÜCÜ
 * ============================================================================ */

static const u8 PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

static usize write_chunk(u8* dst, const char type[4], const u8* data, u32 len) {
    write_be32(dst, len);
    dst[4] = (u8)type[0];
    dst[5] = (u8)type[1];
    dst[6] = (u8)type[2];
    dst[7] = (u8)type[3];
    if (len > 0 && data) {
        mem_copy(dst + 8, data, len);
    }
    u32 crc = png_crc32(dst + 4, 4u + len);
    write_be32(dst + 8 + len, crc);
    return 12u + (usize)len;
}

u8* png_encode(const u32* rgba_px, u32 w, u32 h, usize* out_len, Arena* arena) {
    if (!rgba_px || w == 0 || h == 0 || !out_len || !arena) return NULL;
    *out_len = 0;

    usize mark = arena_mark(arena);
    usize row_bytes = (usize)w * 4u;
    usize raw_size  = (usize)h * (1u + row_bytes);

    u8* filtered = (u8*)arena_alloc(arena, raw_size, 16);
    u8* cand     = (u8*)arena_alloc(arena, row_bytes * 5u, 16);

    const u8* prev_row = NULL;
    for (u32 y = 0; y < h; y++) {
        const u8* cur_row = (const u8*)(rgba_px + (usize)y * w);
        u8* dst_line = filtered + (usize)y * (1u + row_bytes);

        u8  best_type = 0;
        u64 best_score = ~0ULL;

        for (u8 f = 0; f < 5; f++) {
            u8* fbuf = cand + (usize)f * row_bytes;
            png_filter_row(f, cur_row, prev_row, fbuf, row_bytes, 4);

            /* Minimum mutlak toplam (sum of absolute differences) sezgiseli */
            u64 score = 0;
            for (usize i = 0; i < row_bytes; i++) {
                i8 sv = (i8)fbuf[i];
                score += (u64)(sv < 0 ? -sv : sv);
            }
            if (score < best_score) {
                best_score = score;
                best_type  = f;
            }
        }

        dst_line[0] = best_type;
        mem_copy(dst_line + 1, cand + (usize)best_type * row_bytes, row_bytes);
        prev_row = cur_row;
    }

    usize idat_len = 0;
    u8* idat_data = png_zlib_compress(filtered, raw_size, &idat_len, arena);
    if (!idat_data) {
        arena_reset(arena, mark);
        return NULL;
    }

    usize total_png = 8u + (12u + 13u) + (12u + idat_len) + 12u;
    u8* png_buf = (u8*)arena_alloc(arena, total_png, 16);

    usize off = 0;
    mem_copy(png_buf + off, PNG_SIG, 8);
    off += 8;

    u8 ihdr[13];
    write_be32(ihdr + 0, w);
    write_be32(ihdr + 4, h);
    ihdr[8]  = 8; /* bit depth = 8 */
    ihdr[9]  = 6; /* color type = 6 (RGBA) */
    ihdr[10] = 0; /* compression = 0 */
    ihdr[11] = 0; /* filter = 0 */
    ihdr[12] = 0; /* interlace = 0 */
    off += write_chunk(png_buf + off, "IHDR", ihdr, 13);
    off += write_chunk(png_buf + off, "IDAT", idat_data, (u32)idat_len);
    off += write_chunk(png_buf + off, "IEND", NULL, 0);

    /* Ara tamponları serbest bırakıp yalnızca nihai PNG dizisini Arena'da tut */
    arena_reset(arena, mark);
    u8* final_png = (u8*)arena_alloc(arena, off, 16);
    if (final_png != png_buf) {
        for (usize i = 0; i < off; i++) final_png[i] = png_buf[i];
    }

    *out_len = off;
    return final_png;
}

u32* png_decode(const u8* png_data, usize png_len, u32* out_w, u32* out_h, Arena* arena) {
    if (!png_data || png_len < 33 || !out_w || !out_h || !arena) return NULL;
    for (usize i = 0; i < 8; i++) {
        if (png_data[i] != PNG_SIG[i]) return NULL;
    }

    usize mark = arena_mark(arena);
    u32 w = 0, h = 0;
    u8  bit_depth = 0, color_type = 0, interlace = 0;
    b32 got_ihdr = FALSE, got_iend = FALSE;

    u32 palette[256];
    u32 pal_count = 0;
    for (u32 i = 0; i < 256; i++) palette[i] = color_pack(0, 0, 0, 255);

    OutBuf idat_stream;
    outbuf_init(&idat_stream, arena, png_len);

    usize pos = 8;
    while (pos + 12 <= png_len) {
        u32 clen = read_be32(png_data + pos);
        const u8* ctype = png_data + pos + 4;
        if (pos + 12u + (usize)clen > png_len) {
            arena_reset(arena, mark);
            return NULL;
        }
        const u8* cdata = png_data + pos + 8;
        u32 expected_crc = read_be32(cdata + clen);
        u32 actual_crc   = png_crc32(ctype, 4u + clen);
        if (expected_crc != actual_crc) {
            arena_reset(arena, mark);
            return NULL;
        }

        if (ctype[0] == 'I' && ctype[1] == 'H' && ctype[2] == 'D' && ctype[3] == 'R') {
            if (clen != 13 || got_ihdr) { arena_reset(arena, mark); return NULL; }
            w          = read_be32(cdata + 0);
            h          = read_be32(cdata + 4);
            bit_depth  = cdata[8];
            color_type = cdata[9];
            u8 comp    = cdata[10];
            u8 filt    = cdata[11];
            interlace  = cdata[12];

            if (w == 0 || h == 0 || bit_depth != 8 || comp != 0 || filt != 0 || interlace != 0) {
                arena_reset(arena, mark);
                return NULL;
            }
            if (color_type != 0 && color_type != 2 && color_type != 3 &&
                color_type != 4 && color_type != 6) {
                arena_reset(arena, mark);
                return NULL;
            }
            got_ihdr = TRUE;
        } else if (ctype[0] == 'P' && ctype[1] == 'L' && ctype[2] == 'T' && ctype[3] == 'E') {
            if (clen % 3 != 0 || clen > 768) { arena_reset(arena, mark); return NULL; }
            pal_count = clen / 3u;
            for (u32 i = 0; i < pal_count; i++) {
                u8 r = cdata[i * 3u + 0];
                u8 g = cdata[i * 3u + 1];
                u8 b = cdata[i * 3u + 2];
                palette[i] = (palette[i] & 0xFF000000u) | color_pack(r, g, b, 0);
            }
        } else if (ctype[0] == 't' && ctype[1] == 'R' && ctype[2] == 'N' && ctype[3] == 'S') {
            if (color_type == 3) {
                for (u32 i = 0; i < clen && i < 256u; i++) {
                    palette[i] = (palette[i] & 0x00FFFFFFu) | ((u32)cdata[i] << 24u);
                }
            }
        } else if (ctype[0] == 'I' && ctype[1] == 'D' && ctype[2] == 'A' && ctype[3] == 'T') {
            outbuf_reserve(&idat_stream, clen);
            mem_copy(idat_stream.data + idat_stream.len, cdata, clen);
            idat_stream.len += clen;
        } else if (ctype[0] == 'I' && ctype[1] == 'E' && ctype[2] == 'N' && ctype[3] == 'D') {
            got_iend = TRUE;
            break;
        }

        pos += 12u + (usize)clen;
    }

    if (!got_ihdr || !got_iend || idat_stream.len == 0) {
        arena_reset(arena, mark);
        return NULL;
    }
    if (color_type == 3 && pal_count == 0) {
        arena_reset(arena, mark);
        return NULL;
    }

    usize bpp = 4;
    if (color_type == 0 || color_type == 3) bpp = 1;
    else if (color_type == 4) bpp = 2;
    else if (color_type == 2) bpp = 3;
    else if (color_type == 6) bpp = 4;

    usize row_bytes = (usize)w * bpp;
    usize expected_raw = (usize)h * (1u + row_bytes);

    usize raw_len = 0;
    u8* raw = png_zlib_decompress(idat_stream.data, idat_stream.len, &raw_len, arena);
    if (!raw || raw_len < expected_raw) {
        arena_reset(arena, mark);
        return NULL;
    }

    u8* Recon_a = (u8*)arena_alloc(arena, row_bytes, 16);
    u8* Recon_b = (u8*)arena_alloc(arena, row_bytes, 16);
    u32* out_px = (u32*)arena_alloc(arena, (usize)w * (usize)h * sizeof(u32), 16);

    const u8* prev_recon = NULL;
    for (u32 y = 0; y < h; y++) {
        const u8* src_line = raw + (usize)y * (1u + row_bytes);
        u8 ftype = src_line[0];
        u8* cur_recon = (y & 1u) ? Recon_b : Recon_a;

        if (!png_unfilter_row(ftype, src_line + 1, prev_recon, cur_recon, row_bytes, bpp)) {
            arena_reset(arena, mark);
            return NULL;
        }
        prev_recon = cur_recon;

        u32* dst_row = out_px + (usize)y * w;
        if (color_type == 6) {
            for (u32 x = 0; x < w; x++) {
                const u8* p = cur_recon + (usize)x * 4u;
                dst_row[x] = color_pack(p[0], p[1], p[2], p[3]);
            }
        } else if (color_type == 2) {
            for (u32 x = 0; x < w; x++) {
                const u8* p = cur_recon + (usize)x * 3u;
                dst_row[x] = color_pack(p[0], p[1], p[2], 255u);
            }
        } else if (color_type == 0) {
            for (u32 x = 0; x < w; x++) {
                u8 g = cur_recon[x];
                dst_row[x] = color_pack(g, g, g, 255u);
            }
        } else if (color_type == 4) {
            for (u32 x = 0; x < w; x++) {
                u8 g = cur_recon[x * 2u + 0];
                u8 a = cur_recon[x * 2u + 1];
                dst_row[x] = color_pack(g, g, g, a);
            }
        } else if (color_type == 3) {
            for (u32 x = 0; x < w; x++) {
                u8 idx = cur_recon[x];
                dst_row[x] = palette[idx];
            }
        }
    }

    /* Ara tamponları serbest bırakıp yalnızca piksel dizisini Arena'da tut */
    usize px_bytes = (usize)w * (usize)h * sizeof(u32);
    arena_reset(arena, mark);
    u32* final_px = (u32*)arena_alloc(arena, px_bytes, 16);
    if (final_px != out_px) {
        for (usize i = 0; i < (usize)w * (usize)h; i++) final_px[i] = out_px[i];
    }

    *out_w = w;
    *out_h = h;
    return final_px;
}
