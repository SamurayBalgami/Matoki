/* tests/test_gfx3d_pipeline.c — 3D boru hattı testleri (M8 Faz 1).
 * Kırpma doğruluğu (konum/varying/alan), çatlaksızlık (ortak kenar + yakın düzlem),
 * perspektif-doğru UV, frustum culling, top-left kuralı ve yardımcı lane dışdeğerlemesi.
 * Raster gerçek framebuffer'a çizer; "yakalama" shader'ları sonuçları tamponlara yazar. */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "gfx3d/pipeline.h"

static const Vec4 NEAR_PLANE = { 0, 0, 1, 0 };

static f32 lin0(f32 x, f32 y, f32 z) { return 2.0f * x + 3.0f * y - z + 0.5f; }
static f32 lin1(f32 x, f32 y, f32 z) { return -x + 0.25f * z - 0.125f * y; }

static f64 tri_area3(f64 ax, f64 ay, f64 az, f64 bx, f64 by, f64 bz, f64 cx, f64 cy, f64 cz) {
    f64 ux = bx - ax, uy = by - ay, uz = bz - az;
    f64 vx = cx - ax, vy = cy - ay, vz = cz - az;
    f64 nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    return 0.5 * __builtin_sqrt(nx * nx + ny * ny + nz * nz);
}

static f64 poly_area3(const ClipVert* p, u32 n) {
    f64 a = 0.0;
    for (u32 i = 1; i + 1 < n; i++) {
        a += tri_area3(p[0].x, p[0].y, p[0].z, p[i].x, p[i].y, p[i].z, p[i + 1].x, p[i + 1].y, p[i + 1].z);
    }
    return a;
}

TEST(clip_near_random_10000) {
    Rng rng = rng_make(0xC11Bull);
    u32 bad_side = 0, bad_var = 0, bad_area = 0, bad_count = 0;
    for (u32 it = 0; it < 10000; it++) {
        ClipVert tri[3];
        u32 inside;
        do {
            inside = 0;
            for (u32 k = 0; k < 3; k++) {
                tri[k].x = rng_range(&rng, -2, 2);
                tri[k].y = rng_range(&rng, -2, 2);
                tri[k].z = rng_range(&rng, -1, 1);
                tri[k].w = 1.0f;
                tri[k].var[0] = lin0(tri[k].x, tri[k].y, tri[k].z);
                tri[k].var[1] = lin1(tri[k].x, tri[k].y, tri[k].z);
                if (tri[k].z >= 0.0f) inside++;
            }
        } while (inside == 0 || inside == 3);

        ClipVert out[CLIP_MAX_VERTS];
        u32 n = pipe_clip_polygon(tri, 3, out, NEAR_PLANE, 2);
        if (n != (inside == 1 ? 3u : 4u)) bad_count++;
        for (u32 k = 0; k < n; k++) {
            if (out[k].z < -1e-6f) bad_side++;
            f32 e0 = out[k].var[0] - lin0(out[k].x, out[k].y, out[k].z);
            f32 e1 = out[k].var[1] - lin1(out[k].x, out[k].y, out[k].z);
            if (m_absf(e0) > 1e-5f || m_absf(e1) > 1e-5f) bad_var++;
        }

        f64 A = tri_area3(tri[0].x, tri[0].y, tri[0].z, tri[1].x, tri[1].y, tri[1].z, tri[2].x, tri[2].y, tri[2].z);
        f64 expected;
        if (inside == 1) {
            u32 i = tri[0].z >= 0 ? 0 : (tri[1].z >= 0 ? 1 : 2);
            u32 j = (i + 1) % 3, k = (i + 2) % 3;
            f64 tj = (f64)tri[i].z / ((f64)tri[i].z - tri[j].z);
            f64 tk = (f64)tri[i].z / ((f64)tri[i].z - tri[k].z);
            expected = A * tj * tk;
        } else {
            u32 o = tri[0].z < 0 ? 0 : (tri[1].z < 0 ? 1 : 2);
            u32 j = (o + 1) % 3, k = (o + 2) % 3;
            f64 sj = (f64)tri[o].z / ((f64)tri[o].z - tri[j].z);
            f64 sk = (f64)tri[o].z / ((f64)tri[o].z - tri[k].z);
            expected = A * (1.0 - sj * sk);
        }
        f64 got = poly_area3(out, n);
        f64 diff = got - expected;
        if (diff < 0) diff = -diff;
        if (diff > 1e-4 * (A > 1.0 ? A : 1.0)) bad_area++;
    }
    CHECK(bad_count == 0);
    CHECK(bad_side == 0);
    CHECK(bad_var == 0);
    CHECK(bad_area == 0);
}

/* ---- yakalama shader'ları ---- */

typedef struct CountCapture {
    u32* counts;
    i32  w;
} CountCapture;

static void shade_count(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const CountCapture* c = (const CountCapture*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        out[l] = 0xFFFFFFFFu;
        if (q->mask & (1u << l)) c->counts[(q->y + (i32)(l >> 1)) * c->w + q->x + (i32)(l & 1)]++;
    }
}

typedef struct UvCapture {
    f32* u;
    f32* v;
    u8*  hit;
    i32  w;
} UvCapture;

static void shade_uv_capture(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const UvCapture* c = (const UvCapture*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        out[l] = 0;
        if (!(q->mask & (1u << l))) continue;
        i32 idx = (q->y + (i32)(l >> 1)) * c->w + q->x + (i32)(l & 1);
        c->u[idx] = q->var[VAR3D_UV][l];
        c->v[idx] = q->var[VAR3D_UV + 1][l];
        c->hit[idx] = 1;
    }
}

typedef struct Env {
    Framebuffer fb;
    Raster r;
    Pipeline p;
    Arena frame;
    Arena bins;
    Mat4 proj;
    f32 fov;
} Env;

static void env_init(Env* e, i32 w, i32 h) {
    Arena* a = test_arena();
    e->fb.w = w;
    e->fb.h = h;
    e->fb.color = ARENA_ARRAY(a, u32, w * h);
    e->fb.depth = ARENA_ARRAY(a, f32, w * h);
    arena_init(&e->frame, arena_alloc(a, 4 << 20, 16), 4 << 20);
    arena_init(&e->bins, arena_alloc(a, 8 << 20, 16), 8 << 20);
    raster_init(&e->r, &e->fb);
    raster_begin(&e->r, &e->bins);
    raster_set_depth(&e->r, FALSE, FALSE, DEPTH_ALWAYS);
    e->fov = 60.0f * (M_PI_F / 180.0f);
    e->proj = mat4_perspective(e->fov, (f32)w / (f32)h, 0.1f, 1000.0f);
    pipe_begin(&e->p, &e->r, &e->frame, mat4_identity(), e->proj, v3(0, 0, 0));
}

/* Piksel merkezinden geçen ışın (kamera orijinde, -Z'ye bakar), f64 */
static void pixel_ray(const Env* e, i32 px, i32 py, f64* dx, f64* dy, f64* dz) {
    f64 th = (f64)m_tanf(e->fov * 0.5f);
    f64 aspect = (f64)e->fb.w / (f64)e->fb.h;
    f64 nx = ((f64)px + 0.5) / e->fb.w * 2.0 - 1.0;
    f64 ny = 1.0 - ((f64)py + 0.5) / e->fb.h * 2.0;
    *dx = nx * th * aspect;
    *dy = ny * th;
    *dz = -1.0;
}

static Mesh* quad_mesh(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3) {
    Mesh* m = mesh_alloc(test_arena(), 4, 6);
    Vec3 ps[4] = { p0, p1, p2, p3 };
    for (u32 i = 0; i < 4; i++) {
        m->px[i] = ps[i].x; m->py[i] = ps[i].y; m->pz[i] = ps[i].z;
        m->nx[i] = 0; m->ny[i] = 1; m->nz[i] = 0;
        m->u[i] = ps[i].x; m->v[i] = ps[i].z;   /* uv = (x, z) */
    }
    u32 idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (u32 i = 0; i < 6; i++) m->indices[i] = idx[i];
    mesh_compute_bounds(m);
    return m;
}

TEST(clip_crack_free_shared_edge) {
    Env e;
    env_init(&e, 160, 120);
    u32* counts = ARENA_ARRAY(test_arena(), u32, e.fb.w * e.fb.h);
    CountCapture cap = { counts, e.fb.w };
    Material mat = { .shade = shade_count, .uniforms = &cap, .cull = CULL_NONE };
    u32 doubles = 0, holes = 0, checked = 0, near_clips = 0, guard_clips = 0;
    for (u32 trial = 0; trial < 24; trial++) {
        for (i32 i = 0; i < e.fb.w * e.fb.h; i++) counts[i] = 0;
        f32 ang = (f32)trial * 0.27f;
        f32 c = m_cosf(ang), s = m_sinf(ang);
        const f32 H = 100.0f;
        Vec3 corners[4];
        f32 lx[4] = { -H, H, H, -H }, lz[4] = { -H, -H, H, H };
        for (u32 k = 0; k < 4; k++) corners[k] = v3(c * lx[k] + s * lz[k], -1.0f, -s * lx[k] + c * lz[k]);
        Mesh* m = quad_mesh(corners[0], corners[1], corners[2], corners[3]);
        DrawItem d = { m, &mat, mat4_identity() };
        pipe_begin(&e.p, &e.r, &e.frame, mat4_identity(), e.proj, v3(0, 0, 0));
        pipe_draw(&e.p, &d);
        pipe_end(&e.p);
        near_clips += e.p.stats.tris_near_clipped;
        guard_clips += e.p.stats.tris_guard_clipped;

        for (i32 py = 0; py < e.fb.h; py++) {
            for (i32 px = 0; px < e.fb.w; px++) {
                u32 cnt = counts[py * e.fb.w + px];
                if (cnt > 1) doubles++;
                f64 dx, dy, dz;
                pixel_ray(&e, px, py, &dx, &dy, &dz);
                if (dy > -1e-3) continue;
                f64 t = -1.0 / dy;
                f64 hx = dx * t, hz = dz * t;
                f64 qx = c * hx - s * hz, qz = s * hx + c * hz;   /* yerel kareye geri döndür */
                if (qx < -0.9 * H || qx > 0.9 * H || qz < -0.9 * H || qz > 0.9 * H) continue;
                checked++;
                if (cnt != 1) holes++;
            }
        }
    }
    test_note("kontrol edilen piksel", checked, 0);
    CHECK(checked > 100000);
    CHECK(near_clips > 0);
    CHECK(doubles == 0);
    CHECK(holes == 0);
    UNUSED(guard_clips);
}

TEST(perspective_correct_uv) {
    Env e;
    env_init(&e, 200, 150);
    usize n = (usize)e.fb.w * e.fb.h;
    UvCapture cap = { ARENA_ARRAY(test_arena(), f32, n), ARENA_ARRAY(test_arena(), f32, n),
                      ARENA_ARRAY(test_arena(), u8, n), e.fb.w };
    Material mat = { .shade = shade_uv_capture, .uniforms = &cap, .cull = CULL_NONE };
    /* eğik düzlem y = -1, x ∈ [-3,3], z ∈ [-8,-1.5] */
    Mesh* m = quad_mesh(v3(-3, -1, -8), v3(3, -1, -8), v3(3, -1, -1.5f), v3(-3, -1, -1.5f));
    DrawItem d = { m, &mat, mat4_identity() };
    pipe_draw(&e.p, &d);
    pipe_end(&e.p);
    u32 hits = 0;
    f64 max_err = 0.0;
    for (i32 py = 0; py < e.fb.h; py++) {
        for (i32 px = 0; px < e.fb.w; px++) {
            usize i = (usize)py * e.fb.w + px;
            if (!cap.hit[i]) continue;
            hits++;
            f64 dx, dy, dz;
            pixel_ray(&e, px, py, &dx, &dy, &dz);
            f64 t = -1.0 / dy;
            f64 eu = dx * t, ev = dz * t;
            f64 du = cap.u[i] - eu, dv = cap.v[i] - ev;
            if (du < 0) du = -du;
            if (dv < 0) dv = -dv;
            if (du > max_err) max_err = du;
            if (dv > max_err) max_err = dv;
        }
    }
    test_note("piksel", hits, 0);
    test_note("en büyük UV hatası", max_err, 8);
    CHECK(hits > 5000);
    CHECK(max_err < 1e-4);
}

TEST(frustum_culling_known) {
    Mat4 vp = mat4_perspective(60.0f * (M_PI_F / 180.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    Vec4 pl[6];
    pipe_extract_frustum(&vp, pl);
    CHECK(pipe_sphere_visible(pl, v3(0, 0, -10), 1.0f));
    CHECK(!pipe_sphere_visible(pl, v3(0, 0, 10), 1.0f));          /* arkada */
    CHECK(!pipe_sphere_visible(pl, v3(100, 0, -10), 1.0f));       /* sağda dışarıda */
    CHECK(!pipe_sphere_visible(pl, v3(0, 0, -2000), 1.0f));       /* uzak düzlemin ötesinde */
    CHECK(!pipe_sphere_visible(pl, v3(0, 0, -0.05f), 0.01f));     /* yakın düzlemin önünde */
    CHECK(pipe_sphere_visible(pl, v3(-20, 0, -10), 15.0f));       /* sol düzlemi kesiyor */
    CHECK(pipe_sphere_visible(pl, v3(0, 0, 0), 1.0f));            /* kamerayı içeriyor */
    CHECK(pipe_aabb_visible(pl, v3(-1, -1, -11), v3(1, 1, -9)));
    CHECK(!pipe_aabb_visible(pl, v3(-1, -1, 5), v3(1, 1, 9)));
    CHECK(!pipe_aabb_visible(pl, v3(50, -1, -11), v3(52, 1, -9)));
    CHECK(pipe_aabb_visible(pl, v3(-500, -1, -11), v3(500, 1, -9)));

    /* dönüştürülmüş AABB: 45° dönen birim küp → yarı-boyut √2/2 */
    Vec3 mn, mx;
    Mat4 rot = mat4_rotate_y(M_PI_F * 0.25f);
    pipe_transform_aabb(&rot, v3(-0.5f, -0.5f, -0.5f), v3(0.5f, 0.5f, 0.5f), &mn, &mx);
    CHECK_NEAR(mx.x, 0.70710678, 1e-5);
    CHECK_NEAR(mn.z, -0.70710678, 1e-5);
    CHECK_NEAR(mx.y, 0.5, 1e-6);
}

static void rv(RasterVert* v, f32 x, f32 y) {
    for (u32 i = 0; i < SHADE_MAX_VARS; i++) v->var[i] = 0.0f;
    v->x = x;
    v->y = y;
    v->z = 0.5f;
    v->inv_w = 1.0f;
    v->var[0] = x;
    v->var[1] = y;
}

TEST(raster_top_left_shared_diagonal) {
    Env e;
    env_init(&e, 32, 32);
    u32* counts = ARENA_ARRAY(test_arena(), u32, 32 * 32);
    CountCapture cap = { counts, 32 };
    raster_set_shader(&e.r, shade_count, &cap, 2);
    raster_set_cull(&e.r, CULL_NONE);
    /* köşegen piksel merkezlerinden geçer → beraberlik durumları */
    RasterVert a, b, c, d;
    rv(&a, 4, 4); rv(&b, 20, 4); rv(&c, 20, 20); rv(&d, 4, 20);
    raster_triangle(&e.r, &a, &b, &c);
    raster_triangle(&e.r, &a, &c, &d);
    raster_flush(&e.r);
    u32 bad = 0;
    for (i32 y = 0; y < 32; y++) {
        for (i32 x = 0; x < 32; x++) {
            u32 expect = (x >= 4 && x < 20 && y >= 4 && y < 20) ? 1u : 0u;
            if (counts[y * 32 + x] != expect) bad++;
        }
    }
    CHECK(bad == 0);
    /* üçgen yelpazesi: tek merkez etrafında 8 üçgen; her piksel tam bir kez */
    for (i32 i = 0; i < 32 * 32; i++) counts[i] = 0;
    const f32 cx = 16.0f, cy = 16.0f, rr = 13.0f;
    f32 xs[9], ys[9];
    for (u32 k = 0; k <= 8; k++) {
        f32 ang = (f32)k * (M_TAU_F / 8.0f) + 0.1f;
        xs[k] = cx + m_cosf(ang) * rr;
        ys[k] = cy + m_sinf(ang) * rr;
    }
    xs[8] = xs[0];
    ys[8] = ys[0];
    for (u32 k = 0; k < 8; k++) {
        RasterVert p0, p1, p2;
        rv(&p0, cx, cy); rv(&p1, xs[k], ys[k]); rv(&p2, xs[k + 1], ys[k + 1]);
        raster_triangle(&e.r, &p0, &p1, &p2);
    }
    raster_flush(&e.r);
    u32 over = 0, inner_holes = 0;
    for (i32 y = 0; y < 32; y++) {
        for (i32 x = 0; x < 32; x++) {
            u32 cnt = counts[y * 32 + x];
            if (cnt > 1) over++;
            f32 dx = (f32)x + 0.5f - cx, dy = (f32)y + 0.5f - cy;
            if (dx * dx + dy * dy < 8.0f * 8.0f && cnt != 1) inner_holes++;
        }
    }
    CHECK(over == 0);
    CHECK(inner_holes == 0);
}

typedef struct LaneCapture {
    u32 partial_quads;
    u32 bad;
} LaneCapture;

static void shade_lane_check(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    LaneCapture* c = (LaneCapture*)uniforms;
    if (q->mask != 0xF) c->partial_quads++;
    for (u32 l = 0; l < 4; l++) {
        out[l] = 0;
        f32 ex = (f32)(q->x + (i32)(l & 1)) + 0.5f;
        f32 ey = (f32)(q->y + (i32)(l >> 1)) + 0.5f;
        if (m_absf(q->var[0][l] - ex) > 1e-3f || m_absf(q->var[1][l] - ey) > 1e-3f) c->bad++;
    }
    /* kaba türevler: ekran x/y için tam 1 olmalı */
    if (m_absf((q->var[0][1] - q->var[0][0]) - 1.0f) > 1e-3f) c->bad++;
    if (m_absf((q->var[1][2] - q->var[1][0]) - 1.0f) > 1e-3f) c->bad++;
}

TEST(raster_helper_lanes_extrapolated) {
    Env e;
    env_init(&e, 32, 32);
    LaneCapture cap = { 0, 0 };
    raster_set_shader(&e.r, shade_lane_check, &cap, 2);
    raster_set_cull(&e.r, CULL_NONE);
    RasterVert a, b, c;
    rv(&a, 10.2f, 10.2f); rv(&b, 13.7f, 10.6f); rv(&c, 10.4f, 14.1f);
    raster_triangle(&e.r, &a, &b, &c);
    rv(&a, 3.3f, 20.1f); rv(&b, 27.9f, 22.4f); rv(&c, 6.1f, 29.7f);
    raster_triangle(&e.r, &a, &b, &c);
    raster_flush(&e.r);
    CHECK(cap.partial_quads > 4);
    CHECK(cap.bad == 0);
}

/* ---- skaler ↔ SIMD bit-bit eşitliği ---- */

/* Tüm varying'lerin, derinliğin ve maskenin bitlerini renge katlar: herhangi bir bit farkı görünür. */
static void shade_bits(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    u32 nv = *(const u32*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        u32 h = 0x811c9dc5u ^ q->mask;
        h = (h ^ m_f32_bits(q->z[l])) * 0x01000193u;
        for (u32 i = 0; i < nv; i++) h = (h ^ m_f32_bits(q->var[i][l])) * 0x01000193u;
        out[l] = h;
    }
}

static void random_scene(Raster* r, Rng* rng, i32 w, i32 h, u32 count, u32 nv) {
    for (u32 i = 0; i < count; i++) {
        RasterVert v[3];
        /* küçük, orta ve ekran dışına taşan büyük üçgen karışımı */
        f32 size = (i % 10 == 0) ? 600.0f : ((i % 3 == 0) ? 90.0f : 14.0f);
        f32 cx = rng_range(rng, -20.0f, (f32)w + 20.0f), cy = rng_range(rng, -20.0f, (f32)h + 20.0f);
        for (u32 k = 0; k < 3; k++) {
            v[k].x = cx + rng_range(rng, -size, size);
            v[k].y = cy + rng_range(rng, -size, size);
            v[k].z = rng_range(rng, 0.0f, 1.0f);
            v[k].inv_w = rng_range(rng, 0.05f, 2.0f);
            for (u32 j = 0; j < SHADE_MAX_VARS; j++) v[k].var[j] = j < nv ? rng_range(rng, -50.0f, 50.0f) : 0.0f;
        }
        raster_triangle(r, &v[0], &v[1], &v[2]);
    }
}

TEST(raster_simd_matches_scalar) {
    /* tek boyutlar: framebuffer kenarındaki yarım quad'lar da sınansın */
    const i32 W = 197, H = 131;
    Env e;
    env_init(&e, W, H);
    usize n = (usize)W * H;
    u32* ref_c = ARENA_ARRAY(test_arena(), u32, n);
    f32* ref_d = ARENA_ARRAY(test_arena(), f32, n);
    static const DepthFunc funcs[3] = { DEPTH_LESS, DEPTH_GEQUAL, DEPTH_ALWAYS };
    u32 diff_c = 0, diff_d = 0, pixels = 0, full_bins = 0;
    for (u32 run = 0; run < 6; run++) {
        u32 nv = run * 3 % (SHADE_MAX_VARS + 1);
        for (u32 mode = 0; mode < 2; mode++) {
            Rng rng = rng_make(0xABCD0000ull + run);
            fb_clear(&e.fb, 0, run & 1 ? 0.0f : 1.0f);
            raster_reset_stats(&e.r);
            raster_set_mode(&e.r, mode ? RASTER_MODE_SIMD : RASTER_MODE_SCALAR);
            raster_set_cull(&e.r, CULL_NONE);
            raster_set_depth(&e.r, TRUE, run != 3, funcs[run % 3]);
            raster_set_shader(&e.r, shade_bits, &nv, nv);
            random_scene(&e.r, &rng, W, H, 1500, nv);
            raster_flush(&e.r);
            if (mode == 0) {
                for (usize i = 0; i < n; i++) { ref_c[i] = e.fb.color[i]; ref_d[i] = e.fb.depth[i]; }
                pixels += e.r.stats.pixels_written;
                full_bins += e.r.stats.bin_full;
            } else {
                for (usize i = 0; i < n; i++) {
                    if (ref_c[i] != e.fb.color[i]) diff_c++;
                    if (m_f32_bits(ref_d[i]) != m_f32_bits(e.fb.depth[i])) diff_d++;
                }
            }
        }
    }
    test_note("yazılan piksel (skaler)", pixels, 0);
    test_note("tam kaplanan karo", full_bins, 0);
    CHECK(pixels > 100000);
    CHECK(full_bins > 0);
    CHECK(diff_c == 0);
    CHECK(diff_d == 0);
}

TEST(raster_arena_overflow_flushes_midframe) {
    /* küçük arena: ara flush gerekir, sonuç büyük arenayla aynı olmalı */
    const i32 W = 128, H = 96;
    Env e;
    env_init(&e, W, H);
    usize n = (usize)W * H;
    u32* ref = ARENA_ARRAY(test_arena(), u32, n);
    u32 nv = 4;
    Arena small;
    arena_init(&small, arena_alloc(test_arena(), 200 * 1024, 16), 200 * 1024);
    u32 flushes[2] = { 0, 0 };
    for (u32 pass = 0; pass < 2; pass++) {
        Rng rng = rng_make(77);
        fb_clear(&e.fb, 0, 1.0f);
        raster_reset_stats(&e.r);
        raster_begin(&e.r, pass ? &small : &e.bins);
        raster_set_cull(&e.r, CULL_NONE);
        raster_set_depth(&e.r, TRUE, TRUE, DEPTH_LESS);
        raster_set_shader(&e.r, shade_bits, &nv, nv);
        random_scene(&e.r, &rng, W, H, 3000, nv);
        raster_flush(&e.r);
        flushes[pass] = e.r.stats.flushes;
        if (!pass) for (usize i = 0; i < n; i++) ref[i] = e.fb.color[i];
    }
    u32 diff = 0;
    for (usize i = 0; i < n; i++) diff += ref[i] != e.fb.color[i];
    CHECK(flushes[0] == 1);
    CHECK(flushes[1] > 1);
    CHECK(diff == 0);
}
