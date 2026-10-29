/* tests/test_robust.c — temel sağlamlık (M12 Faz 1–2): bellek baskısı ve rasterizer fuzz.
 * Bellek: arena_try / taşma; küçücük raster arenasında ara flush'lar sırayı ve kaplamayı bozmaz; tablo bile sığmazsa
 * üçgenler sayılıp atlanır; boru hattında çizim sınırı / kare belleği dolunca çizim atlanır (çökme yok).
 * Fuzz: rastgele (dev, dejenere, NaN, sonsuz) üçgenlerin kaplaması i64 kesin referansla (24.8 sabit nokta +
 * top-left) piksel piksel; SIMD = skaler (MSAA açık/kapalı); tampon dışına yazma yok (nöbetçi değerler). */
#include "test.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "gfx3d/pipeline.h"

#define CANARY 0xDEADBEEFu
#define PAD    64

/* ---- ortak: nöbetçili framebuffer ---- */

typedef struct RFb {
    Framebuffer fb;
    u32* color_raw;
    u32* ms_raw;
    usize n;
} RFb;

static void rfb_init(RFb* f, i32 w, i32 h, b32 msaa) {
    Arena* a = test_arena();
    f->n = (usize)w * (usize)h;
    f->color_raw = ARENA_ARRAY(a, u32, f->n + 2 * PAD);
    f->ms_raw = ARENA_ARRAY(a, u32, f->n * RASTER_MSAA + 2 * PAD);
    for (u32 i = 0; i < PAD; i++) {
        f->color_raw[i] = f->color_raw[PAD + f->n + i] = CANARY;
        f->ms_raw[i] = f->ms_raw[PAD + f->n * RASTER_MSAA + i] = CANARY;
    }
    f->fb.w = w;
    f->fb.h = h;
    f->fb.color = f->color_raw + PAD;
    f->fb.depth = ARENA_ARRAY(a, f32, f->n);
    f->fb.color_ms = f->ms_raw + PAD;
    f->fb.depth_ms = ARENA_ARRAY(a, f32, f->n * RASTER_MSAA);
    f->fb.samples = msaa ? RASTER_MSAA : 1u;
}

static b32 rfb_canaries_ok(const RFb* f) {
    for (u32 i = 0; i < PAD; i++) {
        if (f->color_raw[i] != CANARY || f->color_raw[PAD + f->n + i] != CANARY) return FALSE;
        if (f->ms_raw[i] != CANARY || f->ms_raw[PAD + f->n * RASTER_MSAA + i] != CANARY) return FALSE;
    }
    return TRUE;
}

typedef struct Counts {
    u32* c;
    i32  w, h;
    u32  outside;   /* framebuffer dışına düşen lane (olmamalı) */
} Counts;

static void shade_counts(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    Counts* c = (Counts*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        out[l] = 0xFF00FF00u;
        if (!(q->mask & (1u << l))) continue;
        i32 x = q->x + (i32)(l & 1), y = q->y + (i32)(l >> 1);
        if (x < 0 || y < 0 || x >= c->w || y >= c->h) c->outside++;
        else c->c[y * c->w + x]++;
    }
}

static void rvert(RasterVert* v, f32 x, f32 y) {
    for (u32 i = 0; i < SHADE_MAX_VARS; i++) v->var[i] = 0.0f;
    v->x = x;
    v->y = y;
    v->z = 0.5f;
    v->inv_w = 1.0f;
}

/* ---- 1) bellek ---- */

TEST(robust_arena_try_and_overflow) {
    Arena a;
    arena_init(&a, arena_alloc(test_arena(), 1024, 16), 1024);
    CHECK(arena_try(&a, 1000, 16) != NULL);
    CHECK(arena_try(&a, 100, 16) == NULL);
    CHECK(a.fails == 1);
    CHECK(arena_try_array(&a, (usize)1 << 20, (usize)1 << 20, 4) == NULL);   /* eleman × sayı 32 bitte taşar */
    CHECK(a.fails == 2);
    CHECK(arena_alloc_atomic(&a, 4096, 16) == NULL);
    CHECK(a.fails == 3);
    arena_reset(&a, 0);
    CHECK(arena_try(&a, 1024, 1) != NULL);   /* sıfırlandıktan sonra yine tamamı kullanılabilir */
}

/* küçücük arena: seri yol her dolduğunda flush eder → sıra ve kaplama aynı, hiçbir üçgen atlanmaz */
TEST(robust_tiny_raster_arena_flushes_preserve_coverage) {
    RFb f;
    rfb_init(&f, 96, 64, FALSE);
    Arena bins;
    const usize cap = 64u * 1024u + 32u * 1024u;   /* parça tablosu 16 KB + karo listesi + birkaç üçgen */
    arena_init(&bins, arena_alloc(test_arena(), cap, 16), cap);
    Raster r;
    raster_init(&r, &f.fb);
    raster_begin(&r, &bins);
    raster_set_depth(&r, FALSE, FALSE, DEPTH_ALWAYS);
    raster_set_cull(&r, CULL_NONE);
    Counts c = { ARENA_ARRAY(test_arena(), u32, f.n), 96, 64, 0 };
    raster_set_shader(&r, shade_counts, &c, 2);
    const u32 N = 200;
    for (u32 i = 0; i < N; i++) {   /* ekranı kaplayan iki üçgen (dikdörtgen) */
        RasterVert a, b, cc, d;
        rvert(&a, -10, -10); rvert(&b, 106, -10); rvert(&cc, 106, 74); rvert(&d, -10, 74);
        raster_triangle(&r, &a, &b, &cc);
        raster_triangle(&r, &a, &cc, &d);
    }
    raster_flush(&r);
    u32 bad = 0;
    for (usize i = 0; i < f.n; i++) bad += c.c[i] != N;
    test_note("kucuk arena: ara flush", r.stats.flushes, 0);
    CHECK(r.stats.flushes > 2);
    CHECK(bad == 0);
    CHECK(r.stats.tris_dropped == 0);
    CHECK(c.outside == 0);
    CHECK(rfb_canaries_ok(&f));
}

TEST(robust_raster_arena_too_small_drops_without_crash) {
    RFb f;
    rfb_init(&f, 64, 64, FALSE);
    Arena bins;
    arena_init(&bins, arena_alloc(test_arena(), 8192, 16), 8192);   /* parça tablosu bile sığmaz */
    Raster r;
    raster_init(&r, &f.fb);
    raster_begin(&r, &bins);
    raster_set_cull(&r, CULL_NONE);
    Counts c = { ARENA_ARRAY(test_arena(), u32, f.n), 64, 64, 0 };
    raster_set_shader(&r, shade_counts, &c, 2);
    RasterVert a, b, cc;
    rvert(&a, 0, 0); rvert(&b, 60, 0); rvert(&cc, 0, 60);
    for (u32 i = 0; i < 10; i++) raster_triangle(&r, &a, &b, &cc);
    raster_flush(&r);
    CHECK(r.stats.tris_dropped == 10);
    CHECK(bins.fails > 0);
}

static Mesh* grid_mesh(u32 seg) { return mesh_plane(test_arena(), 4.0f, 4.0f, 1.0f, seg); }

typedef struct PEnv {
    RFb f;
    Raster r;
    Pipeline p;
    Arena frame, bins;
    Counts c;
    Material mat;
} PEnv;

static void penv_init(PEnv* e, i32 w, i32 h, usize frame_cap, usize bins_cap, b32 msaa) {
    rfb_init(&e->f, w, h, msaa);
    arena_init(&e->frame, arena_alloc(test_arena(), frame_cap, 16), frame_cap);
    arena_init(&e->bins, arena_alloc(test_arena(), bins_cap, 16), bins_cap);
    raster_init(&e->r, &e->f.fb);
    raster_begin(&e->r, &e->bins);
    raster_set_depth(&e->r, FALSE, FALSE, DEPTH_ALWAYS);
    e->c.c = ARENA_ARRAY(test_arena(), u32, e->f.n);
    e->c.w = w;
    e->c.h = h;
    e->c.outside = 0;
    mem_zero(&e->mat, sizeof(e->mat));
    e->mat.shade = shade_counts;
    e->mat.uniforms = &e->c;
    e->mat.cull = CULL_NONE;
}

static void penv_frame(PEnv* e, const Mesh* m, u32 draws, Mat4 (*model)(u32)) {
    Mat4 view = mat4_look_at(v3(0, 6, 6), v3(0, 0, 0), v3(0, 1, 0));
    Mat4 proj = mat4_perspective(1.0f, (f32)e->f.fb.w / (f32)e->f.fb.h, 0.1f, 100.0f);
    pipe_begin(&e->p, &e->r, &e->frame, view, proj, v3(0, 6, 6));
    for (u32 i = 0; i < draws; i++) {
        DrawItem d = { m, &e->mat, model(i) };
        pipe_draw(&e->p, &d);
    }
    pipe_end(&e->p);
}

static Mat4 model_identity(u32 i) { UNUSED(i); return mat4_identity(); }

TEST(robust_pipeline_draw_limit_and_frame_memory) {
    PEnv e;
    /* çizim sınırı: PIPE_MAX_DRAWS + 25 görünür çizim → 25 atlanır */
    penv_init(&e, 64, 48, 16u << 20, 8u << 20, FALSE);
    Mesh* quad = grid_mesh(1);
    penv_frame(&e, quad, PIPE_MAX_DRAWS + 25, model_identity);
    test_note("cizim siniri: atlanan", e.p.stats.draws_dropped, 0);
    CHECK(e.p.stats.draws_dropped == 25);
    CHECK(rfb_canaries_ok(&e.f));
    /* kare belleği çok küçük: çizim kaydı tablosu sığmaz → hepsi atlanır, çökme yok */
    PEnv s;
    penv_init(&s, 64, 48, 4096, 8u << 20, FALSE);
    penv_frame(&s, quad, 10, model_identity);
    CHECK(s.p.stats.draws_dropped == 10);
    CHECK(s.frame.fails > 0);
    /* köşe belleği sığmayan büyük mesh atlanır, küçükler çizilir */
    PEnv v;
    usize recs = sizeof(DrawRec) * PIPE_MAX_DRAWS + 4096;
    penv_init(&v, 64, 48, recs + 64u * 1024u, 8u << 20, FALSE);
    Mesh* big = grid_mesh(200);   /* 40 401 köşe × ~70 bayt > 64 KB */
    penv_frame(&v, big, 1, model_identity);
    CHECK(v.p.stats.draws_dropped == 1);
    penv_frame(&v, quad, 3, model_identity);
    CHECK(v.p.stats.draws_dropped == 0);
    u32 lit = 0;
    for (usize i = 0; i < v.f.n; i++) lit += v.c.c[i] > 0;
    CHECK(lit > 100);
}

TEST(robust_pipeline_parallel_overflow_draws_what_fits) {
    /* tek çizim raster arenasının bütçesini aşar (paralel kurulumda flush yok) → sığanlar çizilir, gerisi sayılır */
    PEnv e;
    penv_init(&e, 320, 240, 16u << 20, 256u * 1024u, TRUE);
    Mesh* big = grid_mesh(120);   /* 28 800 üçgen */
    penv_frame(&e, big, 1, model_identity);
    test_note("tasma: atlanan ucgen", e.r.stats.tris_dropped, 0);
    test_note("tasma: flush'ta tasma", e.r.stats.overflows, 0);
    CHECK(e.r.stats.tris_dropped > 0);
    CHECK(e.r.stats.overflows > 0);
    CHECK(e.c.outside == 0);
    CHECK(rfb_canaries_ok(&e.f));
    u32 lit = 0;
    for (usize i = 0; i < e.f.n; i++) lit += e.c.c[i] > 0;
    CHECK(lit > 0);
}

/* ---- 2) rasterizer fuzz ---- */

/* kesin referans: 24.8 sabit nokta (floor(v·256 + 0.5)), piksel merkezi p·256 + 128, kenar a→b için
 * e = dx·(Py − ay) − dy·(Px − ax); içeride: hepsi > 0 ya da = 0 ve kenar top-left (dy < 0 ya da dy = 0, dx > 0) */
static i64 snap(f32 v) { return (i64)__builtin_floor((f64)v * 256.0 + 0.5); }

static b32 ref_cover(const i64* X, const i64* Y, i32 px, i32 py) {
    i64 Px = (i64)px * 256 + 128, Py = (i64)py * 256 + 128;
    for (u32 k = 0; k < 3; k++) {
        u32 j = (k + 1) % 3;
        i64 dx = X[j] - X[k], dy = Y[j] - Y[k];
        i64 e = dx * (Py - Y[k]) - dy * (Px - X[k]);
        b32 tl = dy < 0 || (dy == 0 && dx > 0);
        if (e < 0 || (e == 0 && !tl)) return FALSE;
    }
    return TRUE;
}

static f32 fuzz_coord(Rng* r, f32 lo, f32 hi) {
    u32 k = rng_u32(r) % 100;
    if (k == 0) return __builtin_nanf("");
    if (k == 1) return __builtin_inff();
    if (k == 2) return -3.0e6f;                                    /* sınır içi dev */
    if (k == 3) return 5.0e6f;                                     /* RASTER_MAX_COORD dışı */
    if (k < 8) return m_floorf(rng_range(r, lo, hi)) + 0.5f;     /* piksel merkezine tam denk (beraberlik) */
    return rng_range(r, lo, hi);
}

TEST(robust_raster_fuzz_coverage_matches_exact_reference) {
    const i32 W = 64, H = 48;
    RFb f;
    rfb_init(&f, W, H, FALSE);
    Arena bins;
    arena_init(&bins, arena_alloc(test_arena(), 2u << 20, 16), 2u << 20);
    Raster r;
    raster_init(&r, &f.fb);
    raster_set_cull(&r, CULL_NONE);
    raster_set_depth(&r, FALSE, FALSE, DEPTH_ALWAYS);
    Counts c = { ARENA_ARRAY(test_arena(), u32, f.n), W, H, 0 };
    Rng rng = rng_make(20261029);
    u32 tris = 0, mismatch = 0, drawn_px = 0, rejected = 0;
    for (u32 mode = 0; mode < RASTER_MODE_COUNT; mode++) {
        raster_set_mode(&r, (RasterMode)mode);
        for (u32 t = 0; t < 3000; t++) {
            RasterVert v[3];
            for (u32 k = 0; k < 3; k++) rvert(&v[k], fuzz_coord(&rng, -40.0f, (f32)W + 40.0f), fuzz_coord(&rng, -40.0f, (f32)H + 40.0f));
            if (t % 7 == 0) v[2] = v[1];   /* dejenere */
            mem_zero(c.c, sizeof(u32) * f.n);
            raster_begin(&r, &bins);
            raster_set_shader(&r, shade_counts, &c, 2);
            raster_triangle(&r, &v[0], &v[1], &v[2]);
            raster_flush(&r);
            tris++;
            b32 valid = TRUE;
            i64 X[3], Y[3];
            for (u32 k = 0; k < 3; k++) {
                if (!(m_absf(v[k].x) <= RASTER_MAX_COORD && m_absf(v[k].y) <= RASTER_MAX_COORD)) valid = FALSE;
                else { X[k] = snap(v[k].x); Y[k] = snap(v[k].y); }
            }
            if (valid) {   /* raster yönü normalleştirir: alan < 0 ise iki köşe yer değiştirir */
                i64 area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
                if (area == 0) valid = FALSE;
                else if (area < 0) { i64 tx = X[1]; X[1] = X[2]; X[2] = tx; i64 ty = Y[1]; Y[1] = Y[2]; Y[2] = ty; }
            } else {
                rejected++;
            }
            for (i32 py = 0; py < H; py++) {
                for (i32 px = 0; px < W; px++) {
                    u32 want = valid && ref_cover(X, Y, px, py) ? 1u : 0u;
                    u32 got = c.c[py * W + px];
                    if (got != want) mismatch++;
                    drawn_px += got;
                }
            }
        }
    }
    test_note("fuzz ucgen (skaler + SIMD)", tris, 0);
    test_note("fuzz: gecersiz koordinatli", rejected, 0);
    test_note("fuzz: boyanan piksel", drawn_px, 0);
    test_note("fuzz: referanstan farkli piksel", mismatch, 0);
    CHECK(mismatch == 0);
    CHECK(drawn_px > 100000);
    CHECK(r.stats.tris_dropped == 0);
    CHECK(bins.fails == 0);   /* raster_begin tekrarında sızıntı yok */
    CHECK(c.outside == 0);
    CHECK(rfb_canaries_ok(&f));
}

/* renk = varying'lerden (lane'e özgü), derinlik testi açık: SIMD ↔ skaler aynı framebuffer */
static void shade_vars(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    UNUSED(uniforms);
    for (u32 l = 0; l < 4; l++) {
        u32 r8 = (u32)m_clampf(q->var[0][l] * 255.0f, 0.0f, 255.0f);
        u32 g8 = (u32)m_clampf(q->var[1][l] * 255.0f, 0.0f, 255.0f);
        u32 b8 = (u32)m_clampf(q->z[l] * 255.0f, 0.0f, 255.0f);
        out[l] = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
    }
}

static u32 soup_hash(b32 msaa, RasterMode mode) {
    const i32 W = 96, H = 64;
    RFb f;
    rfb_init(&f, W, H, msaa);
    Arena bins;
    arena_init(&bins, arena_alloc(test_arena(), 1u << 20, 16), 1u << 20);   /* ara flush'lar da dahil */
    Raster r;
    raster_init(&r, &f.fb);
    raster_set_mode(&r, mode);
    raster_set_cull(&r, CULL_NONE);
    raster_set_depth(&r, TRUE, TRUE, DEPTH_LESS);
    fb_clear(&f.fb, 0xFF202020u, 1.0f);
    raster_begin(&r, &bins);
    raster_set_shader(&r, shade_vars, NULL, 2);
    Rng rng = rng_make(77);
    for (u32 t = 0; t < 2000; t++) {
        RasterVert v[3];
        for (u32 k = 0; k < 3; k++) {
            rvert(&v[k], fuzz_coord(&rng, -30.0f, (f32)W + 30.0f), fuzz_coord(&rng, -30.0f, (f32)H + 30.0f));
            v[k].z = rng_f32(&rng);
            v[k].inv_w = rng_range(&rng, 0.2f, 2.0f);
            v[k].var[0] = rng_f32(&rng);
            v[k].var[1] = rng_f32(&rng);
        }
        raster_triangle(&r, &v[0], &v[1], &v[2]);
    }
    raster_flush(&r);
    if (msaa) fb_resolve(&f.fb);
    u32 h = 2166136261u;
    for (usize i = 0; i < f.n; i++) h = (h ^ f.fb.color[i]) * 16777619u;
    if (!rfb_canaries_ok(&f)) h = 0;
    return h;
}

TEST(robust_raster_fuzz_simd_equals_scalar) {
    for (u32 msaa = 0; msaa < 2; msaa++) {
        u32 a = soup_hash(msaa, RASTER_MODE_SCALAR), b = soup_hash(msaa, RASTER_MODE_SIMD);
        test_note(msaa ? "corba MSAA hash" : "corba hash", a, 0);
        CHECK(a != 0 && a == b);
    }
}

/* boru hattı: rastgele (kameranın arkasında, w ≈ 0, dev, NaN) köşeli mesh'ler → çökme yok, tampon dışı yok */
static Mat4 model_spin(u32 i) { return mat4_mul(mat4_rotate_y((f32)i * 0.7f), mat4_rotate_x((f32)i * 0.3f)); }

TEST(robust_pipeline_fuzz_garbage_meshes) {
    PEnv e;
    penv_init(&e, 160, 120, 8u << 20, 8u << 20, TRUE);
    Rng rng = rng_make(4242);
    Mesh* m = mesh_alloc(test_arena(), 64, 64 * 3);
    u32 drawn = 0;
    for (u32 frame = 0; frame < 60; frame++) {
        for (u32 i = 0; i < m->vertex_count; i++) {
            u32 k = rng_u32(&rng) % 40;
            f32 s = k == 0 ? 1.0e7f : k < 4 ? 50.0f : 4.0f;
            m->px[i] = rng_range(&rng, -s, s);
            m->py[i] = rng_range(&rng, -s, s);
            m->pz[i] = k == 1 ? 5.999f : rng_range(&rng, -s, s);   /* kameraya çok yakın (w ≈ 0) */
            if (k == 2) m->px[i] = __builtin_nanf("");
            m->nx[i] = 0; m->ny[i] = 1; m->nz[i] = 0;
            m->u[i] = m->v[i] = 0.5f;
        }
        for (u32 i = 0; i < m->index_count; i++) m->indices[i] = rng_u32(&rng) % (m->vertex_count + 2);   /* aralık dışı da */
        mesh_compute_bounds(m);
        m->sphere_radius = 1.0e8f;   /* NaN'lı sınırlar elemeyi atlatsın: her şey boru hattına girsin */
        m->aabb_min = v3(-1.0e8f, -1.0e8f, -1.0e8f);
        m->aabb_max = v3(1.0e8f, 1.0e8f, 1.0e8f);
        m->sphere_center = v3(0, 0, 0);
        penv_frame(&e, m, 4, model_spin);
        drawn += e.r.stats.tris_drawn;
    }
    test_note("cop mesh: cizilen ucgen", drawn, 0);
    CHECK(drawn > 0);
    CHECK(e.c.outside == 0);
    CHECK(rfb_canaries_ok(&e.f));
}
