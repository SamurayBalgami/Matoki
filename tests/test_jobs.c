/* tests/test_jobs.c — iş sistemi ve paralel raster.
 * jobs_parallel_for: her indeks tam bir kez, birden çok thread katılıyor (run.mjs worker başlatır).
 * Raster: aynı üçgen akışı tek thread (jobs_set_limit(1)) ve tüm thread'lerle → renk ve derinlik
 * bit-bit aynı; C SIMD shader ve önsözlü + discard'lı Matoki shader (iki uniform bloğu iç içe). */
#include "test.h"
#include "core/jobs.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "img/procgen.h"
#include "matoki/matoki.h"
#include "raster/raster.h"
#include "raster/ref_shaders.h"

typedef struct CountCtx {
    u32 hits[4096];
    u32 by_tid[JOBS_MAX_THREADS];
} CountCtx;

static void count_job(void* ctx, u32 index, u32 tid) {
    CountCtx* c = (CountCtx*)ctx;
    __atomic_fetch_add(&c->hits[index], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->by_tid[tid], 1u, __ATOMIC_RELAXED);
    /* biraz iş: thread'ler gerçekten paylaşsın */
    volatile f32 x = 0.0f;
    for (u32 i = 0; i < 2000; i++) x += m_sinf((f32)i);
}

TEST(jobs_parallel_for_each_index_once) {
    CountCtx* c = ARENA_NEW(test_arena(), CountCtx);
    u32 bad = 0;
    for (u32 round = 0; round < 20; round++) {
        mem_zero(c, sizeof(*c));
        u32 n = 1 + round * 200;
        jobs_parallel_for(count_job, c, n);
        for (u32 i = 0; i < n; i++) bad += c->hits[i] != 1;
    }
    u32 tids = 0;
    for (u32 t = 0; t < JOBS_MAX_THREADS; t++) tids += c->by_tid[t] > 0;
    test_note("thread", jobs_threads(), 0);
    test_note("son turda katılan thread", tids, 0);
    CHECK(bad == 0);
    if (jobs_threads() > 1) CHECK(tids > 1);
}

typedef struct Scene {
    RasterVert* v;
    u32         tris;
    u32*        mat;       /* üçgen başına materyal 0..2 */
} Scene;

static Scene make_scene(u32 tris, u32 w, u32 h) {
    Scene s;
    s.tris = tris;
    s.v = ARENA_ARRAY(test_arena(), RasterVert, tris * 3);
    s.mat = ARENA_ARRAY(test_arena(), u32, tris);
    Rng r = rng_make(0x7A11);
    for (u32 t = 0; t < tris; t++) {
        f32 cx = rng_range(&r, -20, (f32)w + 20), cy = rng_range(&r, -20, (f32)h + 20);
        f32 size = (t % 7 == 0) ? 120.0f : 25.0f;
        s.mat[t] = t % 3;
        for (u32 k = 0; k < 3; k++) {
            RasterVert* v = &s.v[t * 3 + k];
            mem_zero(v, sizeof(*v));
            v->x = cx + rng_range(&r, -size, size);
            v->y = cy + rng_range(&r, -size, size);
            v->z = rng_range(&r, 0.05f, 0.95f);
            v->inv_w = rng_range(&r, 0.5f, 2.0f);
            for (u32 i = 0; i < VAR3D_COUNT; i++) v->var[i] = rng_range(&r, -3, 3) * v->inv_w;
        }
    }
    return s;
}

typedef struct DrawSet {
    ShadeFn     shade;
    PreludeFn   prelude;
    const void* uni[3];
} DrawSet;

static void render(const Scene* s, const DrawSet* d, u32* color, f32* depth, u32 w, u32 h, u32 threads) {
    Framebuffer fb = { color, depth, (i32)w, (i32)h };
    fb_clear(&fb, 0xFF332211u, 1.0f);
    Raster r;
    raster_init(&r, &fb);
    raster_set_cull(&r, CULL_NONE);
    raster_set_depth(&r, TRUE, TRUE, DEPTH_LESS);
    Arena bins;
    /* küçük arena → ara flush'lar da paralel yoldan geçer */
    arena_init(&bins, arena_alloc(test_arena(), 256 << 10, 16), 256 << 10);
    raster_begin(&r, &bins);
    jobs_set_limit(threads);
    for (u32 t = 0; t < s->tris; t++) {
        raster_set_shader(&r, d->shade, d->uni[s->mat[t]], VAR3D_COUNT);
        if (d->prelude) raster_set_prelude(&r, d->prelude);
        raster_triangle(&r, &s->v[t * 3], &s->v[t * 3 + 1], &s->v[t * 3 + 2]);
    }
    raster_flush(&r);
    jobs_set_limit(0);
}

static void compare_threads(TestCtx* t_, const Scene* s, const DrawSet* d, const char* label) {
    enum { W = 320, H = 200 };
    u32* c1 = ARENA_ARRAY(test_arena(), u32, W * H);
    u32* cn = ARENA_ARRAY(test_arena(), u32, W * H);
    f32* d1 = ARENA_ARRAY(test_arena(), f32, W * H);
    f32* dn = ARENA_ARRAY(test_arena(), f32, W * H);
    render(s, d, c1, d1, W, H, 1);
    render(s, d, cn, dn, W, H, 0);
    u32 bad = 0, painted = 0;
    for (u32 i = 0; i < W * H; i++) {
        bad += c1[i] != cn[i] || m_f32_bits(d1[i]) != m_f32_bits(dn[i]);
        painted += c1[i] != 0xFF332211u;
    }
    test_note(label, painted, 0);
    CHECK(bad == 0);
    CHECK(painted > W * H / 4);
}

TEST(jobs_raster_parallel_bit_exact) {
    Arena* a = test_arena();
    Scene s = make_scene(3000, 320, 200);
    Texture* tex = tex_create(a, 64, 64, TEX_FLAG_MIPMAP);
    procgen_bricks(tex, 3);
    tex_build_mips(tex);
    LitUniforms* lu = ARENA_ARRAY(a, LitUniforms, 3);
    for (u32 k = 0; k < 3; k++) {
        lu[k].tex = tex;
        lu[k].to_light = v3_norm(v3(0.3f, 0.8f, 0.2f + (f32)k * 0.2f));
        lu[k].ambient = 0.3f;
        lu[k].light_color = v3(0.9f, 0.8f, 0.7f);
        lu[k].fog_start = 1.0f;
        lu[k].fog_end = 9.0f + (f32)k;
        lu[k].fog_color = v3(0.5f, 0.6f, 0.7f);
        lu[k].use_mip = TRUE;
        lu[k].tint = v3(1.0f, 0.5f + (f32)k * 0.2f, 1.0f);
        lit_uniforms_prepare(&lu[k]);
    }
    DrawSet dc = { shade_lit_tex_simd, NULL, { &lu[0], &lu[1], &lu[2] } };
    compare_threads(t_, &s, &dc, "C SIMD: boyanan piksel");

    /* Matoki: önsöz (uniform'dan türeyen ölçek) + discard; üç uniform bloğu üçgenler arasında iç içe */
    const char* src =
        "uniform tint: vec3;\nuniform k: f32;\nvarying p: vec3;\nvarying n: vec3;\nvarying uv: vec2;\n"
        "fn frag() -> vec4 {\n"
        "  let s = 1.0 / max(k * k, 0.25);\n"
        "  if (fract(uv.x * 2.0) < 0.15) { discard; }\n"
        "  return vec4(fract(abs(n) * s) * tint, 1.0);\n"
        "}\n";
    MatokiProgram p;
    MatokiError e;
    b32 ok = matoki_compile(a, src, (u32)cstr_len(src), NULL, &p, &e) && matoki_jit(a, &p, &e);
    CHECK(ok);
    if (!ok) return;
    CHECK(p.wasm.hoisted > 0);
    struct { f32 tint[3]; f32 k; } mu[3] = { { { 1, 0.5f, 0.2f }, 1.0f }, { { 0.2f, 1, 0.5f }, 2.0f }, { { 0.5f, 0.2f, 1 }, 0.7f } };
    DrawSet dm = { p.fn_fast, p.prelude, { &mu[0], &mu[1], &mu[2] } };
    compare_threads(t_, &s, &dm, "Matoki: boyanan piksel");
}
