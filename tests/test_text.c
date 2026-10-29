/* tests/test_text.c — gömülü "Matoki Sans": ayrıştırma, sargı yönü, delikli glifler; 12/16/24/48 px atlas ve taban
 * çizgisi; kelime sarmalama ve hizalama; kırpma ve düşmanca girdiler; görsel kare hash'i ve 1000 karakter ölçümü.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_text.c), izinle uyarlandı (D37): PPM dosyası yerine kare
 * hash'i, malloc yerine test arenası, disk ↔ gömülü font karşılaştırması çıkarıldı (wasm'da dosya sistemi yok). */
#include "test.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/sys.h"
#include "raster/raster.h"
#include "font/ttf.h"
#include "font/raster.h"
#include "font/atlas.h"
#include "font/text.h"
#include "font/matoki_sans_data.h"

static u8* g_arena_mem;   /* her TEST başında test_arena'dan (32 MB) */

static void fb_init(Framebuffer* fb, Arena* a, u32 w, u32 h) {
    mem_zero(fb, sizeof(*fb));
    fb->w = (i32)w;
    fb->h = (i32)h;
    fb->color = ARENA_ARRAY(a, u32, (usize)w * h);
}

static void fb_fill(Framebuffer* fb, u32 c) {
    for (usize i = 0; i < (usize)fb->w * (usize)fb->h; i++) fb->color[i] = c;
}

static u32 fb_hash(const Framebuffer* fb) {
    u32 h = 0x811c9dc5u;
    for (usize i = 0; i < (usize)fb->w * (usize)fb->h; i++) h = (h ^ fb->color[i]) * 0x01000193u;
    return h;
}

/* Framebuffer içinde arka plan renginden (bg_rgba) farklı piksellerin sınırlayıcı kutusunu bulur */
static b32 fb_ink_bbox(const Framebuffer* fb, u32 bg_rgba,
                       i32* out_x0, i32* out_y0, i32* out_x1, i32* out_y1) {
    i32 xmin = (i32)((u32)fb->w), ymin = (i32)((u32)fb->h);
    i32 xmax = -1, ymax = -1;
    for (u32 y = 0u; y < ((u32)fb->h); y++) {
        const u32* row = fb->color + (usize)y * ((u32)fb->w);
        for (u32 x = 0u; x < ((u32)fb->w); x++) {
            if (row[x] != bg_rgba) {
                if ((i32)x < xmin) xmin = (i32)x;
                if ((i32)y < ymin) ymin = (i32)y;
                if ((i32)x > xmax) xmax = (i32)x;
                if ((i32)y > ymax) ymax = (i32)y;
            }
        }
    }
    if (xmax < xmin || ymax < ymin) return FALSE;
    if (out_x0) *out_x0 = xmin;
    if (out_y0) *out_y0 = ymin;
    if (out_x1) *out_x1 = xmax + 1;
    if (out_y1) *out_y1 = ymax + 1;
    return TRUE;
}

/* Düzleştirilmiş kenarlardan bir glif konturunun ekran uzayı (+y aşağı) işaretli alanını hesaplar.
 * TTF (+y yukarı) uzayında CW olan dış konturlar ekran uzayında (+y aşağı) CCW olur (alan < 0),
 * TTF uzayında CCW olan delikler ekran uzayında CW olur (alan > 0). */
static f32 glyph_contour_signed_area_ttf(const TtfPoint* pts, u32 n) {
    if (n < 3u) return 0.0f;
    /* Çokgen köşeleri üzerinden yaklaşık alan + örtük orta noktalar */
    Vec2 poly[512];
    u32  pn = 0u;
    for (u32 i = 0u; i < n && pn < 510u; i++) {
        TtfPoint cur  = pts[i];
        TtfPoint next = pts[(i + 1u) % n];
        poly[pn++] = v2(cur.x, cur.y);
        if (!cur.on_curve && !next.on_curve && pn < 510u) {
            poly[pn++] = v2(0.5f * (cur.x + next.x), 0.5f * (cur.y + next.y));
        }
    }
    f32 a2 = 0.0f;
    for (u32 i = 0u; i < pn; i++) {
        Vec2 p0 = poly[i];
        Vec2 p1 = poly[(i + 1u) % pn];
        a2 += p0.x * p1.y - p1.x * p0.y;
    }
    return 0.5f * a2;
}

/* Rasterlenmiş bir glif tamponunda (gw x gh) iç delik (boşluk) bulunduğunu doğrular:
 * hem solunda, hem sağında, hem üstünde, hem altında dolu (>180) piksel olan bir 0 (boş) pikseli arar. */
static b32 bitmap_has_enclosed_hole(const u8* bmp, u32 gw, u32 gh) {
    if (gw < 5u || gh < 5u) return FALSE;
    for (u32 y = 2u; y + 2u < gh; y++) {
        for (u32 x = 2u; x + 2u < gw; x++) {
            if (bmp[y * gw + x] != 0u) continue;
            b32 left = FALSE, right = FALSE, up = FALSE, down = FALSE;
            for (u32 lx = 0u; lx < x; lx++) {
                if (bmp[y * gw + lx] >= 180u) { left = TRUE; break; }
            }
            for (u32 rx = x + 1u; rx < gw; rx++) {
                if (bmp[y * gw + rx] >= 180u) { right = TRUE; break; }
            }
            for (u32 uy = 0u; uy < y; uy++) {
                if (bmp[uy * gw + x] >= 180u) { up = TRUE; break; }
            }
            for (u32 dy = y + 1u; dy < gh; dy++) {
                if (bmp[dy * gw + x] >= 180u) { down = TRUE; break; }
            }
            if (left && right && up && down) return TRUE;
        }
    }
    return FALSE;
}

TEST(matoki_sans_ttf_parse_winding_and_holes) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    CHECK(g_matoki_sans_ttf_len == MATOKI_SANS_TTF_LEN && g_matoki_sans_ttf_len > 4000u);

    /* 2. ttf_parse doğrulaması ve metrikler */
    TtfFont font;
    CHECK(ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font));
    CHECK(font.units_per_em == 1000u);
    CHECK(font.ascent == 800);
    CHECK(font.descent == -200);
    CHECK(font.line_gap == 100);
    CHECK(font.num_glyphs >= 120u);
    CHECK(font.cmap4_off != 0u && font.cmap12_off != 0u);
    CHECK(font.kern0_pairs_off != 0u && font.kern0_pair_count >= 50u);

    /* 3. Tüm tanımlı kod noktalarının glif bulduğunu doğrula */
    for (u32 cp = 32u; cp <= 126u; cp++) {
        u16 gid  = ttf_glyph_index(&font, cp);
        u16 gid4 = ttf_cmap4_lookup(&font, cp);
        u16 gid12 = ttf_cmap12_lookup(&font, cp);
        CHECK(gid != 0u && gid < font.num_glyphs);
        CHECK(gid == gid4 && gid == gid12);
    }

    const u32 tr_and_sym[] = {
        0x00C7u, 0x00D6u, 0x00DCu, 0x00E7u, 0x00F6u, 0x00FCu, /* Ç Ö Ü ç ö ü */
        0x011Eu, 0x011Fu, 0x0130u, 0x0131u, 0x015Eu, 0x015Fu, /* Ğ ğ İ ı Ş ş */
        0x00A0u, 0x00A9u, 0x00ABu, 0x00AEu, 0x00B0u, 0x00B1u, /* NBSP © « ® ° ± */
        0x00B7u, 0x00BBu, 0x00D7u, 0x00F7u,                   /* · » × ÷ */
        0x2013u, 0x2014u, 0x2022u, 0x2026u, 0x20BAu           /* – — • … ₺ */
    };
    for (u32 i = 0u; i < ARRAY_COUNT(tr_and_sym); i++) {
        u16 gid = ttf_glyph_index(&font, tr_and_sym[i]);
        CHECK(gid != 0u && gid < font.num_glyphs);
    }

    /* 4. Tüm gliflerin konturlarını yükle, sargı yönünü (dış CW: alan < 0, delik CCW: alan > 0) doğrula */
    Arena a;
    arena_init(&a, g_arena_mem, MB(8));
    for (u16 gid = 0u; gid < font.num_glyphs; gid++) {
        usize mark = arena_mark(&a);
        TtfGlyph tg;
        CHECK(ttf_load_glyph(&font, gid, &a, &tg));
        CHECK(tg.advance > 0u);
        if (tg.contour_count > 0u) {
            /* İlk kontur her zaman dış konturdur ve TTF (+y yukarı) uzayında CW (işaretli alan < 0) olmalıdır */
            u32 c0_n = (u32)tg.contour_ends[0] + 1u;
            f32 area0 = glyph_contour_signed_area_ttf(tg.points, c0_n);
            CHECK(area0 < -10.0f);

            u32 start = 0u;
            for (u32 c = 0u; c < tg.contour_count; c++) {
                u32 end = (u32)tg.contour_ends[c];
                CHECK(end >= start && end < tg.point_count);
                u32 cn = end - start + 1u;
                CHECK(cn >= 3u);
                f32 area = glyph_contour_signed_area_ttf(tg.points + start, cn);
                CHECK(m_absf(area) > 10.0f);
                start = end + 1u;
            }
        }
        arena_reset(&a, mark);
    }

    /* 5. Delikli harflerin ("O", "a", "e", "ğ" ve diğerleri) rasterleyicide deliklerinin boş olduğunu doğrula */
    const u32 hole_cps[] = {
        'O', 'a', 'e', 0x011Fu /* ğ */,
        'A', 'B', 'D', 'P', 'Q', 'R',
        'b', 'd', 'g', 'o', 'p', 'q',
        '0', '4', '6', '8', '9',
        0x00D6u /* Ö */, 0x00F6u /* ö */
    };
    u8 bmp[128 * 128];
    f32 scale = 48.0f / (f32)font.units_per_em;
    for (u32 i = 0u; i < ARRAY_COUNT(hole_cps); i++) {
        usize mark = arena_mark(&a);
        u16 gid = ttf_glyph_index(&font, hole_cps[i]);
        CHECK(gid != 0u);
        TtfGlyph tg;
        CHECK(ttf_load_glyph(&font, gid, &a, &tg));
        CHECK(tg.contour_count >= 2u);

        /* En az bir CCW delik konturu (TTF alanında > 0) bulunduğunu doğrula */
        b32 found_ccw = FALSE;
        u32 start = 0u;
        for (u32 c = 0u; c < tg.contour_count; c++) {
            u32 end = (u32)tg.contour_ends[c];
            f32 area = glyph_contour_signed_area_ttf(tg.points + start, end - start + 1u);
            if (area > 10.0f) found_ccw = TRUE;
            start = end + 1u;
        }
        CHECK(found_ccw);

        i32 ix0, iy0, ix1, iy1;
        font_glyph_bbox(&tg, scale, scale, 0.0f, 0.0f, &ix0, &iy0, &ix1, &iy1);
        u32 gw = (u32)(ix1 - ix0);
        u32 gh = (u32)(iy1 - iy0);
        CHECK(gw > 8u && gw <= 128u && gh > 8u && gh <= 128u);
        mem_set(bmp, (u8)(0xFF), sizeof(bmp));
        CHECK(font_rasterize_glyph(&tg, scale, scale, 0.0f, 0.0f, bmp, gw, gh, gw, &a));
        CHECK(bitmap_has_enclosed_hole(bmp, gw, gh));
        arena_reset(&a, mark);
    }
}

TEST(matoki_sans_atlas_12_16_24_48_and_baseline) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    TtfFont font;
    CHECK(ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(16));
    arena_init(&scratch_a, g_arena_mem + MB(16), MB(16));

    const f32 sizes[4] = { 12.0f, 16.0f, 24.0f, 48.0f };
    FontAtlas atlases[4];
    const char* sample = "Ağaç kökü İğdır'da şöyle";

    f32 w_at[4], h_at[4];
    for (u32 s = 0u; s < 4u; s++) {
        CHECK(font_atlas_build(&font, sizes[s], 0u, 0u, &out_a, &scratch_a, &atlases[s]));
        CHECK(atlases[s].w >= 256u && atlases[s].h >= 256u);

        f32 fm_w = 0.0f, fm_h = 0.0f;
        f32 tm_w = 0.0f, tm_h = 0.0f;
        f32 r1 = font_measure(&atlases[s], sample, &fm_w, &fm_h);
        f32 r2 = text_measure(&atlases[s], sample, NULL, &tm_w, &tm_h);
        CHECK_NEAR(r1, fm_w, 1e-5);
        CHECK_NEAR(r2, tm_w, 1e-5);
        CHECK_NEAR(fm_w, tm_w, 1e-5);
        CHECK_NEAR(fm_h, tm_h, 1e-5);
        CHECK(tm_w > sizes[s] * 8.0f);
        CHECK_NEAR(tm_h, sizes[s] * 1.1f, 1e-4);

        /* Kerning aktifken genişlik, kerning kapalıykenki genişlikten küçüktür */
        TextStyle no_kern_st = text_style_default(color_pack(255, 255, 255, 255));
        no_kern_st.no_kerning = TRUE;
        f32 w_nokern = text_measure(&atlases[s], sample, &no_kern_st, NULL, NULL);
        CHECK(tm_w < w_nokern);

        w_at[s] = tm_w;
        h_at[s] = tm_h;

        /* Çizim ve ölçüm tutarlılığı */
        usize mark = arena_mark(&scratch_a);
        Framebuffer fb;
        fb_init(&fb, &scratch_a, 1200u, 160u);
        u32 bg = color_pack(10, 12, 18, 255);
        fb_fill(&fb, bg);

        TextStyle st = text_style_default(color_pack(255, 255, 255, 255));
        text_draw(&fb, &atlases[s], 20, 20, sample, &st);

        i32 ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
        CHECK(fb_ink_bbox(&fb, bg, &ix0, &iy0, &ix1, &iy1));
        f32 drawn_w = (f32)(ix1 - ix0);
        /* Çizilen mürekkep genişliği ile advance genişliği yan boşluklar (lsb+rsb) kadar fark eder */
        CHECK(m_absf(drawn_w - tm_w) <= sizes[s] * 0.4f + 2.0f);
        CHECK(iy0 >= 20 && iy1 <= 20 + (i32)m_ceilf(tm_h) + 2);

        /* Taban çizgisi (baseline) hizası testi:
         * Taban çizgisine oturan alçalmasız harflerin ("A", "a", "k", "ö", "ü", "İ", "ı", "d", "r", "o", "l", "e")
         * hepsi tam olarak aynı taban satırında (baseline_y - 1) biter;
         * alçalan harfler ("ğ", "ç", "ş", "y") ise baseline_y'nin altına iner. */
        const char* base_chars[] = { "A", "a", "k", "ö", "ü", "İ", "ı", "d", "r", "o", "l", "e" };
        const char* desc_chars[] = { "ğ", "ç", "ş", "y" };
        i32 expected_baseline = 30 + (i32)m_ceilf(atlases[s].ascent);

        for (u32 c = 0u; c < ARRAY_COUNT(base_chars); c++) {
            fb_fill(&fb, bg);
            text_draw(&fb, &atlases[s], 30, 30, base_chars[c], &st);
            i32 cx0, cy0, cx1, cy1;
            CHECK(fb_ink_bbox(&fb, bg, &cx0, &cy0, &cx1, &cy1));
            CHECK(cy1 == expected_baseline);
        }
        for (u32 c = 0u; c < ARRAY_COUNT(desc_chars); c++) {
            fb_fill(&fb, bg);
            text_draw(&fb, &atlases[s], 30, 30, desc_chars[c], &st);
            i32 cx0, cy0, cx1, cy1;
            CHECK(fb_ink_bbox(&fb, bg, &cx0, &cy0, &cx1, &cy1));
            CHECK(cy1 > expected_baseline);
        }

        arena_reset(&scratch_a, mark);
    }

    /* Ölçek orantılılığı: 12, 16, 24, 48 px ölçümleri boyutla tam orantılı */
    CHECK_NEAR(w_at[1] / 16.0f, w_at[0] / 12.0f, 1e-4);
    CHECK_NEAR(w_at[2] / 24.0f, w_at[0] / 12.0f, 1e-4);
    CHECK_NEAR(w_at[3] / 48.0f, w_at[0] / 12.0f, 1e-4);
    CHECK_NEAR(h_at[3], h_at[1] * 3.0f, 1e-4);
}

TEST(text_word_wrap_and_alignment) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    TtfFont font;
    CHECK(ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(8));
    arena_init(&scratch_a, g_arena_mem + MB(8), MB(8));

    FontAtlas atlas;
    CHECK(font_atlas_build(&font, 16.0f, 0u, 0u, &out_a, &scratch_a, &atlas));

    const char* para =
        "Ağaç kökü İğdır'da şöyle filizlenir; güneşli dağların eteklerinde "
        "rüzgârla salınan yapraklar gökyüzüne doğru uzanır.";

    const f32 widths[3] = { 110.0f, 160.0f, 240.0f };
    for (u32 wi = 0u; wi < 3u; wi++) {
        f32 max_w = widths[wi];
        TextLine lines[64];
        u32 n_lines = text_wrap(&atlas, para, max_w, lines, 64u);
        CHECK(n_lines >= 3u && n_lines < 64u);

        for (u32 i = 0u; i < n_lines; i++) {
            CHECK(lines[i].byte_len > 0u);
            CHECK(lines[i].width <= max_w + 1e-3f);

            /* Satır sınırlarının kelime ortasına düşmediğini doğrula:
             * Satır başı ya metin başıdır ya da bir önceki karakter boşluktur;
             * satır sonu ya metin sonudur ya da bir sonraki karakter boşluktur. */
            if (lines[i].start > para) {
                char prev_c = *(lines[i].start - 1);
                CHECK(prev_c == ' ' || prev_c == '\t' || prev_c == '\n');
            }
            const char* after = lines[i].start + lines[i].byte_len;
            CHECK(*after == '\0' || *after == ' ' || *after == '\t' || *after == '\n');

            /* Satır dilimini ayrı bir C dizgesi olarak ölçüp genişliğin birebir tuttuğunu doğrula */
            char buf[256];
            u32 blen = MIN(lines[i].byte_len, (u32)sizeof(buf) - 1u);
            mem_copy(buf, lines[i].start, blen);
            buf[blen] = '\0';
            f32 slice_w = text_measure(&atlas, buf, NULL, NULL, NULL);
            CHECK_NEAR(slice_w, lines[i].width, 1e-3f);
            CHECK(slice_w <= max_w + 1e-3f);
        }

        /* text_measure ile sarmalanmış ölçümün text_wrap ile tutarlılığı */
        TextStyle wrap_st = text_style_default(color_pack(255, 255, 255, 255));
        wrap_st.max_width = max_w;
        f32 mw = 0.0f, mh = 0.0f;
        text_measure(&atlas, para, &wrap_st, &mw, &mh);
        CHECK(mw <= max_w + 1e-3f);
        CHECK_NEAR(mh, (f32)n_lines * atlas.line_height, 1e-3f);
    }

    /* Tek kelime max_width'ten uzunsa karakter bazlı bölünmeli ve hiçbir satır max_width'i aşmamalı */
    const char* long_word = "Muvaffakiyetsizleştiricileştiriveremeyebileceklerimizdenmişsinizcesine";
    f32 char_wrap_w = 85.0f;
    TextLine cw_lines[32];
    u32 cw_count = text_wrap(&atlas, long_word, char_wrap_w, cw_lines, 32u);
    CHECK(cw_count >= 5u && cw_count < 32u);

    char reconstructed[256];
    u32 rec_pos = 0u;
    for (u32 i = 0u; i < cw_count; i++) {
        CHECK(cw_lines[i].byte_len > 0u);
        CHECK(cw_lines[i].width <= char_wrap_w + 1e-3f);
        /* UTF-8 çok baytlı karakterlerin ortadan bölünmediğini doğrula */
        u8 first_byte = (u8)cw_lines[i].start[0];
        CHECK((first_byte & 0xC0u) != 0x80u);
        mem_copy(reconstructed + rec_pos, cw_lines[i].start, cw_lines[i].byte_len);
        rec_pos += cw_lines[i].byte_len;
    }
    reconstructed[rec_pos] = '\0';
    CHECK(rec_pos == (u32)cstr_len(long_word) && mem_equal(reconstructed, long_word, rec_pos));

    /* Sol / Orta / Sağ hizalama doğrulaması */
    Framebuffer fb;
    fb_init(&fb, &scratch_a, 320u, 80u);
    u32 bg = color_pack(0, 0, 0, 255);
    i32 x0_l = 0, x0_c = 0, x0_r = 0;

    TextStyle st = text_style_default(color_pack(255, 255, 255, 255));
    st.max_width = 240.0f;

    st.align = TEXT_ALIGN_LEFT;
    fb_fill(&fb, bg);
    text_draw(&fb, &atlas, 20, 10, "Matoki", &st);
    CHECK(fb_ink_bbox(&fb, bg, &x0_l, NULL, NULL, NULL));

    st.align = TEXT_ALIGN_CENTER;
    fb_fill(&fb, bg);
    text_draw(&fb, &atlas, 20, 10, "Matoki", &st);
    CHECK(fb_ink_bbox(&fb, bg, &x0_c, NULL, NULL, NULL));

    st.align = TEXT_ALIGN_RIGHT;
    fb_fill(&fb, bg);
    text_draw(&fb, &atlas, 20, 10, "Matoki", &st);
    CHECK(fb_ink_bbox(&fb, bg, &x0_r, NULL, NULL, NULL));

    CHECK(x0_l < x0_c && x0_c < x0_r);
}

TEST(text_clipping_and_hostile_inputs_asan) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    TtfFont font;
    CHECK(ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(8));
    arena_init(&scratch_a, g_arena_mem + MB(8), MB(8));

    FontAtlas atlas;
    CHECK(font_atlas_build(&font, 24.0f, 0u, 0u, &out_a, &scratch_a, &atlas));

    TextStyle st = text_style_default(color_pack(255, 220, 100, 255));
    st.shadow_color  = color_pack(0, 0, 0, 200);
    st.shadow_dx     = 2;
    st.shadow_dy     = 2;
    st.outline_color = color_pack(40, 20, 10, 255);
    st.outline_px    = 2;

    /* 1. Sıfır boyutlu ve NULL Framebuffer testleri */
    Framebuffer zero_fb = { NULL, NULL, 0, 0, 0, NULL, NULL };
    text_draw(NULL, &atlas, 0, 0, "Test", &st);
    text_draw(&zero_fb, &atlas, 0, 0, "Test", &st);
    u32 dummy_px = 0x11223344u;
    Framebuffer w0_fb = { &dummy_px, NULL, 0, 10, 0, NULL, NULL };
    Framebuffer h0_fb = { &dummy_px, NULL, 10, 0, 0, NULL, NULL };
    text_draw(&w0_fb, &atlas, 0, 0, "Test", &st);
    text_draw(&h0_fb, &atlas, 0, 0, "Test", &st);
    CHECK(dummy_px == 0x11223344u);

    /* 2. ASan tam boyutlu yığın (redzone) tamponunda sınır ve negatif/ekran dışı koordinat kırpma testleri */
    const u32 dims[3][2] = { { 1u, 1u }, { 13u, 7u }, { 64u, 32u } };
    const i32 coords[][2] = {
        { 0, 0 }, { -5, -5 }, { -12, 4 }, { 4, -15 },
        { 60, 28 }, { -500, -500 }, { 500, 500 },
        { I32_MIN, I32_MIN }, { I32_MAX, I32_MAX },
        { I32_MIN + 3, 5 }, { 5, I32_MAX - 3 }
    };

    for (u32 d = 0u; d < 3u; d++) {
        u32 w = dims[d][0];
        u32 h = dims[d][1];
        u32* heap_px = ARENA_ARRAY(test_arena(), u32, (usize)w * h);
        CHECK(heap_px != NULL);
        Framebuffer fb = { heap_px, NULL, (i32)w, (i32)h, 0, NULL, NULL };

        for (u32 c = 0u; c < ARRAY_COUNT(coords); c++) {
            mem_set(heap_px, (u8)(0), (usize)w * (usize)h * sizeof(u32));
            text_draw(&fb, &atlas, coords[c][0], coords[c][1],
                      "Ağaç kökü İğdır'da şöyle\nİkinci satır", &st);
        }

        /* Düşmanca stil değerleri ve bozuk UTF-8 dizileri */
        TextStyle hostile = st;
        hostile.outline_px     = 1000000; /* TEXT_MAX_OUTLINE_PX ile sınırlanmalı */
        hostile.max_width      = 0.5f;    /* tek gliften dar genişlik: sonsuz döngü olmamalı */
        hostile.line_spacing   = -50.0f;
        hostile.letter_spacing = 1e20f;
        const char* bad_utf8 = "\xFF\xFE\x80\xC0\xAF\xE0\x80\xAF\xF0\x80\x80\xAF" "Ağaç\xC4";
        text_draw(&fb, &atlas, -2, -2, bad_utf8, &hostile);

    }
}

TEST(text_visual_hash_and_1000_char_bench) {
    g_arena_mem = (u8*)arena_alloc_nozero(test_arena(), MB(32), 16);
    TtfFont font;
    CHECK(ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font));

    Arena out_a, scratch_a;
    arena_init(&out_a, g_arena_mem, MB(16));
    arena_init(&scratch_a, g_arena_mem + MB(16), MB(16));

    FontAtlas a16, a24, a48;
    CHECK(font_atlas_build(&font, 16.0f, 0u, 0u, &out_a, &scratch_a, &a16));
    CHECK(font_atlas_build(&font, 24.0f, 0u, 0u, &out_a, &scratch_a, &a24));
    CHECK(font_atlas_build(&font, 48.0f, 0u, 0u, &out_a, &scratch_a, &a48));

    /* 1. Görsel kanıt: 3 boyutta (16, 24, 48 px) örnek metin; kare hash'i (shot ile PNG'ye alınabilir) */
    {
        usize mark = arena_mark(&scratch_a);
        Framebuffer fb;
        fb_init(&fb, &scratch_a, 860u, 380u);
        u32 bg = color_pack(18, 22, 32, 255);
        fb_fill(&fb, bg);

        /* 48 px başlık (kontur + gölge) */
        TextStyle st48 = text_style_default(color_pack(255, 240, 180, 255));
        st48.shadow_color  = color_pack(0, 0, 0, 200);
        st48.shadow_dx     = 3;
        st48.shadow_dy     = 3;
        st48.outline_color = color_pack(40, 55, 90, 255);
        st48.outline_px    = 1;
        text_draw(&fb, &a48, 24, 18, "Ağaç kökü İğdır'da şöyle", &st48);

        /* 24 px alt başlık (gölgeli) */
        TextStyle st24 = text_style_default(color_pack(140, 220, 255, 255));
        st24.shadow_color = color_pack(0, 0, 0, 180);
        st24.shadow_dx    = 2;
        st24.shadow_dy    = 2;
        text_draw(&fb, &a24, 24, 88, "24px: Ağaç kökü İğdır'da şöyle «0123456789» ©®°±×÷", &st24);
        text_draw(&fb, &a24, 24, 122, "Pijamalı hasta yağız şoföre çabucak güvendi.", &st24);

        /* 16 px gövde metni (sol / orta / sağ hizalı ve satır kaydırmalı) */
        TextStyle st16 = text_style_default(color_pack(230, 235, 245, 255));
        text_draw(&fb, &a16, 24, 168,
                  "16px: ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz ğĞıİşŞçÇöÖüÜ", &st16);

        TextStyle wrap_l = text_style_default(color_pack(200, 245, 200, 255));
        wrap_l.max_width = 250.0f;
        wrap_l.align     = TEXT_ALIGN_LEFT;
        text_draw(&fb, &a16, 24, 205,
                  "[SOL HİZALI] Ağaç kökü İğdır'da şöyle filizlenir; kendi TrueType fontumuz Matoki Sans ile çizildi.",
                  &wrap_l);

        TextStyle wrap_c = wrap_l;
        wrap_c.color = color_pack(255, 220, 180, 255);
        wrap_c.align = TEXT_ALIGN_CENTER;
        text_draw(&fb, &a16, 305, 205,
                  "[ORTA HİZALI] Ağaç kökü İğdır'da şöyle filizlenir; kelime bölmeden satır kaydırma aktif.",
                  &wrap_c);

        TextStyle wrap_r = wrap_l;
        wrap_r.color = color_pack(220, 200, 255, 255);
        wrap_r.align = TEXT_ALIGN_RIGHT;
        text_draw(&fb, &a16, 585, 205,
                  "[SAĞ HİZALI] Ağaç kökü İğdır'da şöyle filizlenir; delikler (O, a, e, ğ, ö) temiz ve açık.",
                  &wrap_r);

        test_note("metin karesi 860x380 hash", fb_hash(&fb), 0);
        CHECK(fb_hash(&fb) == 543713094u);   /* font / atlas / text_draw değişirse bilinçli güncellenir */
        arena_reset(&scratch_a, mark);
    }

    /* 2. Benchmark: 16 px'te tam 1000 karakterlik (Unicode kod noktası) paragraf çizim süresi */
    const char* unit_sentence = "Ağaç kökü İğdır'da şöyle filizlenir ve rüzgârla salınır. ";
    char para1000[4096];
    u32 byte_pos = 0u;
    u32 cp_count = 0u;
    while (cp_count < 1000u) {
        const char* p = unit_sentence;
        while (*p && cp_count < 1000u) {
            const char* c0 = p;
            (void)font_atlas_utf8_next(&p);
            u32 clen = (u32)(p - c0);
            if (byte_pos + clen < sizeof(para1000) - 1u) {
                mem_copy(para1000 + byte_pos, c0, clen);
                byte_pos += clen;
                cp_count++;
            }
        }
    }
    para1000[byte_pos] = '\0';
    CHECK(cp_count == 1000u);

    Framebuffer bench_fb;
    fb_init(&bench_fb, &scratch_a, 640u, 480u);
    fb_fill(&bench_fb, color_pack(20, 24, 32, 255));

    TextStyle bench_st = text_style_default(color_pack(240, 240, 240, 255));
    bench_st.max_width = 600.0f;

    /* Isınma (5 tur) */
    for (u32 w = 0u; w < 5u; w++) {
        text_draw(&bench_fb, &a16, 20, 20, para1000, &bench_st);
    }

    u32 iters = 50u;
    f64 samples[15];
    for (u32 run = 0u; run < 15u; run++) {
        f64 t0 = sys_now_ms();
        for (u32 it = 0u; it < iters; it++) {
            text_draw(&bench_fb, &a16, 20, 20, para1000, &bench_st);
        }
        samples[run] = (sys_now_ms() - t0) / (f64)iters;
    }
    for (u32 i = 0u; i < 15u; i++) {
        for (u32 j = i + 1u; j < 15u; j++) {
            if (samples[j] < samples[i]) {
                f64 tmp = samples[i]; samples[i] = samples[j]; samples[j] = tmp;
            }
        }
    }
    f64 med_ms = samples[7];
    f64 chars_per_ms = (med_ms > 0.0) ? (1000.0 / med_ms) : 0.0;
    test_note("1000 karakter paragraf @ 16 px (wrap 600): medyan ms", med_ms, 3);
    test_note("  karakter / ms", chars_per_ms, 0);
}
