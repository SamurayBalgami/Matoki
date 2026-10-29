/* font/ttf.c — Sıfırdan TrueType (.ttf) ikili ayrıştırıcı.
 * Tüm okumalar big-endian ve u64 taşma korumalı sınır denetimlidir.
 * Basit ve bileşik (dönüşüm matrisli) glifleri, cmap biçim 4/12 ve kern biçim 0'ı destekler.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/ttf.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "font/ttf.h"
#include "core/math.h"

#define TTF_TAG(a, b, c, d) \
    (((u32)(u8)(a) << 24) | ((u32)(u8)(b) << 16) | ((u32)(u8)(c) << 8) | (u32)(u8)(d))

/* Basit glif bayrakları */
#define GLYF_ON_CURVE  0x01u
#define GLYF_X_SHORT   0x02u
#define GLYF_Y_SHORT   0x04u
#define GLYF_REPEAT    0x08u
#define GLYF_X_SAME    0x10u
#define GLYF_Y_SAME    0x20u

/* Bileşik glif bayrakları */
#define COMP_ARG_WORDS        0x0001u
#define COMP_ARGS_ARE_XY      0x0002u
#define COMP_HAVE_SCALE       0x0008u
#define COMP_MORE_COMPONENTS  0x0020u
#define COMP_HAVE_XY_SCALE    0x0040u
#define COMP_HAVE_TWO_BY_TWO  0x0080u
#define COMP_SCALED_OFFSET    0x0800u
#define COMP_UNSCALED_OFFSET  0x1000u

static inline b32 check_range(u32 total_len, u64 off, u64 size) {
    return off <= (u64)total_len && size <= (u64)total_len - off;
}

static inline u16 rd_u16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

static inline i16 rd_i16(const u8* p) {
    return (i16)rd_u16(p);
}

static inline u32 rd_u32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static b32 parse_cmap(TtfFont* f) {
    if (f->cmap_len < 4) return FALSE;
    const u8* base = f->data + f->cmap_off;
    u16 num_tables = rd_u16(base + 2);
    if (!check_range(f->cmap_len, 4u, (u64)num_tables * 8u)) return FALSE;

    for (u16 i = 0; i < num_tables; i++) {
        const u8* rec = base + 4u + (u32)i * 8u;
        u16 plat = rd_u16(rec + 0);
        u16 enc  = rd_u16(rec + 2);
        u32 sub  = rd_u32(rec + 4);
        UNUSED(enc);

        if (!check_range(f->cmap_len, sub, 4u)) return FALSE;
        u16 fmt = rd_u16(base + sub);

        if (fmt == 4u) {
            u16 sub_len = rd_u16(base + sub + 2u);
            if (sub_len < 16u || !check_range(f->cmap_len, sub, sub_len)) return FALSE;
            u16 seg_x2 = rd_u16(base + sub + 6u);
            if ((seg_x2 & 1u) != 0u || seg_x2 < 2u) return FALSE;
            u32 seg_count = (u32)(seg_x2 >> 1);
            if (!check_range(sub_len, 16u, (u64)seg_count * 8u)) return FALSE;
            /* Platform 0 (Unicode) veya 3 (Windows) tercih edilir, yoksa ilk geçerli biçim 4 alınır */
            if (f->cmap4_off == 0u || plat == 0u || plat == 3u) {
                f->cmap4_off = f->cmap_off + sub;
                f->cmap4_len = sub_len;
            }
        } else if (fmt == 12u) {
            if (!check_range(f->cmap_len, sub, 16u)) return FALSE;
            u32 sub_len  = rd_u32(base + sub + 4u);
            u32 n_groups = rd_u32(base + sub + 12u);
            if (sub_len < 16u || !check_range(f->cmap_len, sub, sub_len)) return FALSE;
            if (!check_range(sub_len, 16u, (u64)n_groups * 12u)) return FALSE;
            if (f->cmap12_off == 0u || plat == 0u || plat == 3u) {
                f->cmap12_off = f->cmap_off + sub;
                f->cmap12_len = sub_len;
            }
        }
    }

    return (f->cmap4_off != 0u || f->cmap12_off != 0u) ? TRUE : FALSE;
}

static b32 parse_kern(TtfFont* f) {
    if (f->kern_off == 0u || f->kern_len == 0u) return TRUE;
    if (f->kern_len < 4u) return FALSE;

    const u8* base = f->data + f->kern_off;
    u16 ver = rd_u16(base + 0);
    if (ver != 0u) {
        /* Apple biçimi (version 1.0 = 0x00010000): 8 bayt başlık */
        if (f->kern_len < 8u || rd_u32(base) != 0x00010000u) return FALSE;
        return TRUE;
    }

    u16 n_tables = rd_u16(base + 2);
    u32 pos = 4u;
    for (u16 t = 0; t < n_tables; t++) {
        if (!check_range(f->kern_len, pos, 6u)) return FALSE;
        u16 sub_ver = rd_u16(base + pos + 0);
        u16 sub_len = rd_u16(base + pos + 2);
        u16 cov     = rd_u16(base + pos + 4);
        UNUSED(sub_ver);

        u8 fmt   = (u8)(cov >> 8);
        u8 flags = (u8)(cov & 0xFFu);

        if (fmt == 0u) {
            if (!check_range(f->kern_len, pos, 14u)) return FALSE;
            u16 n_pairs = rd_u16(base + pos + 6u);
            u64 pairs_bytes = (u64)n_pairs * 6u;
            if (!check_range(f->kern_len, pos + 14u, pairs_bytes)) return FALSE;
            if (sub_len < 14u) return FALSE;

            /* Yatay kerning (bit 0 = 1, bit 1 = 0 minimum değil, bit 2 = 0 cross-stream değil) */
            if ((flags & 0x07u) == 0x01u && f->kern0_pairs_off == 0u) {
                f->kern0_pairs_off  = f->kern_off + pos + 14u;
                f->kern0_pair_count = n_pairs;
                b32 sorted = TRUE;
                u32 prev_key = 0u;
                const u8* p = f->data + f->kern0_pairs_off;
                for (u16 k = 0; k < n_pairs; k++) {
                    u32 key = rd_u32(p + (u32)k * 6u);
                    if (k > 0u && key < prev_key) {
                        sorted = FALSE;
                        break;
                    }
                    prev_key = key;
                }
                f->kern0_sorted = sorted;
            }
            u64 actual_step = MAX((u64)sub_len, 14u + pairs_bytes);
            if (!check_range(f->kern_len, pos, actual_step)) return FALSE;
            pos += (u32)actual_step;
        } else {
            if (sub_len < 6u || !check_range(f->kern_len, pos, sub_len)) return FALSE;
            pos += sub_len;
        }
    }
    return TRUE;
}

static b32 glyph_loca_range(const TtfFont* f, u16 glyph_index, u32* out_off0, u32* out_off1) {
    if (glyph_index >= f->num_glyphs) return FALSE;
    const u8* loca = f->data + f->loca_off;
    u32 off0, off1;
    if (f->index_to_loc_format == 0) {
        u32 idx = (u32)glyph_index * 2u;
        if (!check_range(f->loca_len, idx, 4u)) return FALSE;
        off0 = (u32)rd_u16(loca + idx) * 2u;
        off1 = (u32)rd_u16(loca + idx + 2u) * 2u;
    } else {
        u32 idx = (u32)glyph_index * 4u;
        if (!check_range(f->loca_len, idx, 8u)) return FALSE;
        off0 = rd_u32(loca + idx);
        off1 = rd_u32(loca + idx + 4u);
    }
    if (off0 > off1 || off1 > f->glyf_len) return FALSE;
    *out_off0 = off0;
    *out_off1 = off1;
    return TRUE;
}

b32 ttf_parse(const u8* data, u32 len, TtfFont* out) {
    if (!data || !out || len < 12u) return FALSE;
    for (usize i = 0; i < sizeof(TtfFont); i++) ((u8*)out)[i] = 0;

    out->data = data;
    out->len  = len;

    u32 sfnt = rd_u32(data);
    if (sfnt != 0x00010000u && sfnt != TTF_TAG('t', 'r', 'u', 'e')) return FALSE;

    u16 num_tables = rd_u16(data + 4);
    if (num_tables < 7u || num_tables > 256u) return FALSE;
    if (!check_range(len, 12u, (u64)num_tables * 16u)) return FALSE;

    for (u16 i = 0; i < num_tables; i++) {
        const u8* rec = data + 12u + (u32)i * 16u;
        u32 tag  = rd_u32(rec + 0);
        u32 off  = rd_u32(rec + 8);
        u32 tlen = rd_u32(rec + 12);
        if (!check_range(len, off, tlen)) return FALSE;

        if      (tag == TTF_TAG('h', 'e', 'a', 'd')) { out->head_off = off; out->head_len = tlen; }
        else if (tag == TTF_TAG('h', 'h', 'e', 'a')) { out->hhea_off = off; out->hhea_len = tlen; }
        else if (tag == TTF_TAG('h', 'm', 't', 'x')) { out->hmtx_off = off; out->hmtx_len = tlen; }
        else if (tag == TTF_TAG('m', 'a', 'x', 'p')) { out->maxp_off = off; out->maxp_len = tlen; }
        else if (tag == TTF_TAG('l', 'o', 'c', 'a')) { out->loca_off = off; out->loca_len = tlen; }
        else if (tag == TTF_TAG('g', 'l', 'y', 'f')) { out->glyf_off = off; out->glyf_len = tlen; }
        else if (tag == TTF_TAG('c', 'm', 'a', 'p')) { out->cmap_off = off; out->cmap_len = tlen; }
        else if (tag == TTF_TAG('k', 'e', 'r', 'n')) { out->kern_off = off; out->kern_len = tlen; }
    }

    if (out->head_len < 54u || out->hhea_len < 36u || out->maxp_len < 6u ||
        out->hmtx_len == 0u || out->loca_len == 0u || out->glyf_len == 0u || out->cmap_len < 4u) {
        return FALSE;
    }

    const u8* head = data + out->head_off;
    u32 magic = rd_u32(head + 12);
    if (magic != 0x5F0F3CF5u) return FALSE;

    out->units_per_em = rd_u16(head + 18);
    if (out->units_per_em < 16u || out->units_per_em > 16384u) return FALSE;

    out->bbox_xmin = rd_i16(head + 36);
    out->bbox_ymin = rd_i16(head + 38);
    out->bbox_xmax = rd_i16(head + 40);
    out->bbox_ymax = rd_i16(head + 42);
    out->index_to_loc_format = rd_i16(head + 50);
    if (out->index_to_loc_format != 0 && out->index_to_loc_format != 1) return FALSE;

    const u8* maxp = data + out->maxp_off;
    out->num_glyphs = rd_u16(maxp + 4);
    if (out->num_glyphs == 0u) return FALSE;

    const u8* hhea = data + out->hhea_off;
    out->ascent       = rd_i16(hhea + 4);
    out->descent      = rd_i16(hhea + 6);
    out->line_gap     = rd_i16(hhea + 8);
    out->num_hmetrics = rd_u16(hhea + 34);
    if (out->num_hmetrics == 0u || out->num_hmetrics > out->num_glyphs) return FALSE;

    u64 req_hmtx = (u64)out->num_hmetrics * 4u + (u64)(out->num_glyphs - out->num_hmetrics) * 2u;
    if (req_hmtx > (u64)out->hmtx_len) return FALSE;

    u64 req_loca = (u64)(out->num_glyphs + 1u) * (out->index_to_loc_format == 0 ? 2u : 4u);
    if (req_loca > (u64)out->loca_len) return FALSE;

    /* Tüm loca girişlerinin monoton ve glyf sınırları içinde olduğunu doğrula */
    for (u16 g = 0; g < out->num_glyphs; g++) {
        u32 o0, o1;
        if (!glyph_loca_range(out, g, &o0, &o1)) return FALSE;
    }

    if (!parse_cmap(out)) return FALSE;
    if (!parse_kern(out)) return FALSE;

    return TRUE;
}

u16 ttf_cmap4_lookup(const TtfFont* font, u32 codepoint) {
    if (!font || font->cmap4_off == 0u || codepoint > 0xFFFFu) return 0u;
    const u8* sub = font->data + font->cmap4_off;
    u32 sub_len   = font->cmap4_len;
    u32 seg_count = (u32)(rd_u16(sub + 6u) >> 1);

    const u8* end_counts   = sub + 14u;
    const u8* start_counts = sub + 16u + seg_count * 2u;
    const u8* id_deltas    = sub + 16u + seg_count * 4u;
    const u8* id_range_off = sub + 16u + seg_count * 6u;

    /* İkili arama (sıralı değilse doğrusal yedek) */
    u32 lo = 0u, hi = seg_count;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        u16 end_c = rd_u16(end_counts + mid * 2u);
        if ((u32)end_c < codepoint) lo = mid + 1u;
        else hi = mid;
    }

    for (u32 pass = 0; pass < 2; pass++) {
        u32 start_i = (pass == 0) ? lo : 0u;
        u32 end_i   = (pass == 0) ? MIN(lo + 1u, seg_count) : seg_count;
        for (u32 i = start_i; i < end_i; i++) {
            u16 end_c   = rd_u16(end_counts + i * 2u);
            u16 start_c = rd_u16(start_counts + i * 2u);
            if (codepoint >= (u32)start_c && codepoint <= (u32)end_c) {
                if (codepoint == 0xFFFFu && start_c == 0xFFFFu && end_c == 0xFFFFu) return 0u;
                u16 delta = rd_u16(id_deltas + i * 2u);
                u16 roff  = rd_u16(id_range_off + i * 2u);
                u16 gid   = 0u;
                if (roff == 0u) {
                    gid = (u16)((codepoint + (u32)delta) & 0xFFFFu);
                } else {
                    u64 roff_pos = 16u + (u64)seg_count * 6u + (u64)i * 2u;
                    u64 idx_off  = roff_pos + (u64)roff + (u64)(codepoint - (u32)start_c) * 2u;
                    if (!check_range(sub_len, idx_off, 2u)) return 0u;
                    u16 raw = rd_u16(sub + idx_off);
                    if (raw == 0u) return 0u;
                    gid = (u16)(((u32)raw + (u32)delta) & 0xFFFFu);
                }
                return (gid < font->num_glyphs) ? gid : 0u;
            }
        }
        if (pass == 0 && lo < seg_count &&
            codepoint < (u32)rd_u16(start_counts + lo * 2u) &&
            (lo == 0u || codepoint > (u32)rd_u16(end_counts + (lo - 1u) * 2u))) {
            break;
        }
    }
    return 0u;
}

u16 ttf_cmap12_lookup(const TtfFont* font, u32 codepoint) {
    if (!font || font->cmap12_off == 0u) return 0u;
    const u8* sub  = font->data + font->cmap12_off;
    u32 n_groups   = rd_u32(sub + 12u);
    const u8* grps = sub + 16u;

    u32 lo = 0u, hi = n_groups;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        const u8* g = grps + mid * 12u;
        u32 start_c = rd_u32(g + 0u);
        u32 end_c   = rd_u32(g + 4u);
        if (codepoint < start_c) {
            hi = mid;
        } else if (codepoint > end_c) {
            lo = mid + 1u;
        } else {
            u32 start_gid = rd_u32(g + 8u);
            u64 gid = (u64)start_gid + (u64)(codepoint - start_c);
            return (gid < (u64)font->num_glyphs) ? (u16)gid : 0u;
        }
    }
    return 0u;
}

u16 ttf_glyph_index(const TtfFont* font, u32 codepoint) {
    if (!font) return 0u;
    if (codepoint <= 0xFFFFu && font->cmap4_off != 0u) {
        u16 gid = ttf_cmap4_lookup(font, codepoint);
        if (gid != 0u) return gid;
    }
    if (font->cmap12_off != 0u) {
        return ttf_cmap12_lookup(font, codepoint);
    }
    return 0u;
}

b32 ttf_glyph_metrics(const TtfFont* font, u16 glyph_index, u16* out_advance, i16* out_lsb) {
    if (!font || glyph_index >= font->num_glyphs) return FALSE;
    const u8* hmtx = font->data + font->hmtx_off;
    u16 adv;
    i16 lsb;
    if (glyph_index < font->num_hmetrics) {
        u32 off = (u32)glyph_index * 4u;
        adv = rd_u16(hmtx + off);
        lsb = rd_i16(hmtx + off + 2u);
    } else {
        u32 last_off = (u32)(font->num_hmetrics - 1u) * 4u;
        u32 lsb_off  = (u32)font->num_hmetrics * 4u + (u32)(glyph_index - font->num_hmetrics) * 2u;
        adv = rd_u16(hmtx + last_off);
        lsb = rd_i16(hmtx + lsb_off);
    }
    if (out_advance) *out_advance = adv;
    if (out_lsb)     *out_lsb     = lsb;
    return TRUE;
}

i16 ttf_kern_advance(const TtfFont* font, u16 left_glyph, u16 right_glyph) {
    if (!font || font->kern0_pairs_off == 0u || font->kern0_pair_count == 0u) return 0;
    const u8* pairs = font->data + font->kern0_pairs_off;
    u32 target = ((u32)left_glyph << 16) | (u32)right_glyph;

    if (font->kern0_sorted) {
        u32 lo = 0u, hi = font->kern0_pair_count;
        while (lo < hi) {
            u32 mid = (lo + hi) >> 1;
            const u8* p = pairs + mid * 6u;
            u32 key = rd_u32(p);
            if (key < target) lo = mid + 1u;
            else if (key > target) hi = mid;
            else return rd_i16(p + 4u);
        }
        return 0;
    }

    for (u16 i = 0; i < font->kern0_pair_count; i++) {
        const u8* p = pairs + (u32)i * 6u;
        if (rd_u32(p) == target) return rd_i16(p + 4u);
    }
    return 0;
}

/* Basit veya bileşik bir glifin toplam kontur ve nokta sayısını sınır denetimli hesaplar */
/* budget: toplam ziyaret edilen glif düğümü sınırı. Derinlik (16) × bileşen (256) sınırları tek başına yetmez:
 * kendine başvuran çok bileşenli bir glif 256^16 çağrıya şişebilirdi (bozuk fontla kilitlenme). */
static b32 count_glyph_geom(const TtfFont* f, u16 gid, u32 depth, u32* io_contours, u32* io_points, u32* budget) {
    if (depth > TTF_MAX_COMPOSITE_DEPTH) return FALSE;
    if (*budget == 0u) return FALSE;
    (*budget)--;
    u32 off0, off1;
    if (!glyph_loca_range(f, gid, &off0, &off1)) return FALSE;
    if (off0 == off1) return TRUE; /* boş glif */

    u32 glen = off1 - off0;
    if (glen < 10u) return FALSE;
    const u8* gdata = f->data + f->glyf_off + off0;
    i16 n_contours = rd_i16(gdata);

    if (n_contours == 0) return TRUE;

    if (n_contours > 0) {
        u32 nc = (u32)n_contours;
        if (!check_range(glen, 10u, (u64)nc * 2u + 2u)) return FALSE;
        u16 prev_end = 0u;
        for (u32 c = 0; c < nc; c++) {
            u16 end_pt = rd_u16(gdata + 10u + c * 2u);
            if (c > 0u && end_pt <= prev_end) return FALSE;
            prev_end = end_pt;
        }
        u32 n_pts = (u32)prev_end + 1u;
        u16 ins_len = rd_u16(gdata + 10u + nc * 2u);
        u32 pos = 12u + nc * 2u + (u32)ins_len;
        if (pos > glen) return FALSE;

        /* Bayrakları ve koordinat dizisi uzunluğunu doğrula */
        u32 x_bytes = 0u, y_bytes = 0u;
        u32 pt_idx = 0u;
        while (pt_idx < n_pts) {
            if (pos >= glen) return FALSE;
            u8 fl = gdata[pos++];
            u32 rep = 1u;
            if (fl & GLYF_REPEAT) {
                if (pos >= glen) return FALSE;
                rep += (u32)gdata[pos++];
            }
            if (pt_idx + rep > n_pts) return FALSE;
            if (fl & GLYF_X_SHORT) x_bytes += rep;
            else if (!(fl & GLYF_X_SAME)) x_bytes += rep * 2u;

            if (fl & GLYF_Y_SHORT) y_bytes += rep;
            else if (!(fl & GLYF_Y_SAME)) y_bytes += rep * 2u;

            pt_idx += rep;
        }
        if (!check_range(glen, pos, (u64)x_bytes + (u64)y_bytes)) return FALSE;

        *io_contours += nc;
        *io_points   += n_pts;
        if (*io_contours > TTF_MAX_GLYPH_CONTOURS || *io_points > TTF_MAX_GLYPH_POINTS) return FALSE;
        return TRUE;
    }

    /* Bileşik glif (n_contours < 0) */
    u32 pos = 10u;
    u32 comp_count = 0u;
    while (1) {
        if (++comp_count > 256u) return FALSE;
        if (!check_range(glen, pos, 4u)) return FALSE;
        u16 flags     = rd_u16(gdata + pos);
        u16 child_gid = rd_u16(gdata + pos + 2u);
        pos += 4u;

        u32 arg_bytes = (flags & COMP_ARG_WORDS) ? 4u : 2u;
        u32 mat_bytes = 0u;
        if (flags & COMP_HAVE_SCALE)          mat_bytes = 2u;
        else if (flags & COMP_HAVE_XY_SCALE)  mat_bytes = 4u;
        else if (flags & COMP_HAVE_TWO_BY_TWO) mat_bytes = 8u;

        if (!check_range(glen, pos, arg_bytes + mat_bytes)) return FALSE;
        pos += arg_bytes + mat_bytes;

        if (!count_glyph_geom(f, child_gid, depth + 1u, io_contours, io_points, budget)) return FALSE;

        if (!(flags & COMP_MORE_COMPONENTS)) break;
    }
    return TRUE;
}

/* Doğrulanmış glif verisini out->contour_ends ve out->points içine doldurur */
static b32 fill_glyph_geom(const TtfFont* f, u16 gid, u32 depth,
                           TtfGlyph* out, u32* io_c, u32* io_p) {
    if (depth > TTF_MAX_COMPOSITE_DEPTH) return FALSE;
    u32 off0, off1;
    if (!glyph_loca_range(f, gid, &off0, &off1)) return FALSE;
    if (off0 == off1) return TRUE;

    const u8* gdata = f->data + f->glyf_off + off0;
    i16 n_contours = rd_i16(gdata);
    if (n_contours == 0) return TRUE;

    if (n_contours > 0) {
        u32 nc      = (u32)n_contours;
        u32 base_p  = *io_p;
        u32 base_c  = *io_c;
        for (u32 c = 0; c < nc; c++) {
            u16 end_pt = rd_u16(gdata + 10u + c * 2u);
            out->contour_ends[base_c + c] = (u16)(base_p + (u32)end_pt);
        }
        u32 n_pts   = (u32)rd_u16(gdata + 10u + (nc - 1u) * 2u) + 1u;
        u16 ins_len = rd_u16(gdata + 10u + nc * 2u);
        u32 pos     = 12u + nc * 2u + (u32)ins_len;

        /* Bayrakları on_curve alanına geçici olarak tam bayrak baytı olarak aç */
        u32 pt_idx = 0u;
        while (pt_idx < n_pts) {
            u8 fl = gdata[pos++];
            u32 rep = 1u;
            if (fl & GLYF_REPEAT) rep += (u32)gdata[pos++];
            for (u32 r = 0; r < rep; r++) {
                out->points[base_p + pt_idx + r].on_curve = fl;
            }
            pt_idx += rep;
        }

        /* X koordinatları */
        i32 x = 0;
        for (u32 i = 0; i < n_pts; i++) {
            u8 fl = out->points[base_p + i].on_curve;
            if (fl & GLYF_X_SHORT) {
                i32 dx = (i32)gdata[pos++];
                x += (fl & GLYF_X_SAME) ? dx : -dx;
            } else if (!(fl & GLYF_X_SAME)) {
                x += (i32)rd_i16(gdata + pos);
                pos += 2u;
            }
            out->points[base_p + i].x = (f32)x;
        }

        /* Y koordinatları ve on_curve bitini sadeleştirme */
        i32 y = 0;
        for (u32 i = 0; i < n_pts; i++) {
            u8 fl = out->points[base_p + i].on_curve;
            if (fl & GLYF_Y_SHORT) {
                i32 dy = (i32)gdata[pos++];
                y += (fl & GLYF_Y_SAME) ? dy : -dy;
            } else if (!(fl & GLYF_Y_SAME)) {
                y += (i32)rd_i16(gdata + pos);
                pos += 2u;
            }
            out->points[base_p + i].y = (f32)y;
            out->points[base_p + i].on_curve = (u8)(fl & GLYF_ON_CURVE);
        }

        *io_c += nc;
        *io_p += n_pts;
        return TRUE;
    }

    /* Bileşik glif */
    u32 pos = 10u;
    u32 parent_start_p = *io_p;
    while (1) {
        u16 flags     = rd_u16(gdata + pos);
        u16 child_gid = rd_u16(gdata + pos + 2u);
        pos += 4u;

        f32 dx = 0.0f, dy = 0.0f;
        u16 match_pt1 = 0u, match_pt2 = 0u;
        if (flags & COMP_ARG_WORDS) {
            if (flags & COMP_ARGS_ARE_XY) {
                dx = (f32)rd_i16(gdata + pos);
                dy = (f32)rd_i16(gdata + pos + 2u);
            } else {
                match_pt1 = rd_u16(gdata + pos);
                match_pt2 = rd_u16(gdata + pos + 2u);
            }
            pos += 4u;
        } else {
            if (flags & COMP_ARGS_ARE_XY) {
                dx = (f32)(i8)gdata[pos];
                dy = (f32)(i8)gdata[pos + 1u];
            } else {
                match_pt1 = (u16)gdata[pos];
                match_pt2 = (u16)gdata[pos + 1u];
            }
            pos += 2u;
        }

        f32 a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
        if (flags & COMP_HAVE_SCALE) {
            a = d = (f32)rd_i16(gdata + pos) / 16384.0f;
            pos += 2u;
        } else if (flags & COMP_HAVE_XY_SCALE) {
            a = (f32)rd_i16(gdata + pos) / 16384.0f;
            d = (f32)rd_i16(gdata + pos + 2u) / 16384.0f;
            pos += 4u;
        } else if (flags & COMP_HAVE_TWO_BY_TWO) {
            a = (f32)rd_i16(gdata + pos + 0u) / 16384.0f;
            b = (f32)rd_i16(gdata + pos + 2u) / 16384.0f;
            c = (f32)rd_i16(gdata + pos + 4u) / 16384.0f;
            d = (f32)rd_i16(gdata + pos + 6u) / 16384.0f;
            pos += 8u;
        }

        u32 child_start_p = *io_p;
        if (!fill_glyph_geom(f, child_gid, depth + 1u, out, io_c, io_p)) return FALSE;
        u32 child_end_p = *io_p;

        if (!(flags & COMP_ARGS_ARE_XY)) {
            u32 parent_count = child_start_p - parent_start_p;
            u32 child_count  = child_end_p - child_start_p;
            if (match_pt1 >= parent_count || match_pt2 >= child_count) return FALSE;
            TtfPoint p1 = out->points[parent_start_p + match_pt1];
            TtfPoint p2 = out->points[child_start_p + match_pt2];
            f32 tx = a * p2.x + c * p2.y;
            f32 ty = b * p2.x + d * p2.y;
            dx = p1.x - tx;
            dy = p1.y - ty;
        } else if ((flags & COMP_SCALED_OFFSET) && !(flags & COMP_UNSCALED_OFFSET)) {
            f32 sx = m_sqrtf(a * a + b * b);
            f32 sy = m_sqrtf(c * c + d * d);
            dx *= sx;
            dy *= sy;
        }

        for (u32 i = child_start_p; i < child_end_p; i++) {
            f32 px = out->points[i].x;
            f32 py = out->points[i].y;
            out->points[i].x = a * px + c * py + dx;
            out->points[i].y = b * px + d * py + dy;
        }

        if (!(flags & COMP_MORE_COMPONENTS)) break;
    }
    return TRUE;
}

b32 ttf_load_glyph(const TtfFont* font, u16 glyph_index, Arena* arena, TtfGlyph* out) {
    if (!font || !arena || !out || glyph_index >= font->num_glyphs) return FALSE;
    for (usize i = 0; i < sizeof(TtfGlyph); i++) ((u8*)out)[i] = 0;

    out->glyph_index = glyph_index;
    if (!ttf_glyph_metrics(font, glyph_index, &out->advance, &out->lsb)) return FALSE;

    u32 off0, off1;
    if (!glyph_loca_range(font, glyph_index, &off0, &off1)) return FALSE;
    if (off0 == off1) return TRUE; /* boş glif */

    u32 total_c = 0u, total_p = 0u, budget = TTF_MAX_GLYPH_NODES;
    if (!count_glyph_geom(font, glyph_index, 0u, &total_c, &total_p, &budget)) return FALSE;
    if (total_c == 0u || total_p == 0u) return TRUE;

    const u8* gdata = font->data + font->glyf_off + off0;
    out->xmin = rd_i16(gdata + 2u);
    out->ymin = rd_i16(gdata + 4u);
    out->xmax = rd_i16(gdata + 6u);
    out->ymax = rd_i16(gdata + 8u);

    usize mark = arena_mark(arena);
    out->contour_ends = ARENA_ARRAY(arena, u16, total_c);
    out->points       = ARENA_ARRAY(arena, TtfPoint, total_p);

    u32 cur_c = 0u, cur_p = 0u;
    if (!fill_glyph_geom(font, glyph_index, 0u, out, &cur_c, &cur_p) ||
        cur_c != total_c || cur_p != total_p) {
        arena_reset(arena, mark);
        return FALSE;
    }

    out->contour_count = total_c;
    out->point_count   = total_p;

    /* Gerçek noktalardan sınırlayıcı kutuyu güncelle (dönüşümlü bileşik glifler için) */
    f32 min_x = out->points[0].x, max_x = out->points[0].x;
    f32 min_y = out->points[0].y, max_y = out->points[0].y;
    for (u32 i = 1u; i < total_p; i++) {
        f32 px = out->points[i].x;
        f32 py = out->points[i].y;
        if (px < min_x) min_x = px;
        if (px > max_x) max_x = px;
        if (py < min_y) min_y = py;
        if (py > max_y) max_y = py;
    }
    out->xmin = (i16)m_floorf(CLAMP(min_x, -32768.0f, 32767.0f));
    out->ymin = (i16)m_floorf(CLAMP(min_y, -32768.0f, 32767.0f));
    out->xmax = (i16)m_ceilf(CLAMP(max_x, -32768.0f, 32767.0f));
    out->ymax = (i16)m_ceilf(CLAMP(max_y, -32768.0f, 32767.0f));

    return TRUE;
}
