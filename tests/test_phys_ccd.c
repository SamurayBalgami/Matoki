/* tests/test_phys_ccd.c — M9 Faz 4: gerçek hull eylemsizliği, mesh iç kenar düzeltmesi, CCD.
 * Eylemsizlik: döndürülmüş kutu bulutu (analitik R·diag·Rᵀ) ve ızgara örneklemesiyle sayısal tensör.
 * İç kenar: kenar bayrakları (düz / çatı / vadi) ve düz üçgen ızgarada kayan kutu, yuvarlanan küre
 * — düzlemdeki aynı deneyle karşılaştırılır. CCD: ince duvara / mesh'e / düzleme yüksek hız, zeminde
 * hızlı kayma (takılmamalı), dinamik tahtaya mermi (BODY_BULLET). Açık/kapalı ölçümler not edilir. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "phys/phys.h"
#include "phys/shapes.h"

static const f32 DT = 1.0f / 60.0f;

static PhysWorld* world(u32 n) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

static void title(const char* a, const char* b) {
    char buf[160];
    StrBuf s;
    sb_init(&s, buf, sizeof(buf));
    sb_cstr(&s, "  ");
    sb_cstr(&s, a);
    sb_cstr(&s, b);
    log_sb(&s);
}

static Mat3 mat3_rdrt(const Mat3* r, Vec3 d) { return mat3_rotate_diag(r, d); }

static f32 mat3_max_rel_err(const Mat3* a, const Mat3* b) {
    f32 scale = 0.0f, err = 0.0f;
    for (u32 k = 0; k < 9; k++) {
        scale = m_maxf(scale, m_absf(b->m[k]));
        err = m_maxf(err, m_absf(a->m[k] - b->m[k]));
    }
    return err / scale;
}

/* ---- hull eylemsizliği ---- */

TEST(phys_hull_inertia_rotated_box) {
    Quat q = quat_from_axis_angle(v3(1, 2, 3), 0.7f);
    Mat3 R = mat3_from_quat(q);
    Vec3 hh = v3(1.0f, 0.5f, 0.25f);
    Vec3 pts[8];
    for (u32 i = 0; i < 8; i++) {
        Vec3 c = v3((i & 1) ? hh.x : -hh.x, (i & 2) ? hh.y : -hh.y, (i & 4) ? hh.z : -hh.z);
        pts[i] = v3_add(v3(3, -1, 2), mat3_mul_v3(&R, c));
    }
    ConvexHull* h = hull_create(test_arena(), pts, 8);
    CHECK(h != NULL);
    if (!h) return;
    const f32 m = 2.0f;
    PhysWorld* w = world(2);
    BodyDesc d = phys_desc_hull(v3(0, 0, 0), h, m);
    const Body* b = &w->bodies[phys_add_body(w, &d)];
    f32 x2 = hh.x * hh.x, y2 = hh.y * hh.y, z2 = hh.z * hh.z;
    Vec3 inv_exact = v3(3.0f / (m * (y2 + z2)), 3.0f / (m * (x2 + z2)), 3.0f / (m * (x2 + y2)));
    Mat3 want = mat3_rdrt(&R, inv_exact);
    f32 err = mat3_max_rel_err(&b->inv_inertia_world, &want);
    test_note("döndürülmüş kutu hull: I⁻¹ göreli hata", err, 6);
    CHECK(err < 1e-3f);
    /* eski yaklaşım (AABB kutusu, köşegen): aynı gövde için hata */
    Vec3 ah = v3_scale(v3_sub(h->aabb_max, h->aabb_min), 0.5f);
    f32 ax2 = ah.x * ah.x, ay2 = ah.y * ah.y, az2 = ah.z * ah.z;
    Mat3 old;
    mem_zero(&old, sizeof(old));
    old.m[0] = 3.0f / (m * (ay2 + az2));
    old.m[4] = 3.0f / (m * (ax2 + az2));
    old.m[8] = 3.0f / (m * (ax2 + ay2));
    test_note("eski AABB yaklaşımı: I⁻¹ göreli hata", mat3_max_rel_err(&old, &want), 3);
    /* gövde de döndürülünce: dünya tensörü R_b·R·diag·(R_b·R)ᵀ */
    Quat qb = quat_from_axis_angle(v3(-1, 0.5f, 0.2f), 1.9f);
    BodyDesc d2 = phys_desc_hull(v3(1, 1, 1), h, m);
    d2.rot = qb;
    const Body* b2 = &w->bodies[phys_add_body(w, &d2)];
    Mat3 RB = mat3_from_quat(quat_mul(qb, q));
    Mat3 want2 = mat3_rdrt(&RB, inv_exact);
    CHECK(mat3_max_rel_err(&b2->inv_inertia_world, &want2) < 1e-3f);
}

TEST(phys_hull_inertia_numeric) {
    Rng r = rng_make(77);
    Vec3 pts[24];
    for (u32 i = 0; i < 24; i++) {
        Vec3 p = v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), rng_range(&r, -1, 1));
        p = v3_norm(p);
        pts[i] = v3(p.x * 1.6f + 0.4f, p.y * 0.8f - 0.2f, p.z * 0.5f + p.x * 0.3f);
    }
    ConvexHull* h = hull_create(test_arena(), pts, 24);
    CHECK(h != NULL);
    if (!h) return;
    /* ızgara örneklemesi (hücre merkezleri), yerel (merkezlenmiş) uzayda */
    const u32 N = 72;
    Vec3 mn = h->aabb_min, ext = v3_sub(h->aabb_max, h->aabb_min);
    f64 vol = 0, cx = 0, cy = 0, cz = 0;
    f64 C[3][3] = { { 0 } };
    f64 cell = (f64)ext.x * ext.y * ext.z / ((f64)N * N * N);
    for (u32 k = 0; k < N; k++) {
        for (u32 j = 0; j < N; j++) {
            for (u32 i = 0; i < N; i++) {
                Vec3 p = v3(mn.x + ((f32)i + 0.5f) * ext.x / (f32)N, mn.y + ((f32)j + 0.5f) * ext.y / (f32)N,
                            mn.z + ((f32)k + 0.5f) * ext.z / (f32)N);
                b32 in = TRUE;
                for (u32 f = 0; f < h->fcount && in; f++) in = v3_dot(h->faces[f].n, p) <= h->faces[f].d;
                if (!in) continue;
                vol += cell;
                cx += cell * p.x; cy += cell * p.y; cz += cell * p.z;
                const f64 pv[3] = { p.x, p.y, p.z };
                for (u32 a = 0; a < 3; a++) for (u32 b = 0; b < 3; b++) C[a][b] += cell * pv[a] * pv[b];
            }
        }
    }
    test_note("hacim (hull)", h->volume, 4);
    test_note("hacim (örnekleme)", vol, 4);
    CHECK_NEAR(vol, h->volume, 0.01 * h->volume);
    CHECK(m_absf((f32)(cx / vol)) < 0.01f && m_absf((f32)(cy / vol)) < 0.01f && m_absf((f32)(cz / vol)) < 0.01f);
    f64 tr = C[0][0] + C[1][1] + C[2][2];
    Mat3 num;
    for (u32 c = 0; c < 3; c++) {
        for (u32 rr = 0; rr < 3; rr++) num.m[c * 3 + rr] = (f32)(((rr == c ? tr : 0.0) - C[rr][c]) / vol);
    }
    Mat3 ana = mat3_rdrt(&h->inertia_axes, h->inertia);
    f32 err = mat3_max_rel_err(&ana, &num);
    test_note("rastgele hull: I (birim kütle) örneklemeye göre göreli hata", err, 5);
    CHECK(err < 0.01f);
    /* asal eksenler ortonormal, det = +1 */
    const Mat3* Q = &h->inertia_axes;
    Vec3 c0 = v3(Q->m[0], Q->m[1], Q->m[2]), c1 = v3(Q->m[3], Q->m[4], Q->m[5]), c2 = v3(Q->m[6], Q->m[7], Q->m[8]);
    CHECK_NEAR(v3_dot(c0, c1), 0.0, 1e-5);
    CHECK_NEAR(v3_dot(c0, c2), 0.0, 1e-5);
    CHECK_NEAR(v3_dot(v3_cross(c0, c1), c2), 1.0, 1e-5);
}

/* ---- mesh iç kenarları ---- */

/* (n+1)² köşeli ızgara; y = height(x, z) */
static TriMesh* grid(u32 n, f32 cell, i32 kind) {
    Arena* a = test_arena();
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_ARRAY(a, u32, n * n * 6);
    f32 half = (f32)n * cell * 0.5f;
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i * cell - half, z = (f32)j * cell - half;
            f32 y = 0.0f;
            if (kind == 1) y = -0.5f * m_absf(x);   /* çatı: x = 0'da sırt (dışbükey) */
            if (kind == 2) y = 0.5f * m_absf(x);    /* vadi: x = 0'da oluk (içbükey) */
            v[j * (n + 1) + i] = v3(x, y, z);
        }
    }
    u32 k = 0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 a0 = j * (n + 1) + i, a1 = a0 + 1, b0 = a0 + n + 1, b1 = b0 + 1;
            idx[k++] = a0; idx[k++] = b0; idx[k++] = b1;
            idx[k++] = a0; idx[k++] = b1; idx[k++] = a1;
        }
    }
    return trimesh_create(a, v, nv, idx, n * n * 2);
}

/* x = ex hattında (z ekseni boyunca) uzanan kenarların aktifliği: 1 hepsi aktif, 0 hepsi pasif, 2 karışık */
static u32 edges_on_line(const TriMesh* m, f32 ex) {
    u32 act = 0, pas = 0;
    for (u32 t = 0; t < m->tri_count; t++) {
        Vec3 p[3];
        trimesh_tri(m, t, &p[0], &p[1], &p[2]);
        for (u32 k = 0; k < 3; k++) {
            Vec3 a = p[k], b = p[(k + 1) % 3];
            if (m_absf(a.x - ex) < 1e-4f && m_absf(b.x - ex) < 1e-4f) {
                if (m->edge_flags[t] & (1u << k)) act++;
                else pas++;
            }
        }
    }
    return act && pas ? 2 : (act ? 1 : 0);
}

TEST(phys_mesh_edge_flags) {
    TriMesh* flat = grid(4, 1.0f, 0);
    u32 boundary = 0, inner_active = 0;
    for (u32 t = 0; t < flat->tri_count; t++) {
        Vec3 p[3];
        trimesh_tri(flat, t, &p[0], &p[1], &p[2]);
        for (u32 k = 0; k < 3; k++) {
            Vec3 a = p[k], b = p[(k + 1) % 3];
            b32 on_border = (m_absf(a.x) == 2.0f && a.x == b.x) || (m_absf(a.z) == 2.0f && a.z == b.z);
            b32 act = (flat->edge_flags[t] >> k) & 1u;
            if (on_border) boundary += act;
            else inner_active += act;
        }
    }
    CHECK(boundary == 16);       /* sınır kenarları aktif (4 kenar × 4 hücre) */
    CHECK(inner_active == 0);    /* düz iç kenarlar pasif */
    CHECK(edges_on_line(grid(4, 1.0f, 1), 0.0f) == 1);   /* çatı sırtı: aktif */
    CHECK(edges_on_line(grid(4, 1.0f, 2), 0.0f) == 0);   /* vadi: pasif */
    CHECK(edges_on_line(grid(4, 1.0f, 1), 1.0f) == 0);   /* çatının düz eğimindeki iç kenar: pasif */
}

typedef struct SlideResult {
    f32 max_vy, max_w, dev;   /* en büyük |v_y|, |ω|, düzlem deneyine göre son konum farkı */
    Vec3 end;
} SlideResult;

/* ground: 0 düzlem, 1 mesh (düzeltme kapalı), 2 mesh (düzeltme açık); shape 0 kutu, 1 küre, 2 hull */
static SlideResult slide(u32 ground, u32 shape, const ConvexHull* rock) {
    PhysWorld* w = world(4);
    w->mesh_edge_fix = ground == 2;
    if (ground == 0) {
        BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
        g.friction = shape == 1 ? 0.6f : 0.0f;
        phys_add_body(w, &g);
    } else {
        BodyDesc g = phys_desc_mesh(v3(0, 0, 0), grid(24, 1.0f, 0));
        g.friction = shape == 1 ? 0.6f : 0.0f;
        phys_add_body(w, &g);
    }
    BodyDesc d;
    if (shape == 0) d = phys_desc_box(v3(-7, 0.5f, -2.3f), v3(0.5f, 0.5f, 0.5f), 1.0f);
    else if (shape == 1) d = phys_desc_sphere(v3(-7, 0.5f, -2.3f), 0.5f, 1.0f);
    else d = phys_desc_hull(v3(-7, -rock->aabb_min.y, -2.3f), rock, 1.0f);
    d.friction = shape == 1 ? 0.6f : 0.0f;
    d.vel = v3(6.0f, 0, 2.0f);
    if (shape == 1) d.ang_vel = v3(2.0f / 0.5f, 0, -6.0f / 0.5f);   /* yuvarlanma: ω = n × v / r */
    u32 id = phys_add_body(w, &d);
    SlideResult r = { 0, 0, 0, v3(0, 0, 0) };
    for (u32 s = 0; s < 120; s++) {
        phys_step(w, DT);
        const Body* b = &w->bodies[id];
        if (s >= 10) {   /* oturma */
            r.max_vy = m_maxf(r.max_vy, m_absf(b->vel.y));
            r.max_w = m_maxf(r.max_w, shape == 1 ? 0.0f : v3_len(b->ang_vel));
        }
    }
    r.end = w->bodies[id].pos;
    return r;
}

TEST(phys_mesh_internal_edges) {
    Vec3 rp[12];
    Rng rr = rng_make(9);
    for (u32 i = 0; i < 12; i++) rp[i] = v3(rng_range(&rr, -0.6f, 0.6f), rng_range(&rr, -0.4f, 0.4f), rng_range(&rr, -0.6f, 0.6f));
    rp[0] = v3(-0.6f, -0.4f, -0.6f); rp[1] = v3(0.6f, -0.4f, -0.6f); rp[2] = v3(0.6f, -0.4f, 0.6f); rp[3] = v3(-0.6f, -0.4f, 0.6f);
    ConvexHull* rock = hull_create(test_arena(), rp, 12);
    CHECK(rock != NULL);
    if (!rock) return;
    static const char* const NAMES[3] = { "kutu", "küre", "hull" };
    for (u32 shape = 0; shape < 3; shape++) {
        SlideResult ref = slide(0, shape, rock);
        SlideResult off = slide(1, shape, rock);
        SlideResult on = slide(2, shape, rock);
        f32 dev_off = v3_len(v3_sub(off.end, ref.end)), dev_on = v3_len(v3_sub(on.end, ref.end));
        title(NAMES[shape], " düz ızgarada kayma / yuvarlanma:");
        test_note("düzlem |vy|", ref.max_vy, 4);
        test_note("düzlem |w|", ref.max_w, 3);
        test_note("mesh, düzeltme kapalı |vy|", off.max_vy, 4);
        test_note("mesh, düzeltme kapalı |w|", off.max_w, 3);
        test_note("mesh, düzeltme kapalı son konum sapması (m)", dev_off, 4);
        test_note("mesh, düzeltme açık |vy|", on.max_vy, 4);
        test_note("mesh, düzeltme açık |w|", on.max_w, 3);
        test_note("mesh, düzeltme açık son konum sapması (m)", dev_on, 4);
        CHECK(on.max_vy < ref.max_vy + 0.05f);   /* küre düzlemde de ~0,08 (dönen çapa, Faz 1'den beri) */
        CHECK(on.max_w < ref.max_w + 0.1f);
        CHECK(dev_on < 0.05f);
    }
}

/* ---- CCD ---- */

/* x = 5'te ince duvara (kutu / mesh) yatay mermi; dönüş: ulaşılan en büyük x */
static f32 shoot_wall(b32 ccd, b32 mesh_wall, u32 shape, f32 speed) {
    PhysWorld* w = world(4);
    w->gravity = v3(0, 0, 0);
    w->continuous = ccd;
    if (mesh_wall) {
        static Vec3 v[4];
        static const u32 idx[6] = { 0, 1, 2, 0, 2, 3 };
        v[0] = v3(5, -2, -2); v[1] = v3(5, 2, -2); v[2] = v3(5, 2, 2); v[3] = v3(5, -2, 2);
        BodyDesc g = phys_desc_mesh(v3(0, 0, 0), trimesh_create(test_arena(), v, 4, idx, 2));
        phys_add_body(w, &g);
    } else {
        BodyDesc g = phys_desc_box(v3(5.05f, 0, 0), v3(0.05f, 2, 2), 0.0f);
        phys_add_body(w, &g);
    }
    BodyDesc d = shape == 0 ? phys_desc_sphere(v3(0, 0.3f, 0.2f), 0.1f, 1.0f)
                            : phys_desc_capsule(v3(0, 0.3f, 0.2f), 0.08f, 0.3f, 1.0f);
    d.vel = v3(speed, 0, 0);
    if (shape == 1) d.ang_vel = v3(0, 0, 40.0f);   /* dönen kapsül */
    u32 id = phys_add_body(w, &d);
    f32 maxx = -1e9f;
    for (u32 s = 0; s < 40; s++) {
        phys_step(w, DT);
        maxx = m_maxf(maxx, w->bodies[id].pos.x);
    }
    return maxx;
}

TEST(phys_ccd_thin_walls) {
    static const char* const WALL[2] = { "kutu duvar", "mesh duvar" };
    for (u32 mw = 0; mw < 2; mw++) {
        f32 off = shoot_wall(FALSE, mw, 0, 300.0f);
        f32 on = shoot_wall(TRUE, mw, 0, 300.0f);
        title(WALL[mw], " (x = 5):");
        test_note("küre 300 m/s, CCD kapalı en büyük x", off, 2);
        test_note("küre 300 m/s, CCD açık en büyük x", on, 3);
        CHECK(off > 5.5f);     /* CCD'siz geçiyor (sorunu doğrular) */
        CHECK(on < 4.92f);     /* yüzey: 5 − 0,1 (mesh) / 4,9 (kutu) */
        f32 off_c = shoot_wall(FALSE, mw, 1, 200.0f);
        f32 on_c = shoot_wall(TRUE, mw, 1, 200.0f);
        test_note("dönen kapsül 200 m/s, CCD kapalı en büyük x", off_c, 2);
        test_note("dönen kapsül 200 m/s, CCD açık en büyük x", on_c, 3);
        CHECK(on_c < 5.0f);
    }
}

TEST(phys_ccd_fast_fall_onto_plane) {
    f32 pen[2];
    for (u32 c = 0; c < 2; c++) {
        PhysWorld* w = world(4);
        w->continuous = c == 1;
        BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
        phys_add_body(w, &g);
        BodyDesc d = phys_desc_box(v3(0, 40, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
        d.vel = v3(0, -150.0f, 0);
        u32 id = phys_add_body(w, &d);
        f32 lowest = 1e9f;
        for (u32 s = 0; s < 60; s++) {
            phys_step(w, DT);
            lowest = m_minf(lowest, w->bodies[id].pos.y);
        }
        pen[c] = 0.5f - lowest;
    }
    test_note("kutu 150 m/s düzleme, en büyük girişim CCD kapalı (m)", pen[0], 3);
    test_note("kutu 150 m/s düzleme, en büyük girişim CCD açık (m)", pen[1], 4);
    CHECK(pen[1] < 0.03f);
}

/* zeminde hızlı kayan gövde CCD yüzünden takılmamalı (t = 0 → merkez küresi geri dönüşü) */
TEST(phys_ccd_fast_slide_not_stuck) {
    static const char* const GROUND[3] = { "düzlem", "mesh", "statik kutu karoları" };
    for (u32 ground = 0; ground < 3; ground++) {
        f32 dist[2];
        u32 swept = 0;
        for (u32 c = 0; c < 2; c++) {
            PhysWorld* w = world(64);
            w->continuous = c == 1;
            if (ground < 2) {
                BodyDesc g = ground ? phys_desc_mesh(v3(0, 0, 0), grid(48, 1.0f, 0)) : phys_desc_plane(v3(0, 1, 0), 0.0f);
                g.friction = 0.0f;
                phys_add_body(w, &g);
            } else {
                for (u32 k = 0; k < 48; k++) {
                    BodyDesc g = phys_desc_box(v3(-23.5f + (f32)k, -0.5f, 0), v3(0.5f, 0.5f, 2.0f), 0.0f);
                    g.friction = 0.0f;
                    phys_add_body(w, &g);
                }
            }
            BodyDesc d = phys_desc_box(v3(-20, 0.5f, 0.3f), v3(0.5f, 0.5f, 0.5f), 1.0f);
            d.friction = 0.0f;
            d.vel = v3(40.0f, 0, 0);
            u32 id = phys_add_body(w, &d);
            for (u32 s = 0; s < 30; s++) {
                phys_step(w, DT);
                swept += w->stats.ccd_bodies;
            }
            dist[c] = w->bodies[id].pos.x + 20.0f;
        }
        title("40 m/s kayma 0,5 s, zemin: ", GROUND[ground]);
        test_note("CCD kapalı yol (m)", dist[0], 3);
        test_note("CCD açık yol (m)", dist[1], 3);
        test_note("süpürülen gövde-adım", swept, 0);
        CHECK(swept > 0);
        CHECK_NEAR(dist[1], 20.0, 0.1);
    }
}

TEST(phys_ccd_bullet_vs_dynamic) {
    u32 hash[2] = { 0, 0 };
    for (u32 bullet = 0; bullet < 2; bullet++) {
        for (u32 rep = 0; rep < (bullet ? 2u : 1u); rep++) {
            PhysWorld* w = world(4);
            w->gravity = v3(0, 0, 0);
            BodyDesc plank = phys_desc_box(v3(5, 0, 0), v3(0.05f, 1, 1), 5.0f);
            u32 pid = phys_add_body(w, &plank);
            BodyDesc d = phys_desc_sphere(v3(0, 0.2f, 0), 0.05f, 0.1f);
            d.vel = v3(400.0f, 0, 0);
            d.restitution = 0.0f;
            u32 id = phys_add_body(w, &d);
            if (bullet) w->bodies[id].flags |= BODY_BULLET;
            for (u32 s = 0; s < 30; s++) phys_step(w, DT);
            const Body* p = &w->bodies[pid];
            const Body* b = &w->bodies[id];
            if (rep == 0) {
                title("mermi 400 m/s → dinamik tahta, ", bullet ? "BODY_BULLET:" : "normal:");
                test_note("tahta hızı (m/s)", p->vel.x, 2);
                test_note("mermi x", b->pos.x, 2);
                test_note("tahta x", p->pos.x, 2);
            }
            if (bullet) {
                CHECK(p->vel.x > 5.0f);          /* momentum aktarıldı (0,1·400 / 5,1 ≈ 7,8) */
                CHECK(b->pos.x < p->pos.x);      /* geçmedi */
                hash[rep] = phys_state_hash(w);
            }
        }
    }
    CHECK(hash[0] == hash[1]);
}
