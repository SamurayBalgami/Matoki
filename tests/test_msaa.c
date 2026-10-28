/* tests/test_msaa.c — MSAA 4x: kapsama, çözümleme, ortak kenar, thread bağımsızlığı.
 * Örnek konumları (piksel merkezine göre, piksel): (-0.125,-0.375) (0.375,-0.125) (-0.375,0.125) (0.125,0.375).
 * Tam kaplanan pikselde MSAA = tek örnek (shader merkezde bir kez); kenarda sonuç örnek sayısıyla orantılı. */
#include "test.h"
#include "core/jobs.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "raster/raster.h"
#include "raster/ref_shaders.h"

enum { W = 32, H = 16 };

typedef struct Target {
    u32 color[W * H];
    f32 depth[W * H];
    u32 color_ms[W * H * RASTER_MSAA];
    f32 depth_ms[W * H * RASTER_MSAA];
    Framebuffer fb;
    Raster r;
    Arena bins;
} Target;

static Target* target(b32 msaa) {
    Target* t = ARENA_NEW(test_arena(), Target);
    t->fb = (Framebuffer){ t->color, t->depth, W, H, msaa ? RASTER_MSAA : 1u, t->color_ms, t->depth_ms };
    fb_clear(&t->fb, 0xFF000000u, 1.0f);
    raster_init(&t->r, &t->fb);
    raster_set_cull(&t->r, CULL_NONE);
    raster_set_depth(&t->r, TRUE, TRUE, DEPTH_LESS);
    arena_init(&t->bins, arena_alloc(test_arena(), 1 << 20, 16), 1 << 20);
    raster_begin(&t->r, &t->bins);
    return t;
}

/* sabit renk: uniform = renk */
static void shade_const(const ShadeQuad* q, const void* u, u32 out[4]) {
    UNUSED(q);
    for (u32 l = 0; l < 4; l++) out[l] = *(const u32*)u;
}

/* varying'ten renk (merkezde bir kez gölgelendiği görülsün) */
static void shade_var(const ShadeQuad* q, const void* u, u32 out[4]) {
    UNUSED(u);
    for (u32 l = 0; l < 4; l++) {
        u32 c = (u32)m_f2i_sat(m_clampf(q->var[0][l], 0.0f, 1.0f) * 255.0f);
        out[l] = 0xFF000000u | c | (c << 8);
    }
}

static void tri(Target* t, f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, f32 z) {
    RasterVert v[3];
    mem_zero(v, sizeof(v));
    const f32 xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (u32 k = 0; k < 3; k++) {
        v[k].x = xs[k];
        v[k].y = ys[k];
        v[k].z = z;
        v[k].inv_w = 1.0f;
        v[k].var[0] = xs[k] / (f32)W;
    }
    raster_triangle(&t->r, &v[0], &v[1], &v[2]);
}

static void finish(Target* t) {
    raster_flush(&t->r);
    fb_resolve(&t->fb);
}

TEST(msaa_full_cover_equals_single_sample) {
    Target* a = target(FALSE);
    Target* b = target(TRUE);
    Target* ts[2] = { a, b };
    for (u32 k = 0; k < 2; k++) {
        raster_set_shader(&ts[k]->r, shade_var, NULL, 1);
        tri(ts[k], -40, -40, 200, -40, -40, 200, 0.5f);
        finish(ts[k]);
    }
    CHECK(mem_equal(a->color, b->color, sizeof(a->color)));
}

TEST(msaa_vertical_edge_coverage) {
    /* x < 10.3 kaplı: piksel 10'un örnek x'leri 10.375 10.875 10.125 10.625 → yalnız 1 örnek */
    Target* t = target(TRUE);
    u32 white = 0xFFFFFFFFu;
    raster_set_shader(&t->r, shade_const, &white, 0);
    tri(t, -10, -10, 10.3f, -10, 10.3f, 100, 0.5f);
    tri(t, -10, -10, 10.3f, 100, -10, 100, 0.5f);
    finish(t);
    u32 bad = 0;
    for (i32 y = 0; y < H; y++) {
        for (i32 x = 0; x < W; x++) {
            u32 c = t->color[y * W + x];
            u32 want = x < 10 ? 0xFFFFFFFFu : x == 10 ? 0xFF404040u : 0xFF000000u;   /* (255 + 0·3 + 2) >> 2 = 64 */
            bad += c != want;
        }
    }
    CHECK(bad == 0);
}

TEST(msaa_shared_edge_no_gaps) {
    /* rastgele eğik ortak kenarlı dörtgenler: iki üçgenin birleşiminde hiçbir örnek arka planda kalmamalı */
    Rng rng = rng_make(0x5EED);
    u32 holes = 0;
    for (u32 it = 0; it < 40; it++) {
        Target* t = target(TRUE);
        u32 ca = 0xFF0000FFu, cb = 0xFF00FF00u;
        f32 mx = rng_range(&rng, 8, 24), my = rng_range(&rng, 4, 12);   /* ortak kenar: köşeden (mx,my)'ye */
        raster_set_shader(&t->r, shade_const, &ca, 0);
        tri(t, -5, -5, mx, my, 40, -5, 0.5f);
        tri(t, -5, -5, -5, 30, mx, my, 0.5f);
        raster_set_shader(&t->r, shade_const, &cb, 0);
        tri(t, 40, -5, mx, my, 40, 30, 0.5f);
        tri(t, -5, 30, 40, 30, mx, my, 0.5f);
        finish(t);
        for (u32 i = 0; i < W * H * RASTER_MSAA; i++) holes += t->color_ms[i] == 0xFF000000u;
    }
    CHECK(holes == 0);
}

TEST(msaa_parallel_bit_exact) {
    /* rastgele üçgenler, derinlik testi: tek thread ve tüm thread'ler aynı örnek tamponları */
    Target* ts[2];
    for (u32 k = 0; k < 2; k++) {
        ts[k] = target(TRUE);
        jobs_set_limit(k == 0 ? 1 : 0);
        Rng rng = rng_make(77);
        raster_set_shader(&ts[k]->r, shade_var, NULL, 1);
        for (u32 i = 0; i < 400; i++) {
            f32 cx = rng_range(&rng, -4, W + 4), cy = rng_range(&rng, -4, H + 4);
            tri(ts[k], cx + rng_range(&rng, -9, 9), cy + rng_range(&rng, -9, 9), cx + rng_range(&rng, -9, 9),
                cy + rng_range(&rng, -9, 9), cx + rng_range(&rng, -9, 9), cy + rng_range(&rng, -9, 9), rng_f32(&rng));
        }
        finish(ts[k]);
        jobs_set_limit(0);
    }
    CHECK(mem_equal(ts[0]->color_ms, ts[1]->color_ms, sizeof(ts[0]->color_ms)));
    CHECK(mem_equal(ts[0]->depth_ms, ts[1]->depth_ms, sizeof(ts[0]->depth_ms)));
    CHECK(mem_equal(ts[0]->color, ts[1]->color, sizeof(ts[0]->color)));
}

TEST(msaa_simd_matches_scalar) {
    /* aynı üçgen akışı, raster modu skaler ↔ SIMD: örnek renk + derinlik tamponları bit-bit aynı */
    Target* ts[2];
    for (u32 k = 0; k < 2; k++) {
        ts[k] = target(TRUE);
        raster_set_mode(&ts[k]->r, k == 0 ? RASTER_MODE_SCALAR : RASTER_MODE_SIMD);
        Rng rng = rng_make(0x51D);
        raster_set_shader(&ts[k]->r, shade_var, NULL, 1);
        for (u32 i = 0; i < 600; i++) {
            f32 cx = rng_range(&rng, -4, W + 4), cy = rng_range(&rng, -4, H + 4);
            f32 sz = (i % 9 == 0) ? 40.0f : 6.0f;
            tri(ts[k], cx + rng_range(&rng, -sz, sz), cy + rng_range(&rng, -sz, sz), cx + rng_range(&rng, -sz, sz),
                cy + rng_range(&rng, -sz, sz), cx + rng_range(&rng, -sz, sz), cy + rng_range(&rng, -sz, sz), rng_f32(&rng));
        }
        finish(ts[k]);
    }
    CHECK(mem_equal(ts[0]->color_ms, ts[1]->color_ms, sizeof(ts[0]->color_ms)));
    CHECK(mem_equal(ts[0]->depth_ms, ts[1]->depth_ms, sizeof(ts[0]->depth_ms)));
}