/* tests/test_matoki_mip.c — mip zinciri yerleşimi, kutu filtresi ve seviye seçimi.
 * Seviye seçimi M8 §2.3'teki tamsayı kuralını (floor(log2(rho))) doğrular; Matoki
 * @mipmap uygulandığında aynı vakalar Matoki ↔ ref_sample_tex_mip karşılaştırmasına girecek. */
#include "test.h"
#include "img/image.h"
#include "raster/ref_shaders.h"

TEST(mip_layout_square) {
    CHECK(img_mip_level_count(256, 256) == 9);
    CHECK(img_mip_level_count(1, 1) == 1);
    CHECK(img_mip_offset(256, 256, 0) == 0);
    CHECK(img_mip_offset(256, 256, 1) == 65536);
    CHECK(img_mip_offset(256, 256, 2) == 65536 + 16384);
    CHECK(img_mip_total(256, 256, 9) == 87381);   /* (4^9 - 1) / 3 */
}

TEST(mip_layout_nonsquare_256x16) {
    CHECK(img_mip_level_count(256, 16) == 9);
    /* 256x16, 128x8, 64x4, 32x2, 16x1, 8x1, 4x1, 2x1, 1x1 */
    static const u32 w[9] = { 256, 128, 64, 32, 16, 8, 4, 2, 1 };
    static const u32 h[9] = { 16, 8, 4, 2, 1, 1, 1, 1, 1 };
    usize off = 0;
    for (u32 k = 0; k < 9; k++) {
        CHECK(img_mip_dim(256, k) == w[k]);
        CHECK(img_mip_dim(16, k) == h[k]);
        CHECK(img_mip_offset(256, 16, k) == off);
        off += (usize)w[k] * h[k];
    }
    CHECK(img_mip_total(256, 16, 9) == off);
    CHECK(img_mip_level_count(16, 256) == 9);
}

TEST(mip_box_filter) {
    Texture* t = tex_create(test_arena(), 2, 2, TEX_FLAG_MIPMAP);
    CHECK(t->levels == 2);
    t->px[0] = color_pack(0, 0, 0, 255);
    t->px[1] = color_pack(255, 100, 8, 255);
    t->px[2] = color_pack(255, 100, 8, 255);
    t->px[3] = color_pack(2, 0, 0, 255);
    tex_build_mips(t);
    /* (0+255+255+2+2)/4 = 128, (0+100+100+0+2)/4 = 50, (0+8+8+0+2)/4 = 4 */
    CHECK(t->px[4] == color_pack(128, 50, 4, 255));

    /* kare olmayan: 4x1 → 2x1 → 1x1 */
    Texture* r = tex_create(test_arena(), 4, 1, TEX_FLAG_MIPMAP);
    CHECK(r->levels == 3);
    r->px[0] = color_pack(10, 0, 0, 0);
    r->px[1] = color_pack(30, 0, 0, 0);
    r->px[2] = color_pack(50, 0, 0, 0);
    r->px[3] = color_pack(70, 0, 0, 0);
    tex_build_mips(r);
    CHECK((r->px[4] & 0xFF) == 20);
    CHECK((r->px[5] & 0xFF) == 60);
    CHECK((r->px[6] & 0xFF) == 40);
}

static u32 level_for_scale(const Texture* t, f32 k) {
    const f32 base = 0.25f;
    f32 u[4] = { base, base + k / (f32)t->w, base, base + k / (f32)t->w };
    f32 v[4] = { base, base, base + k / (f32)t->h, base + k / (f32)t->h };
    return ref_mip_level(t, u, v);
}

TEST(mip_level_selection) {
    Texture* t = tex_create(test_arena(), 256, 256, TEX_FLAG_MIPMAP);
    CHECK(level_for_scale(t, 0.5f) == 0);
    CHECK(level_for_scale(t, 1.0f) == 0);
    CHECK(level_for_scale(t, 2.0f) == 1);
    CHECK(level_for_scale(t, 3.0f) == 1);
    CHECK(level_for_scale(t, 4.0f) == 2);
    CHECK(level_for_scale(t, 8.0f) == 3);
    CHECK(level_for_scale(t, 1024.0f) == 8);   /* levels - 1'e kenetlenir */
    /* uç durumlar */
    CHECK(ref_mip_level_from_rho2(0.0f, 9) == 0);
    CHECK(ref_mip_level_from_rho2(1e-40f, 9) == 0);            /* denormal */
    CHECK(ref_mip_level_from_rho2(m_bits_f32(0x7FC00000u), 9) == 0);   /* NaN */
    CHECK(ref_mip_level_from_rho2(m_bits_f32(0x7F800000u), 9) == 8);   /* +inf */
    CHECK(ref_mip_level_from_rho2(15.99f, 9) == 1);
    CHECK(ref_mip_level_from_rho2(16.0f, 9) == 2);
    CHECK(ref_mip_level_from_rho2(16.0f, 1) == 0);
}

TEST(mip_sample_reads_level_texels) {
    Texture* t = tex_create(test_arena(), 8, 8, TEX_FLAG_MIPMAP);
    for (u32 i = 0; i < img_mip_total(8, 8, t->levels); i++) t->px[i] = 0;
    for (u32 k = 0; k < t->levels; k++) {
        u32* base = t->px + img_mip_offset(8, 8, k);
        u32 n = img_mip_dim(8, k) * img_mip_dim(8, k);
        for (u32 i = 0; i < n; i++) base[i] = 100 + k * 10 + i;
    }
    /* seviye 1 (4x4): uv (0.3, 0.6) → texel (1, 2) → indeks 9 */
    CHECK(ref_sample_level(t, 0.3f, 0.6f, 1) == 110 + 9);
    /* wrap: u = 1.3 → 0.3 */
    CHECK(ref_sample_level(t, 1.3f, 0.6f, 1) == 110 + 9);
    CHECK(ref_sample_level(t, -0.7f, 0.6f, 1) == 110 + 9);
    /* seviye sınırı aşılırsa son seviye */
    CHECK(ref_sample_level(t, 0.5f, 0.5f, 99) == 130);
    /* quad türevi 2 texel/piksel → seviye 1 */
    f32 u[4] = { 0.3f, 0.3f + 2.0f / 8.0f, 0.3f, 0.3f + 2.0f / 8.0f };
    f32 v[4] = { 0.6f, 0.6f, 0.6f + 2.0f / 8.0f, 0.6f + 2.0f / 8.0f };
    u32 out[4];
    ref_sample_tex_mip(t, u, v, out);
    CHECK(out[0] == 110 + 9);
}
