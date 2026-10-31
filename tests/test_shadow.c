/* tests/test_shadow.c — M13 faz 7: gölge haritası (gfx3d/shadow) ve raster'ın yalnız derinlik yolu.
 * Küp zeminin 1 m üstünde, güneş tepede: haritada küpün izi yakın derinlik, çevresi temiz (1); küpün altındaki zemin
 * noktası haritadakinden uzak (gölgede), yanındaki nokta değil. Yalnız derinlik çizimi skaler = SIMD, renk yazmaz. */
#include "test.h"
#include "core/mem.h"
#include "gfx3d/shadow.h"

static Pipeline g_pipe;

static void draw_cube_shadow(ShadowMap* sm, Arena* frame, Arena* bins) {
    Mesh* cube = mesh_cube(test_arena());
    mesh_compute_bounds(cube);
    shadow_begin(sm, &g_pipe, frame, bins);
    DrawItem d = { .mesh = cube, .mat = shadow_caster(), .model = mat4_translate(v3(0.0f, 1.5f, 0.0f)) };
    pipe_draw(&g_pipe, &d);
    shadow_end(sm, &g_pipe);
}

/* dünya noktası → harita: (u, v, derinlik) ve o texel'deki kayıtlı derinlik */
static f32 lookup(const ShadowMap* sm, Vec3 p, f32* own) {
    Vec4 s = mat4_mul_v4(&sm->world_to_tex, v4(p.x, p.y, p.z, 1.0f));
    *own = s.z;
    i32 x = (i32)m_floorf(s.x * (f32)sm->size), y = (i32)m_floorf(s.y * (f32)sm->size);
    if (x < 0 || y < 0 || x >= (i32)sm->size || y >= (i32)sm->size) return -1.0f;
    return sm->fb.depth[y * (i32)sm->size + x];
}

TEST(shadow_map_cube_over_ground) {
    Arena* a = test_arena();
    Arena frame, bins;
    arena_init(&frame, arena_alloc(a, 4 << 20, 16), 4 << 20);
    arena_init(&bins, arena_alloc(a, 8 << 20, 16), 8 << 20);
    ShadowMap* sm = ARENA_NEW(a, ShadowMap);
    shadow_init(sm, a, 256);
    shadow_fit(sm, v3_norm(v3(0.2f, 1.0f, 0.1f)), v3(0, 0, 0), 5.0f, 10.0f);
    draw_cube_shadow(sm, &frame, &bins);

    u32 written = 0;
    for (u32 i = 0; i < 256u * 256u; i++) written += sm->fb.depth[i] < 1.0f;
    CHECK(written > 100u);
    test_note("küp izi (texel)", (f64)written, 0);

    f32 own, rec;
    rec = lookup(sm, v3(0.0f, 0.0f, 0.0f), &own);        /* küpün altı */
    CHECK(rec >= 0.0f && own > rec + 0.01f);
    rec = lookup(sm, v3(3.5f, 0.0f, 3.5f), &own);        /* açıkta */
    CHECK(rec >= 0.0f && own < rec);
    /* küpün kendi üst yüzü (ışığa en yakın) gölgede sayılmaz: kayıtlı derinlik ≈ kendi derinliği */
    rec = lookup(sm, v3(0.0f, 2.0f, 0.0f), &own);
    CHECK(own <= rec + 0.01f);

    /* yalnız derinlik yolu: skaler ve SIMD raster bit-bit aynı derinlik yazar (renk tamponu yok: yazılsaydı çökerdi) */
    f32* simd = ARENA_ARRAY(a, f32, 256u * 256u);
    mem_copy(simd, sm->fb.depth, sizeof(f32) * 256u * 256u);
    raster_set_mode(&sm->raster, RASTER_MODE_SCALAR);
    arena_reset(&frame, 0);
    arena_reset(&bins, 0);
    draw_cube_shadow(sm, &frame, &bins);
    CHECK(mem_equal(simd, sm->fb.depth, sizeof(f32) * 256u * 256u));
}

/* ayna / eksen hatası yakalayıcı: küp merkezde değil (x = 2, z = 1, y ∈ [1, 2]), demo güneşi eğik (0,4, 0,8, 0,35).
 * Zemindeki gölge analitik: p_yer = p − (sun.x / sun.y, ·, sun.z / sun.y) · y. x = 1,23 doğrusunda z ∈ [−0,375, 1,0625],
 * z = 0,33 doğrusunda x ∈ [0,5, 2,0]. Tarama sınırları 0,1 m (≈ 2 texel) içinde tutmalı; aynalı harita tutturamaz. */
static void scan(const ShadowMap* sm, b32 along_z, f32 fixed, f32* lo, f32* hi) {
    *lo = 1e9f;
    *hi = -1e9f;
    for (i32 k = -400; k <= 400; k++) {
        f32 t = (f32)k * 0.01f;
        Vec3 p = along_z ? v3(fixed, 0.0f, t) : v3(t, 0.0f, fixed);
        f32 own, rec = lookup(sm, p, &own);
        if (rec >= 0.0f && own > rec + 0.01f) {
            if (t < *lo) *lo = t;
            if (t > *hi) *hi = t;
        }
    }
}

TEST(shadow_map_offcenter_matches_analytic) {
    Arena* a = test_arena();
    Arena frame, bins;
    arena_init(&frame, arena_alloc(a, 4 << 20, 16), 4 << 20);
    arena_init(&bins, arena_alloc(a, 8 << 20, 16), 8 << 20);
    ShadowMap* sm = ARENA_NEW(a, ShadowMap);
    shadow_init(sm, a, 256);
    Vec3 sun = v3_norm(v3(0.4f, 0.8f, 0.35f));
    shadow_fit(sm, sun, v3(0, 0, 0), 6.0f, 10.0f);
    Mesh* cube = mesh_cube(test_arena());
    mesh_compute_bounds(cube);
    shadow_begin(sm, &g_pipe, &frame, &bins);
    DrawItem d = { .mesh = cube, .mat = shadow_caster(), .model = mat4_translate(v3(2.0f, 1.5f, 1.0f)) };
    pipe_draw(&g_pipe, &d);
    shadow_end(sm, &g_pipe);
    f32 kx = sun.x / sun.y, kz = sun.z / sun.y;
    f32 lo, hi;
    scan(sm, TRUE, 1.23f, &lo, &hi);
    CHECK_NEAR(lo, 0.5f - 2.0f * kz, 0.1);
    CHECK_NEAR(hi, 1.5f - 1.0f * kz, 0.1);
    scan(sm, FALSE, 0.33f, &lo, &hi);
    CHECK_NEAR(lo, 1.5f - 2.0f * kx, 0.1);
    CHECK_NEAR(hi, 2.5f - 1.0f * kx, 0.1);
}
