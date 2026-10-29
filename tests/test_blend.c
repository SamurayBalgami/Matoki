/* tests/test_blend.c — rasterizer renk karışımı (BlendMode, M13 faz 2).
 * Formül kesinliği (bağımsız f64 referansla), 4 yolun (skaler / SIMD × tek örnek / MSAA) bit-bit aynılığı,
 * 1 thread ↔ tüm thread'ler aynılığı (karo içi sıra gönderim sırası), derinlik testiyle birlikte karışım,
 * NONE'un eski yolu değiştirmemesi ve batch2d'nin düz alfa sprite'ı. */
#include "test.h"
#include "core/jobs.h"
#include "core/mem.h"
#include "core/rng.h"
#include "gfx2d/batch2d.h"
#include "img/image.h"
#include "raster/raster.h"

/* var0..3 = r, g, b, a (0..255), köşelerden doğrusal */
static void shade_rgba(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    UNUSED(uniforms);
    for (u32 l = 0; l < 4; l++) {
        u32 c[4];
        for (u32 k = 0; k < 4; k++) c[k] = (u32)m_f2i_sat(m_clampf(q->var[k][l], 0.0f, 255.0f) + 0.5f);
        out[l] = color_pack(c[0], c[1], c[2], c[3]);
    }
}

typedef struct BlendEnv {
    Framebuffer fb;
    Raster r;
    Arena bins;
} BlendEnv;

static void env_init(BlendEnv* e, i32 w, i32 h, b32 msaa, RasterMode mode) {
    Arena* a = test_arena();
    mem_zero(e, sizeof(*e));
    e->fb.w = w;
    e->fb.h = h;
    e->fb.color = ARENA_ARRAY(a, u32, w * h);
    e->fb.depth = ARENA_ARRAY(a, f32, w * h);
    if (msaa) {
        e->fb.samples = RASTER_MSAA;
        e->fb.color_ms = ARENA_ARRAY(a, u32, w * h * RASTER_MSAA);
        e->fb.depth_ms = ARENA_ARRAY(a, f32, w * h * RASTER_MSAA);
    }
    fb_clear(&e->fb, 0xFF402010u, 1.0f);
    arena_init(&e->bins, arena_alloc(a, 8 << 20, 16), 8 << 20);
    raster_init(&e->r, &e->fb);
    raster_set_mode(&e->r, mode);
    raster_set_cull(&e->r, CULL_NONE);
    raster_set_shader(&e->r, shade_rgba, NULL, 4);
    raster_begin(&e->r, &e->bins);
}

static RasterVert vert(f32 x, f32 y, f32 z, const f32 c[4]) {
    RasterVert v;
    mem_zero(&v, sizeof(v));
    v.x = x;
    v.y = y;
    v.z = z;
    v.inv_w = 1.0f;
    for (u32 k = 0; k < 4; k++) v.var[k] = c[k];
    return v;
}

static void quad(BlendEnv* e, f32 x0, f32 y0, f32 x1, f32 y1, f32 z, const f32 c[4]) {
    RasterVert a = vert(x0, y0, z, c), b = vert(x1, y0, z, c), d = vert(x1, y1, z, c), f = vert(x0, y1, z, c);
    raster_triangle(&e->r, &a, &b, &d);
    raster_triangle(&e->r, &a, &d, &f);
}

/* bağımsız referans (f64, yarım yukarı yuvarlama) */
static u32 ref_blend(BlendMode m, u32 s, u32 d) {
    f64 a = (f64)(s >> 24);
    u32 r = 0;
    for (u32 k = 0; k < 32; k += 8) {
        f64 sc = (f64)((s >> k) & 0xFF), dc = (f64)((d >> k) & 0xFF), c;
        if (m == BLEND_ALPHA) c = k == 24 ? a + (f64)(u32)(dc * (255.0 - a) / 255.0 + 0.5) : (sc * a + dc * (255.0 - a)) / 255.0;
        else if (m == BLEND_PREMUL) c = sc + (f64)(u32)(dc * (255.0 - a) / 255.0 + 0.5);
        else c = sc + dc;
        u32 ci = (u32)(c + (m == BLEND_ALPHA && k != 24 ? 0.5 : 0.0));
        r |= (ci > 255 ? 255 : ci) << k;
    }
    return r;
}

TEST(blend_formula_exact_all_alphas) {
    /* her alfa (0..255) ve 3 mod: 16×16 karede her piksel farklı alfa */
    static const BlendMode MODES[3] = { BLEND_ALPHA, BLEND_PREMUL, BLEND_ADD };
    u32 bad = 0;
    for (u32 mi = 0; mi < 3; mi++) {
        BlendEnv e;
        env_init(&e, 16, 16, FALSE, RASTER_MODE_SCALAR);
        raster_set_depth(&e.r, FALSE, FALSE, DEPTH_ALWAYS);
        raster_set_blend(&e.r, MODES[mi]);
        for (u32 i = 0; i < 256; i++) {
            f32 c[4] = { (f32)((i * 37) & 255), (f32)((i * 91) & 255), (f32)((i * 13) & 255), (f32)i };
            quad(&e, (f32)(i % 16), (f32)(i / 16), (f32)(i % 16 + 1), (f32)(i / 16 + 1), 0.5f, c);
        }
        raster_flush(&e.r);
        for (u32 i = 0; i < 256; i++) {
            u32 s = color_pack((i * 37) & 255, (i * 91) & 255, (i * 13) & 255, i);
            if (e.fb.color[i] != ref_blend(MODES[mi], s, 0xFF402010u)) bad++;
        }
    }
    test_note("karisim: referanstan farkli piksel (3 mod × 256 alfa)", bad, 0);
    CHECK(bad == 0);
    /* uç değerler */
    BlendEnv e;
    env_init(&e, 4, 4, FALSE, RASTER_MODE_SCALAR);
    raster_set_depth(&e.r, FALSE, FALSE, DEPTH_ALWAYS);
    raster_set_blend(&e.r, BLEND_ALPHA);
    f32 opaque[4] = { 9, 8, 7, 255 }, clear[4] = { 9, 8, 7, 0 };
    quad(&e, 0, 0, 2, 4, 0.5f, opaque);
    quad(&e, 2, 0, 4, 4, 0.5f, clear);
    raster_flush(&e.r);
    CHECK(e.fb.color[0] == color_pack(9, 8, 7, 255));    /* a = 255 → kaynak */
    CHECK(e.fb.color[3] == 0xFF402010u);                 /* a = 0 → hedef aynen */
    raster_set_blend(&e.r, (BlendMode)99);               /* geçersiz → NONE */
    CHECK(e.r.blend == BLEND_NONE);
}

/* üst üste 400 yarı saydam üçgen (rastgele z, derinlik testi açık, yazma kapalı) */
static u32 overlap_scene(b32 msaa, RasterMode mode, BlendMode blend, u32 threads) {
    jobs_set_limit(threads);
    usize mark = arena_mark(test_arena());   /* 26 sahne × 8 MB: her sahne kendi belleğini geri verir */
    BlendEnv e;
    env_init(&e, 96, 72, msaa, mode);
    raster_set_depth(&e.r, TRUE, FALSE, DEPTH_LESS);
    f32 op[4] = { 30, 200, 90, 255 };
    raster_set_blend(&e.r, BLEND_NONE);
    quad(&e, 10, 10, 60, 50, 0.5f, op);   /* derinlik yazan opak zemin yok: yazma kapalı → yalnız renk */
    raster_set_blend(&e.r, blend);
    Rng r = rng_make(2025);
    for (u32 i = 0; i < 400; i++) {
        f32 c[4] = { rng_f32(&r) * 255.0f, rng_f32(&r) * 255.0f, rng_f32(&r) * 255.0f, rng_f32(&r) * 255.0f };
        f32 c2[4] = { rng_f32(&r) * 255.0f, c[1], rng_f32(&r) * 255.0f, rng_f32(&r) * 255.0f };
        RasterVert a = vert(rng_f32(&r) * 110.0f - 7.0f, rng_f32(&r) * 86.0f - 7.0f, rng_f32(&r), c);
        RasterVert b = vert(rng_f32(&r) * 110.0f - 7.0f, rng_f32(&r) * 86.0f - 7.0f, rng_f32(&r), c2);
        RasterVert d = vert(rng_f32(&r) * 110.0f - 7.0f, rng_f32(&r) * 86.0f - 7.0f, rng_f32(&r), c);
        raster_triangle(&e.r, &a, &b, &d);
    }
    raster_flush(&e.r);
    if (msaa) fb_resolve(&e.fb);
    jobs_set_limit(0);
    u32 h = 0x811c9dc5u;
    usize n = (usize)e.fb.w * (usize)e.fb.h;
    for (usize i = 0; i < n; i++) h = (h ^ e.fb.color[i]) * 0x01000193u;
    if (msaa) {
        for (usize i = 0; i < n * RASTER_MSAA; i++) h = (h ^ e.fb.color_ms[i]) * 0x01000193u;
    }
    arena_reset(test_arena(), mark);
    return h;
}

TEST(blend_paths_bit_exact_and_threads) {
    static const BlendMode MODES[4] = { BLEND_NONE, BLEND_ALPHA, BLEND_PREMUL, BLEND_ADD };
    u32 mism = 0;
    for (u32 m = 0; m < 4; m++) {
        for (u32 ms = 0; ms < 2; ms++) {
            u32 ref = overlap_scene(ms, RASTER_MODE_SCALAR, MODES[m], 1);
            if (overlap_scene(ms, RASTER_MODE_SIMD, MODES[m], 1) != ref) mism++;
            if (overlap_scene(ms, RASTER_MODE_SIMD, MODES[m], 0) != ref) mism++;
            if (overlap_scene(ms, RASTER_MODE_SCALAR, MODES[m], 0) != ref) mism++;
        }
    }
    test_note("karisim: thread sayisi", jobs_threads(), 0);
    test_note("karisim: skaler/SIMD × 1T/tum × MSAA uyusmazlik", mism, 0);
    CHECK(mism == 0);
    /* karışım sahneyi gerçekten değiştiriyor (NONE ↔ ALPHA farklı) */
    CHECK(overlap_scene(FALSE, RASTER_MODE_SIMD, BLEND_NONE, 0) != overlap_scene(FALSE, RASTER_MODE_SIMD, BLEND_ALPHA, 0));
}

TEST(blend_batch2d_alpha_sprite) {
    BlendEnv e;
    env_init(&e, 16, 16, FALSE, RASTER_MODE_SIMD);
    raster_set_depth(&e.r, FALSE, FALSE, DEPTH_ALWAYS);
    Texture* t = tex_create(test_arena(), 1, 1, TEX_FLAG_CLAMP);
    t->px[0] = color_pack(255, 255, 255, 255);
    Arena frame;
    arena_init(&frame, arena_alloc(test_arena(), 1 << 16, 16), 1 << 16);
    Batch2D b;
    b2d_begin(&b, &e.r, &frame, mat3_identity());
    b2d_set_blend(&b, B2D_ALPHA);
    b2d_sprite(&b, t, rect(0, 0, 1, 1), v2(8, 8), v2(8, 8), 0.0f, v4(1, 1, 1, 0.5f));
    b2d_end(&b);
    CHECK(e.r.blend == BLEND_NONE);   /* b2d_end raster ayarını geri koydu */
    /* s = (255,255,255,128) over d = (0x10,0x20,0x40,0xFF) */
    CHECK(e.fb.color[8 * 16 + 8] == ref_blend(BLEND_ALPHA, color_pack(255, 255, 255, 128), 0xFF402010u));
    CHECK(e.fb.color[0] == 0xFF402010u);
}
