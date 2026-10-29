/* tests/test_font.c — TrueType ayrıştırıcı (ttf), işaretli alan glif rasterleyici ve raf paketlemeli atlas:
 * cmap 4/12, hmtx, kern, bileşik glif matrisleri, alt piksel alan doğruluğu (%0,5), delik (nonzero), Bezier,
 * atlas çakışmasızlığı, 50 000 mutasyonlu fuzz, kendine başvuran bileşik glif (kilitlenme yok).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_font.c), izinle uyarlandı (D37): dosya okuma → gömülü
 * tests/assets/test_font.ttf; sistem DejaVu testi (dosya sistemi) çıkarıldı. */
#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "core/sys.h"
#include "core/mem.h"
#include "core/embed.h"
#include "font/ttf.h"
#include "font/raster.h"
#include "font/atlas.h"

static u8* g_arena_mem;   /* her TEST başında test_arena'dan (32 MB) */
static u8* g_ttf_buf;     /* 2 MB */

/* gömülü varlık (build.py test: tests/assets içindekiler) → dst; yoksa 0 */
static u32 read_bin_file(const char* path, u8* dst, u32 cap) {
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/') name = p + 1;
    const EmbedFile* f = embed_find(name);
    if (!f || f->len > cap) return 0u;
    mem_copy(dst, f->data, f->len);
    return f->len;
}

TEST(ttf_parse_metrics_cmap_kern_composite) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    g_ttf_buf = (u8*)arena_alloc_nozero(test_arena(), MB(2), 16);
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, MB(2));
    CHECK(n > 0u);
    if (n == 0u) return;

    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));
    CHECK(font.units_per_em == 1000u);
    CHECK(font.ascent == 800);
    CHECK(font.descent == -200);
    CHECK(font.line_gap == 100);
    CHECK(font.num_glyphs == 11u);
    CHECK(font.num_hmetrics == 10u);
    CHECK(font.index_to_loc_format == 0);
    CHECK(font.cmap4_off != 0u && font.cmap12_off != 0u);
    CHECK(font.kern0_pairs_off != 0u && font.kern0_pair_count == 5u);

    /* Cmap biçim 4 ve 12 eşlemeleri */
    CHECK(ttf_glyph_index(&font, ' ') == 1u);
    CHECK(ttf_glyph_index(&font, 'A') == 2u);
    CHECK(ttf_glyph_index(&font, 'B') == 3u);
    CHECK(ttf_glyph_index(&font, 'O') == 4u);
    CHECK(ttf_glyph_index(&font, 'C') == 5u);
    CHECK(ttf_glyph_index(&font, 'D') == 6u);
    CHECK(ttf_glyph_index(&font, 0x011Eu) == 7u); /* Ğ (cmap4 glyphIdArray) */
    CHECK(ttf_glyph_index(&font, 0x011Fu) == 8u); /* ğ (cmap4 glyphIdArray) */
    CHECK(ttf_glyph_index(&font, 0x0131u) == 9u); /* ı */
    CHECK(ttf_glyph_index(&font, 0x015Fu) == 9u); /* ş */
    CHECK(ttf_cmap4_lookup(&font, 0x011Eu) == 7u);
    CHECK(ttf_cmap4_lookup(&font, 0x011Fu) == 8u);
    CHECK(ttf_cmap12_lookup(&font, 0x011Eu) == 7u);
    CHECK(ttf_cmap4_lookup(&font, 0x1F600u) == 0u);
    CHECK(ttf_cmap12_lookup(&font, 0x1F600u) == 10u);
    CHECK(ttf_glyph_index(&font, 0x1F600u) == 10u);
    CHECK(ttf_glyph_index(&font, 'Z') == 0u);
    CHECK(ttf_glyph_index(&font, 0xFFFFu) == 0u);

    /* hmtx advance ve lsb (kuyruk lsb dahil) */
    u16 adv = 0u;
    i16 lsb = 0;
    CHECK(ttf_glyph_metrics(&font, 1u, &adv, &lsb) && adv == 300u && lsb == 0);
    CHECK(ttf_glyph_metrics(&font, 2u, &adv, &lsb) && adv == 1000u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 3u, &adv, &lsb) && adv == 800u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 6u, &adv, &lsb) && adv == 950u && lsb == 100);
    CHECK(ttf_glyph_metrics(&font, 7u, &adv, &lsb) && adv == 850u && lsb == 80);
    CHECK(ttf_glyph_metrics(&font, 9u, &adv, &lsb) && adv == 650u && lsb == 50);
    CHECK(ttf_glyph_metrics(&font, 10u, &adv, &lsb) && adv == 650u && lsb == 45);
    CHECK(!ttf_glyph_metrics(&font, 11u, &adv, &lsb));

    /* Kern biçim 0 */
    CHECK(ttf_kern_advance(&font, 2u, 3u) == -80);
    CHECK(ttf_kern_advance(&font, 2u, 4u) == -50);
    CHECK(ttf_kern_advance(&font, 3u, 2u) == -30);
    CHECK(ttf_kern_advance(&font, 4u, 2u) == -40);
    CHECK(ttf_kern_advance(&font, 6u, 7u) == 25);
    CHECK(ttf_kern_advance(&font, 2u, 2u) == 0);

    /* Bileşik glif dönüşümleri */
    Arena a;
    arena_init(&a, g_arena_mem, MB(4));
    TtfGlyph g;

    /* Glif 1: boş */
    CHECK(ttf_load_glyph(&font, 1u, &a, &g));
    CHECK(g.contour_count == 0u && g.point_count == 0u && g.advance == 300u);

    /* Glif 6: ölçekli (0.5) kare + xy-ölçekli (0.5, 0.5) üçgen */
    CHECK(ttf_load_glyph(&font, 6u, &a, &g));
    CHECK(g.contour_count == 2u && g.point_count == 7u);
    CHECK(g.contour_ends[0] == 3u && g.contour_ends[1] == 6u);
    CHECK_NEAR(g.points[0].x, 100.0f, 1e-4);
    CHECK_NEAR(g.points[0].y, 100.0f, 1e-4);
    CHECK_NEAR(g.points[2].x, 400.0f, 1e-4);
    CHECK_NEAR(g.points[2].y, 400.0f, 1e-4);
    CHECK_NEAR(g.points[4].x, 450.0f, 1e-4);
    CHECK_NEAR(g.points[4].y, 100.0f, 1e-4);
    CHECK_NEAR(g.points[5].x, 650.0f, 1e-4);
    CHECK_NEAR(g.points[5].y, 500.0f, 1e-4);
    CHECK_NEAR(g.points[6].x, 850.0f, 1e-4);
    CHECK_NEAR(g.points[6].y, 100.0f, 1e-4);
    CHECK(g.xmin == 100 && g.ymin == 100 && g.xmax == 850 && g.ymax == 500);

    /* Glif 7: 2x2 matrisli (a=0.5, b=0, c=0.25, d=0.5, dx=80, dy=40) kare */
    CHECK(ttf_load_glyph(&font, 7u, &a, &g));
    CHECK(g.contour_count == 1u && g.point_count == 4u);
    CHECK_NEAR(g.points[0].x, 155.0f, 1e-3);
    CHECK_NEAR(g.points[0].y,  90.0f, 1e-3);
    CHECK_NEAR(g.points[1].x, 305.0f, 1e-3);
    CHECK_NEAR(g.points[1].y, 390.0f, 1e-3);
    CHECK_NEAR(g.points[2].x, 605.0f, 1e-3);
    CHECK_NEAR(g.points[2].y, 390.0f, 1e-3);
    CHECK_NEAR(g.points[3].x, 455.0f, 1e-3);
    CHECK_NEAR(g.points[3].y,  90.0f, 1e-3);
}

static f64 bitmap_area_sum(const u8* px, u32 w, u32 h) {
    u64 sum = 0u;
    for (u32 i = 0u; i < w * h; i++) sum += px[i];
    return (f64)sum / 255.0;
}

TEST(font_raster_square_subpixel_hole_area_triangle) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    g_ttf_buf = (u8*)arena_alloc_nozero(test_arena(), MB(2), 16);
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, MB(2));
    CHECK(n > 0u);
    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));

    Arena a;
    arena_init(&a, g_arena_mem, MB(8));

    /* 1. Eksen hizalı kare (Glif 3: [100..700] x [100..700])
     * scale = 0.01 -> [1..7] x [-7..-1]. offset=(1, 9) ile 10x10 tamponda [2..8) x [2..8) */
    TtfGlyph sq;
    CHECK(ttf_load_glyph(&font, 3u, &a, &sq));
    u8 canvas[100];
    mem_set(canvas, (u8)(0xAA), sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&sq, 0.01f, 0.01f, 1.0f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (x >= 2u && x < 8u && y >= 2u && y < 8u) {
                CHECK(v == 255u);
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 36.0, 36.0 * 0.005);

    /* 2. Yarım piksel (+0.5) x kaydırmalı kare: [2.5 .. 8.5] x [2.0 .. 8.0] */
    mem_set(canvas, (u8)(0xAA), sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&sq, 0.01f, 0.01f, 1.5f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (y >= 2u && y < 8u) {
                if (x == 2u || x == 8u) {
                    CHECK(v >= 127u && v <= 128u);
                } else if (x >= 3u && x < 8u) {
                    CHECK(v == 255u);
                } else {
                    CHECK(v == 0u);
                }
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 36.0, 36.0 * 0.005);

    /* 3. Delikli 'O' konturu (Glif 4: dış [100..700]^2, iç delik [300..500]^2) */
    TtfGlyph hollow;
    CHECK(ttf_load_glyph(&font, 4u, &a, &hollow));
    mem_set(canvas, (u8)(0xAA), sizeof(canvas));
    CHECK(font_rasterize_glyph_ex(&hollow, 0.01f, 0.01f, 1.0f, 9.0f, canvas, 10u, 10u, 10u, &a));
    for (u32 y = 0u; y < 10u; y++) {
        for (u32 x = 0u; x < 10u; x++) {
            u8 v = canvas[y * 10u + x];
            if (x >= 4u && x < 6u && y >= 4u && y < 6u) {
                CHECK(v == 0u); /* iç delik 0 */
            } else if (x >= 2u && x < 8u && y >= 2u && y < 8u) {
                CHECK(v == 255u);
            } else {
                CHECK(v == 0u);
            }
        }
    }
    CHECK_NEAR(bitmap_area_sum(canvas, 10u, 10u), 32.0, 32.0 * 0.005);

    /* Delikli konturu küsuratlı ölçek ve alt piksel kaydırmayla da doğrula */
    u8 big[64 * 64];
    f32 sc = 0.043f;
    CHECK(font_rasterize_glyph(&hollow, sc, sc, 0.37f, 0.21f, big, 64u, 64u, 64u, &a));
    f64 expected_hollow = 320000.0 * (f64)sc * (f64)sc;
    CHECK_NEAR(bitmap_area_sum(big, 64u, 64u), expected_hollow, expected_hollow * 0.005);

    /* 4. İkinci derece Bezier eğrisi (Glif 5: analitik alan = 300000 * sc^2)
     * Hem doğrudan rasterleme hem font_flatten_glyph + font_rasterize_edges yolu doğrulanır */
    TtfGlyph bez;
    CHECK(ttf_load_glyph(&font, 5u, &a, &bez));
    mem_set(big, (u8)(0), sizeof(big));
    CHECK(font_rasterize_glyph(&bez, 0.05f, 0.05f, 0.25f, 0.0f, big, 64u, 64u, 64u, &a));
    f64 expected_bez = 300000.0 * 0.05 * 0.05; /* 750.0 px^2 */
    CHECK_NEAR(bitmap_area_sum(big, 64u, 64u), expected_bez, expected_bez * 0.005);

    FontEdge* edges = NULL;
    u32 edge_cnt = font_flatten_glyph(&bez, 0.05f, 0.05f, 2.0f, 40.0f, FONT_DEFAULT_FLATNESS, &a, &edges);
    CHECK(edge_cnt >= 16u && edges != NULL);
    u8 big2[64 * 64];
    mem_set(big2, (u8)(0), sizeof(big2));
    CHECK(font_rasterize_edges(edges, edge_cnt, big2, 64u, 64u, 64u, &a));
    CHECK_NEAR(bitmap_area_sum(big2, 64u, 64u), expected_bez, expected_bez * 0.005);

    /* 5. Üçgen (Glif 2: (100,0)->(500,800)->(900,0), analitik alan = 320000 * sc^2)
     * Kenar monotonluğu ve alan doğrulaması */
    TtfGlyph tri;
    CHECK(ttf_load_glyph(&font, 2u, &a, &tri));
    u8 tri_bmp[32 * 32];
    mem_set(tri_bmp, (u8)(0), sizeof(tri_bmp));
    f32 tsc = 0.025f; /* genişlik=20, yükseklik=20, tepe x=12.5, analitik alan = 200.0 */
    CHECK(font_rasterize_glyph_ex(&tri, tsc, tsc, 0.0f, 22.0f, tri_bmp, 32u, 32u, 32u, &a));
    f64 expected_tri = 320000.0 * (f64)tsc * (f64)tsc;
    CHECK_NEAR(bitmap_area_sum(tri_bmp, 32u, 32u), expected_tri, expected_tri * 0.005);

    /* Üçgenin her satırında soldan ortaya monoton artan, ortadan sağa monoton azalan;
     * ayrıca her sütunda yukarıdan aşağıya (y=2 tepe -> y=21 taban) monoton artan */
    for (u32 y = 2u; y < 22u; y++) {
        for (u32 x = 0u; x < 12u; x++) {
            CHECK(tri_bmp[y * 32u + x] <= tri_bmp[y * 32u + x + 1u]);
        }
        for (u32 x = 13u; x < 31u; x++) {
            CHECK(tri_bmp[y * 32u + x] >= tri_bmp[y * 32u + x + 1u]);
        }
    }
    for (u32 x = 0u; x < 32u; x++) {
        for (u32 y = 2u; y < 21u; y++) {
            CHECK(tri_bmp[y * 32u + x] <= tri_bmp[(y + 1u) * 32u + x]);
        }
    }
}

TEST(font_atlas_and_measure) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    g_ttf_buf = (u8*)arena_alloc_nozero(test_arena(), MB(2), 16);
    u32 n = read_bin_file("tests/assets/test_font.ttf", g_ttf_buf, MB(2));
    CHECK(n > 0u);
    TtfFont font;
    CHECK(ttf_parse(g_ttf_buf, n, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(16));
    arena_init(&scratch_a, g_arena_mem + MB(16), MB(16));

    FontAtlas atlas;
    CHECK(font_atlas_build(&font, 20.0f, 256u, 256u, &out_a, &scratch_a, &atlas));
    CHECK(atlas.w == 256u && atlas.h == 256u);
    CHECK_NEAR(atlas.scale, 0.02f, 1e-6);

    /* Paketlenen glif kutularının birbirine çarpmadığını doğrula */
    for (u32 i = 0u; i < atlas.glyph_count; i++) {
        const FontGlyph* gi = &atlas.glyphs[i];
        if (gi->w == 0u || gi->h == 0u) continue;
        CHECK(gi->x + gi->w <= atlas.w && gi->y + gi->h <= atlas.h);
        CHECK(gi->u0 >= 0.0f && gi->u1 <= 1.0f && gi->u0 < gi->u1);
        CHECK(gi->v0 >= 0.0f && gi->v1 <= 1.0f && gi->v0 < gi->v1);
        for (u32 j = i + 1u; j < atlas.glyph_count; j++) {
            const FontGlyph* gj = &atlas.glyphs[j];
            if (gj->w == 0u || gj->h == 0u) continue;
            b32 overlap_x = (gi->x < gj->x + gj->w) && (gj->x < gi->x + gi->w);
            b32 overlap_y = (gi->y < gj->y + gj->h) && (gj->y < gi->y + gi->h);
            CHECK(!(overlap_x && overlap_y));
        }
    }

    /* Kerning ve UTF-8 metin ölçümü */
    f32 w = 0.0f, h = 0.0f;
    CHECK_NEAR(font_measure(&atlas, "A", &w, &h), 20.0f, 1e-4);
    CHECK_NEAR(w, 20.0f, 1e-4);
    CHECK_NEAR(h, 22.0f, 1e-4); /* (800 - (-200) + 100) * 0.02 = 22 */

    /* 'A' (1000) + 'B' (800) + kern(-80) = 1720 * 0.02 = 34.4 */
    CHECK_NEAR(font_measure(&atlas, "AB", NULL, NULL), 34.4f, 1e-4);
    /* 'D' (950) + 'Ğ' (850) + kern(+25) = 1825 * 0.02 = 36.5 */
    CHECK_NEAR(font_measure(&atlas, "D\xC4\x9E", NULL, NULL), 36.5f, 1e-4);
    /* "Ağış": 1000 + 750 + 650 + 650 = 3050 * 0.02 = 61.0 */
    CHECK_NEAR(font_measure(&atlas, "A\xC4\x9F\xC4\xB1\xC5\x9F", NULL, NULL), 61.0f, 1e-4);
}

TEST(font_ttf_fuzz_50k) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    g_ttf_buf = (u8*)arena_alloc_nozero(test_arena(), MB(2), 16);
    u8 orig[2048];
    u32 n = read_bin_file("tests/assets/test_font.ttf", orig, sizeof(orig));
    CHECK(n > 0u && n <= sizeof(orig));

    /* ASan kırmızı bölge (redzone) denetimi için tam boyutlu yığın tamponu */
    u8* mut_buf = ARENA_ARRAY(test_arena(), u8, n);
    CHECK(mut_buf != NULL);

    Arena a;
    arena_init(&a, g_arena_mem, MB(4));

    Rng rng = rng_make(0x9C0FFEE1u);
    u32 parsed_ok = 0u;
    u8 small_bmp[32 * 32];

    for (u32 it = 0u; it < 50000u; it++) {
        mem_copy(mut_buf, orig, n);
        u32 mut_count = 1u + (rng_u32(&rng) & 3u);
        for (u32 m = 0u; m < mut_count; m++) {
            mut_buf[rng_u32(&rng) % n] = (u8)rng_u32(&rng);
        }

        u32 test_len = n;
        if ((it & 31u) == 0u) {
            test_len = rng_u32(&rng) % (n + 1u);
        }

        TtfFont f;
        if (ttf_parse(mut_buf, test_len, &f)) {
            parsed_ok++;
            u16 gid_a = ttf_glyph_index(&f, 'A');
            u16 gid_b = ttf_glyph_index(&f, 'B');
            (void)ttf_kern_advance(&f, gid_a, gid_b);
            (void)ttf_glyph_index(&f, 0x011Eu);
            (void)ttf_glyph_index(&f, 0x1F600u);

            u16 limit = MIN(f.num_glyphs, (u16)11u);
            for (u16 g = 0u; g < limit; g++) {
                usize mark = arena_mark(&a);
                TtfGlyph tg;
                if (ttf_load_glyph(&f, g, &a, &tg)) {
                    font_rasterize_glyph(&tg, 0.01f, 0.01f, 0.0f, 0.0f,
                                         small_bmp, 16u, 16u, 16u, &a);
                }
                arena_reset(&a, mark);
            }
        }
    }

    CHECK(parsed_ok > 0u);
}

/* İnceleme: kendine başvuran bileşik glif (6 → 6, 6) sınırlı işte reddedilir.
 * Bütçe olmadan 2^17 çağrı yapardı; 256 bileşenli bir glifte 256^16 → kilitlenme. */
TEST(ttf_review_self_referencing_composite) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    g_ttf_buf = (u8*)arena_alloc_nozero(test_arena(), MB(2), 16);
    static u8 buf[4096];
    u32 n = read_bin_file("tests/assets/test_font.ttf", buf, sizeof(buf));
    CHECK(n > 0);
    TtfFont font;
    CHECK(ttf_parse(buf, n, &font));
    u32 off = font.index_to_loc_format == 0
        ? (u32)(((u32)buf[font.loca_off + 12] << 8) | buf[font.loca_off + 13]) * 2u
        : ((u32)buf[font.loca_off + 24] << 24) | ((u32)buf[font.loca_off + 25] << 16) |
          ((u32)buf[font.loca_off + 26] << 8) | buf[font.loca_off + 27];
    u8* g = buf + font.glyf_off + off;
    CHECK((i16)((g[0] << 8) | g[1]) < 0);    /* glif 6 bileşik */
    /* İki bileşen: bayrak(2) gid(2) arg(4) + ölçek(2) ; bayrak gid arg(4) + xy ölçek(4) */
    g[10 + 2] = 0; g[10 + 3] = 6;
    g[10 + 10 + 2] = 0; g[10 + 10 + 3] = 6;
    TtfFont f2;
    CHECK(ttf_parse(buf, n, &f2));
    u8* mem = ARENA_ARRAY(test_arena(), u8, KB(256));
    Arena a;
    arena_init(&a, mem, KB(256));
    TtfGlyph gl;
    f64 t0 = sys_now_ms();
    CHECK(!ttf_load_glyph(&f2, 6, &a, &gl));
    CHECK(sys_now_ms() - t0 < 50.0);
}
