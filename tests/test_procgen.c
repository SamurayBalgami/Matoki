/* tests/test_procgen.c — malzeme dokuları (D52): kenardan kenara dikişsiz (tekrar eden yüzeyde çizgi çıkmaz), aynı seed aynı doku,
 * derz taştan koyu, kabartı parlaklıktan okunduğu için harç tuğladan koyu olmalı. Bulutun yoğunluk ve aydınlık kanalları dolu. */
#include "test.h"
#include "img/image.h"
#include "img/procgen.h"
#include "core/math.h"

static f32 luma(u32 c) {
    return (f32)(c & 0xFFu) * 0.3f + (f32)((c >> 8) & 0xFFu) * 0.6f + (f32)((c >> 16) & 0xFFu) * 0.1f;
}

/* satır geçişi başına ortalama fark: içerideki en büyük geçiş ve son → ilk satır geçişi */
static f32 row_step(const Texture* t, u32 y0, u32 y1) {
    f32 a = 0.0f;
    for (u32 x = 0; x < t->w; x++) a += m_absf(luma(t->px[y0 * t->w + x]) - luma(t->px[y1 * t->w + x]));
    return a / (f32)t->w;
}

static void row_steps(const Texture* t, f32* inner, f32* seam) {
    *inner = 0.0f;
    for (u32 y = 0; y + 1u < t->h; y++) *inner = m_maxf(*inner, row_step(t, y, y + 1u));
    *seam = row_step(t, t->h - 1u, 0u);
}

TEST(procgen_rock_seamless_and_deterministic) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 128, 128, 0);
    Texture* u = tex_create(a, 128, 128, 0);
    procgen_rock(t, 5);
    procgen_rock(u, 5);
    b32 same = TRUE;
    for (u32 i = 0; i < 128u * 128u; i++) same = same && t->px[i] == u->px[i];
    CHECK(same);
    f32 inner, seam;
    row_steps(t, &inner, &seam);
    CHECK(seam <= inner * 1.2f);
}

TEST(procgen_grass_cloth_paint_seamless) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 128, 128, 0);
    f32 inner, seam;
    procgen_grass(t, 3);
    row_steps(t, &inner, &seam);
    CHECK(seam <= inner * 1.2f);
    procgen_cloth(t, 3);
    row_steps(t, &inner, &seam);
    CHECK(seam <= inner * 1.2f);
    /* boya kenarda kirlenir ama iki kenar aynı: geçişte sıçrama yok */
    procgen_paint(t, 3);
    row_steps(t, &inner, &seam);
    CHECK(seam <= inner * 1.2f);
}

TEST(procgen_pavers_joint_darker_than_slab) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 256, 256, 0);
    procgen_pavers(t, 8, 9);
    /* 32 piksellik taş: sıra başı derz, taş ortası */
    f32 joint = 0.0f, slab = 0.0f;
    for (u32 x = 0; x < 256; x++) {
        joint += luma(t->px[32u * 256u + x]);
        slab += luma(t->px[(32u + 16u) * 256u + x]);
    }
    CHECK(joint < slab * 0.8f);
}

TEST(procgen_bricks_mortar_recessed) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 128, 128, 0);
    procgen_bricks(t, 7);
    f32 mortar = 0.0f, brick = 0.0f;
    for (u32 x = 0; x < 128; x++) {
        mortar += luma(t->px[16u * 128u + x]);
        brick += luma(t->px[(16u + 8u) * 128u + x]);
    }
    CHECK(mortar < brick);
}

TEST(procgen_clouds_channels_span) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 128, 128, TEX_FLAG_BILINEAR);
    procgen_clouds(t, 61, 0.4f, 0.35f);
    u32 lo_d = 255, hi_d = 0, lo_l = 255, hi_l = 0;
    for (u32 i = 0; i < 128u * 128u; i++) {
        u32 d = t->px[i] & 0xFFu, l = (t->px[i] >> 8) & 0xFFu;
        lo_d = MIN(lo_d, d);
        hi_d = MAX(hi_d, d);
        lo_l = MIN(lo_l, l);
        hi_l = MAX(hi_l, l);
    }
    /* shader eşiği 0,5–0,68 (128–173): hem açık gök hem dolu bulut var */
    CHECK(lo_d < 128u);
    CHECK(hi_d > 173u);
    CHECK(hi_l - lo_l > 60u);
}
