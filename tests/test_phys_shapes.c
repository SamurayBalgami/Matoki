/* tests/test_phys_shapes.c — M9 Faz 3b: hull, GJK/EPA, kapsül, mesh + BVH, eklemler.
 * Çapraz doğrulama: aynı kutu çiftleri SAT (box_box) ve GJK/EPA + yüz kırpma (hull çekirdeği) ile.
 * Analitik: hull hacmi/merkezi, mesh raycast yüksekliği, sarkaç periyodu 2π√(L/g), ip uzunluğu,
 * motor hızı. Yapısal: Euler (V − E + F = 2), BVH sorgusu = kaba kuvvet. */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "phys/gjk.h"
#include "phys/phys.h"
#include "phys/shapes.h"

static const f32 DT = 1.0f / 60.0f;

static PhysWorld* world(u32 n) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

/* tek kullanımlık gövde (ortak bir dünyada kurulup kopyalanır; test arenası taşmasın) */
static PhysWorld* g_tmp_world;
static Arena* g_tmp_arena;

static Body make_body(const BodyDesc* d) {
    if (!g_tmp_world || g_tmp_arena != test_arena() || arena_mark(test_arena()) < sizeof(PhysWorld)) {
        g_tmp_world = world(2);
        g_tmp_arena = test_arena();
    }
    phys_world_clear(g_tmp_world);
    u32 id = phys_add_body(g_tmp_world, d);
    return g_tmp_world->bodies[id];
}

/* ---- hull ---- */

/* 0 = geçerli; 1 Euler, 2 CCW, 3 dışarıda nokta, 4 NULL */
static u32 hull_check(const ConvexHull* h, const Vec3* pts, u32 n) {
    if (!h) return 4;
    u32 E = h->icount / 2;
    if ((i32)h->vcount - (i32)E + (i32)h->fcount != 2) return 1;
    for (u32 f = 0; f < h->fcount; f++) {
        const HullFace* hf = &h->faces[f];
        Vec3 a = h->verts[h->idx[hf->first]], b = h->verts[h->idx[hf->first + 1]], c = h->verts[h->idx[hf->first + 2]];
        if (v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), hf->n) <= 0.0f) return 2;
        for (u32 i = 0; i < n; i++) {
            if (v3_dot(hf->n, v3_sub(pts[i], h->offset)) > hf->d + 1e-3f) return 3;
        }
    }
    return 0;
}

static b32 hull_valid(const ConvexHull* h, const Vec3* pts, u32 n) { return hull_check(h, pts, n) == 0; }

TEST(phys_hull_build) {
    Arena* a = test_arena();
    Vec3 cube[8];
    for (u32 i = 0; i < 8; i++) cube[i] = v3((i & 1) ? 2.0f : -2.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 0.5f : -0.5f);
    ConvexHull* h = hull_create(a, cube, 8);
    CHECK(h != NULL);
    CHECK(h->vcount == 8 && h->fcount == 6);
    CHECK_NEAR(h->volume, 8.0, 1e-4);
    CHECK(hull_valid(h, cube, 8));

    Vec3 tet[4] = { v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1) };
    ConvexHull* t = hull_create(a, tet, 4);
    CHECK(t && t->vcount == 4 && t->fcount == 4);
    CHECK_NEAR(t->volume, 1.0 / 6.0, 1e-5);
    CHECK_NEAR(t->offset.x, 0.25, 1e-5);
    CHECK(hull_valid(t, tet, 4));

    Rng r = rng_make(31);
    u32 ok = 0;
    for (u32 trial = 0; trial < 20; trial++) {
        Vec3 pts[40];
        for (u32 i = 0; i < 40; i++) {
            Vec3 p;
            do p = v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), rng_range(&r, -1, 1)); while (v3_dot(p, p) > 1.0f);
            pts[i] = v3_add(v3(3, -2, 1), v3_mul(p, v3(1.0f, 0.6f, 1.4f)));
        }
        ConvexHull* rh = hull_create(a, pts, 40);
        u32 code = hull_check(rh, pts, 40);
        if (code) {
            test_note("hull hata kodu", code, 0);
            if (rh) {
                test_note("  V", rh->vcount, 0);
                test_note("  F", rh->fcount, 0);
                test_note("  E", rh->icount / 2, 0);
            }
        }
        ok += code == 0;
    }
    CHECK(ok == 20);

    Vec3 flat[4] = { v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), v3(1, 0, 1) };
    CHECK(hull_create(a, flat, 4) == NULL);
}

/* ---- SAT ↔ GJK/EPA çapraz kontrolü ---- */

static f32 max_depth(const Manifold* m) {
    f32 d = -1.0f;
    for (u32 i = 0; i < m->count; i++) d = m_maxf(d, m->p[i].depth);
    return d;
}

/* İki kutunun gerçek en küçük ayırma mesafesi: 15 SAT ekseninde en küçük örtüşme (>0 = girişim) */
static f32 sat_mtd(const Body* A, const Body* B) {
    Vec3 ax[15];
    u32 n = 0;
    for (u32 i = 0; i < 3; i++) ax[n++] = mat3_col(&A->rot_m, i);
    for (u32 i = 0; i < 3; i++) ax[n++] = mat3_col(&B->rot_m, i);
    for (u32 i = 0; i < 3; i++) {
        for (u32 j = 0; j < 3; j++) {
            Vec3 c = v3_cross(mat3_col(&A->rot_m, i), mat3_col(&B->rot_m, j));
            if (v3_len(c) > 1e-4f) ax[n++] = v3_norm(c);
        }
    }
    f32 best = 3.4e38f;
    Vec3 d = v3_sub(B->pos, A->pos);
    for (u32 k = 0; k < n; k++) {
        f32 ra = 0.0f, rb = 0.0f;
        const f32 ha[3] = { A->half.x, A->half.y, A->half.z }, hb[3] = { B->half.x, B->half.y, B->half.z };
        for (u32 i = 0; i < 3; i++) {
            ra += ha[i] * m_absf(v3_dot(mat3_col(&A->rot_m, i), ax[k]));
            rb += hb[i] * m_absf(v3_dot(mat3_col(&B->rot_m, i), ax[k]));
        }
        best = m_minf(best, ra + rb - m_absf(v3_dot(d, ax[k])));
    }
    return best;
}

static BodyDesc random_box(Rng* r, Vec3 pos) {
    BodyDesc d = phys_desc_box(pos, v3(rng_range(r, 0.3f, 1), rng_range(r, 0.3f, 1), rng_range(r, 0.3f, 1)), 1.0f);
    d.rot = quat_from_axis_angle(v3(rng_range(r, -1, 1), rng_range(r, -1, 1), rng_range(r, -1, 1)), rng_range(r, -3, 3));
    return d;
}

TEST(phys_epa_depth_equals_sat_mtd) {
    /* derin girişimler dahil: EPA çekirdek derinliği = 15 eksenli SAT'ın gerçek MTD'si */
    Rng r = rng_make(4242);
    u32 n = 0, bad = 0;
    f32 worst = 0.0f;
    for (u32 it = 0; it < 3000; it++) {
        BodyDesc da = random_box(&r, v3(0, 0, 0));
        BodyDesc db = random_box(&r, v3(rng_range(&r, -1.2f, 1.2f), rng_range(&r, -1.2f, 1.2f), rng_range(&r, -1.2f, 1.2f)));
        Body A = make_body(&da), B = make_body(&db);
        f32 mtd = sat_mtd(&A, &B);
        if (mtd <= 0.001f) continue;
        Core ca, cb;
        phys_body_core(&A, &ca);
        phys_body_core(&B, &cb);
        GjkResult g;
        gjk_distance(&ca, &cb, &g);
        EpaResult e;
        if (!g.overlap) { bad++; continue; }
        epa_penetration(&ca, &cb, &g, &e);
        f32 diff = m_absf(e.depth - mtd);
        worst = m_maxf(worst, diff);
        if (!e.ok || diff > 2e-3f) bad++;
        n++;
    }
    test_note("girişimli çift", n, 0);
    test_note("EPA − MTD en büyük fark", worst, 6);
    CHECK(n > 1000);
    CHECK(bad == 0);
}

TEST(phys_gjk_epa_matches_sat_boxes) {
    /* gerçekçi sığ temaslar: B'yi rastgele yönden A'ya yaklaştır, MTD ∈ [−0,015, 0,06] */
    Rng r = rng_make(777);
    u32 both = 0, exist_mismatch = 0, normal_mismatch = 0, depth_bad = 0, bad_normal = 0, bad_sat = 0, cases = 3000;
    f32 worst = 0.0f;
    for (u32 it = 0; it < cases; it++) {
        BodyDesc da = random_box(&r, v3(0, 0, 0));
        BodyDesc db = random_box(&r, v3(0, 0, 0));
        Vec3 u = v3_norm(v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), rng_range(&r, -1, 1)));
        f32 target = rng_range(&r, -0.015f, 0.06f);
        f32 lo = 0.0f, hi = 4.0f;   /* ikili arama: MTD(t) = target */
        Body A = make_body(&da), B;
        for (u32 k = 0; k < 40; k++) {
            f32 mid = (lo + hi) * 0.5f;
            db.pos = v3_scale(u, mid);
            B = make_body(&db);
            if (sat_mtd(&A, &B) > target) lo = mid; else hi = mid;
        }
        db.pos = v3_scale(u, (lo + hi) * 0.5f);
        B = make_body(&db);
        Manifold ms, mg;
        u32 ns = phys_collide(&A, &B, &ms);
        Core ca, cb;
        phys_body_core(&A, &ca);
        phys_body_core(&B, &cb);
        u32 ng = phys_collide_cores(&ca, &cb, &mg);
        f32 ds = ns ? max_depth(&ms) : -1.0f, dg = ng ? max_depth(&mg) : -1.0f;
        /* pay sınırındaki (|derinlik| < 0.03) farkları sayma */
        if ((ns != 0) != (ng != 0)) {
            if (m_maxf(ds, dg) > 0.03f) exist_mismatch++;
            continue;
        }
        if (!ns) continue;
        both++;
        /* normaller farklı olabilir (SAT yüz histerezisi vs EPA'nın gerçek MTD yönü); ikisi de
         * en iyi eksenden belirgin biçimde kötü olmamalı: o(n) <= 1.1·MTD + 0.012 */
        if (v3_dot(ms.normal, mg.normal) < 0.99f) normal_mismatch++;
        f32 mtd = sat_mtd(&A, &B);
        const Vec3 ns_[2] = { ms.normal, mg.normal };
        for (u32 q = 0; q < 2; q++) {
            Vec3 nn = ns_[q];
            f32 ra = 0.0f, rb = 0.0f;
            const f32 ha[3] = { A.half.x, A.half.y, A.half.z }, hb[3] = { B.half.x, B.half.y, B.half.z };
            for (u32 i = 0; i < 3; i++) {
                ra += ha[i] * m_absf(v3_dot(mat3_col(&A.rot_m, i), nn));
                rb += hb[i] * m_absf(v3_dot(mat3_col(&B.rot_m, i), nn));
            }
            f32 o = ra + rb - m_absf(v3_dot(v3_sub(B.pos, A.pos), nn));
            if (o > 1.1f * mtd + 0.012f) {
                bad_normal++;
                if (q == 0) bad_sat++;
                if (bad_normal <= 3) {
                    test_note("  kötü: MTD", mtd, 5);
                    test_note("  kötü: o(n)", o, 5);
                    test_note("  kötü: nokta sayısı", mg.count, 0);
                    test_note("  kötü: GJK derinlik", dg, 5);
                    GjkResult gg;
                    gjk_distance(&ca, &cb, &gg);
                    test_note("  kötü: gjk tur", gg.iters, 0);
                    test_note("  kötü: gjk çıkış", gg.exit, 0);
                    test_note("  kötü: gjk simplex", gg.n, 0);
                    test_note("  kötü: gjk mesafe", gg.dist, 5);
                    Vec3 nn2 = v3_scale(v3_sub(gg.pb, gg.pa), 1.0f / gg.dist);
                    test_note("  kötü: destekle örtüşme", v3_dot(v3_sub(core_support(&ca, nn2), core_support(&cb, v3_scale(nn2, -1.0f))), nn2), 5);
                }
            }
        }
        f32 diff = m_absf(ds - dg);
        worst = m_maxf(worst, diff);
        if (diff > 0.05f * m_maxf(ds, dg) + 0.012f) depth_bad++;
    }
    test_note("temaslı çift", both, 0);
    test_note("var/yok uyumsuz", exist_mismatch, 0);
    test_note("farklı normal seçen (dot < 0.99, bilgi)", normal_mismatch, 0);
    test_note("en iyi eksenden belirgin kötü normal", bad_normal, 0);
    test_note("  bunlardan SAT", bad_sat, 0);
    test_note("derinlik toleransı dışı", depth_bad, 0);
    test_note("en büyük derinlik farkı", worst, 4);
    CHECK(both > 500);
    CHECK(exist_mismatch == 0);
    CHECK(bad_normal == 0);
    CHECK(depth_bad == 0);
}

/* ---- kapsül ---- */

TEST(phys_capsule_contacts) {
    Manifold m;
    BodyDesc pd = phys_desc_plane(v3(0, 1, 0), 0.0f);
    Body P = make_body(&pd);
    /* yatay kapsül düzlemde: 2 nokta */
    BodyDesc cd = phys_desc_capsule(v3(0, 0.45f, 0), 0.5f, 1.0f, 1.0f);
    cd.rot = quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.5f);
    Body C = make_body(&cd);
    CHECK(phys_collide(&C, &P, &m) == 2);
    CHECK_NEAR(m.normal.y, -1.0, 1e-5);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-4);
    CHECK_NEAR(m.p[1].depth, 0.05, 1e-4);

    /* dik kesişen kapsüller: 1 nokta, d = r1 + r2 − mesafe */
    BodyDesc c1 = phys_desc_capsule(v3(0, 0, 0), 0.3f, 1.0f, 1.0f);
    c1.rot = quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.5f);   /* x ekseni */
    BodyDesc c2 = phys_desc_capsule(v3(0, 0.5f, 0), 0.3f, 1.0f, 1.0f);
    c2.rot = quat_from_axis_angle(v3(1, 0, 0), M_PI_F * 0.5f);   /* z ekseni */
    Body A = make_body(&c1), B = make_body(&c2);
    CHECK(phys_collide(&A, &B, &m) == 1);
    CHECK_NEAR(m.normal.y, 1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.1, 1e-4);

    /* paralel kapsüller (kısmi örtüşme): 2 nokta */
    BodyDesc c3 = phys_desc_capsule(v3(0.8f, 0.5f, 0), 0.3f, 1.0f, 1.0f);
    c3.rot = c1.rot;
    Body Cc = make_body(&c3);
    CHECK(phys_collide(&A, &Cc, &m) == 2);
    CHECK_NEAR(m.normal.y, 1.0, 1e-4);
    for (u32 i = 0; i < 2; i++) CHECK_NEAR(m.p[i].depth, 0.1, 1e-4);

    /* kutu üstünde yatan kapsül: 2 nokta (doğru parçası yüze paralel) */
    BodyDesc bd = phys_desc_box(v3(0, -0.5f, 0), v3(2, 0.5f, 2), 1.0f);
    Body Bx = make_body(&bd);
    CHECK(phys_collide(&C, &Bx, &m) == 2);
    CHECK_NEAR(m.normal.y, -1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-3);

    /* küre-hull (genel yol) */
    Vec3 cube[8];
    for (u32 i = 0; i < 8; i++) cube[i] = v3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    ConvexHull* h = hull_create(test_arena(), cube, 8);
    BodyDesc hd = phys_desc_hull(v3(0, 0, 0), h, 1.0f);
    BodyDesc sd = phys_desc_sphere(v3(0.3f, 1.4f, -0.2f), 0.5f, 1.0f);
    Body H = make_body(&hd), S = make_body(&sd);
    CHECK(phys_collide(&S, &H, &m) == 1);
    CHECK_NEAR(m.normal.y, -1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 0.1, 1e-4);
}

/* ---- mesh + BVH ---- */

static TriMesh* slope_grid(u32 n, f32 cell, f32 sx, f32 sz) {
    Arena* a = test_arena();
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_ARRAY(a, u32, n * n * 6);
    f32 half = (f32)n * cell * 0.5f;
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i * cell - half, z = (f32)j * cell - half;
            v[j * (n + 1) + i] = v3(x, sx * x + sz * z, z);
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

TEST(phys_mesh_bvh_and_raycast) {
    TriMesh* m = slope_grid(32, 1.0f, 0.2f, 0.1f);
    Rng r = rng_make(5);
    u32 mism = 0;
    static u32 got[4096];
    for (u32 it = 0; it < 200; it++) {
        Vec3 c = v3(rng_range(&r, -18, 18), rng_range(&r, -6, 6), rng_range(&r, -18, 18));
        Vec3 e = v3(rng_range(&r, 0.1f, 3), rng_range(&r, 0.1f, 3), rng_range(&r, 0.1f, 3));
        Vec3 mn = v3_sub(c, e), mx = v3_add(c, e);
        u32 n = trimesh_query_aabb(m, mn, mx, got, 4096);
        u32 brute = 0;
        for (u32 t = 0; t < m->tri_count; t++) {
            Vec3 a, b, cc;
            trimesh_tri(m, t, &a, &b, &cc);
            Vec3 tmn = v3(m_minf(a.x, m_minf(b.x, cc.x)), m_minf(a.y, m_minf(b.y, cc.y)), m_minf(a.z, m_minf(b.z, cc.z)));
            Vec3 tmx = v3(m_maxf(a.x, m_maxf(b.x, cc.x)), m_maxf(a.y, m_maxf(b.y, cc.y)), m_maxf(a.z, m_maxf(b.z, cc.z)));
            if (tmn.x <= mx.x && tmx.x >= mn.x && tmn.y <= mx.y && tmx.y >= mn.y && tmn.z <= mx.z && tmx.z >= mn.z) brute++;
        }
        if (n != brute) mism++;
    }
    CHECK(mism == 0);
    test_note("BVH düğüm", m->node_count, 0);

    /* düzlem üzerinde raycast: y = 0.2x + 0.1z */
    PhysWorld* w = world(4);
    BodyDesc md = phys_desc_mesh(v3(0, 0, 0), m);
    phys_add_body(w, &md);
    f32 worst = 0.0f;
    for (u32 it = 0; it < 500; it++) {
        f32 x = rng_range(&r, -15, 15), z = rng_range(&r, -15, 15);
        RayHit h;
        if (!phys_raycast(w, v3(x, 20, z), v3(0, -1, 0), 100.0f, PHYS_NO_BODY, &h)) { worst = 1e9f; continue; }
        worst = m_maxf(worst, m_absf(h.point.y - (0.2f * x + 0.1f * z)));
    }
    test_note("mesh raycast en büyük yükseklik hatası", worst, 6);
    CHECK(worst < 1e-4f);

    /* kapsül ve hull raycast */
    BodyDesc cd = phys_desc_capsule(v3(30, 0, 0), 0.5f, 1.0f, 1.0f);
    phys_add_body(w, &cd);
    RayHit h;
    CHECK(phys_raycast(w, v3(30, 10, 0), v3(0, -1, 0), 100.0f, PHYS_NO_BODY, &h));
    CHECK_NEAR(h.t, 10.0 - 1.5, 1e-4);
    CHECK(phys_raycast(w, v3(25, 0.5f, 0), v3(1, 0, 0), 100.0f, PHYS_NO_BODY, &h));
    CHECK_NEAR(h.t, 4.5, 1e-4);
}

static TriMesh* flat_grid(u32 n, f32 cell) { return slope_grid(n, cell, 0.0f, 0.0f); }

TEST(phys_bodies_rest_on_mesh) {
    PhysWorld* w = world(16);
    BodyDesc md = phys_desc_mesh(v3(0, 0, 0), flat_grid(16, 1.0f));
    phys_add_body(w, &md);
    /* kutu üçgen köşegenlerinin üstünde, döndürülmüş */
    BodyDesc bd = phys_desc_box(v3(0.3f, 0.8f, 0.2f), v3(0.5f, 0.5f, 0.5f), 1.0f);
    bd.rot = quat_from_axis_angle(v3(0, 1, 0), 0.6f);
    u32 box = phys_add_body(w, &bd);
    BodyDesc sd = phys_desc_sphere(v3(-3, 1, 2), 0.4f, 1.0f);
    u32 sph = phys_add_body(w, &sd);
    BodyDesc cd = phys_desc_capsule(v3(3, 1, -2), 0.3f, 0.6f, 1.0f);
    cd.rot = quat_from_axis_angle(v3(1, 0, 0), M_PI_F * 0.5f);
    u32 cap = phys_add_body(w, &cd);
    Vec3 pts[12];
    for (u32 i = 0; i < 12; i++) {
        f32 a = (f32)i * (M_TAU_F / 6.0f);
        pts[i] = v3(m_cosf(a) * 0.5f, i < 6 ? -0.3f : 0.3f, m_sinf(a) * 0.5f);   /* altıgen prizma */
    }
    ConvexHull* hx = hull_create(test_arena(), pts, 12);
    BodyDesc hd = phys_desc_hull(v3(-2, 1, -3), hx, 1.0f);
    u32 hul = phys_add_body(w, &hd);
    for (u32 i = 0; i < 300; i++) phys_step(w, DT);
    const Body* B = &w->bodies[box];
    test_note("mesh üstünde kutu y", B->pos.y, 4);
    CHECK_NEAR(B->pos.y, 0.5, 0.01);
    CHECK(mat3_col(&B->rot_m, 1).y > 0.999f);
    CHECK(v3_len(B->vel) < 0.02f);
    CHECK_NEAR(w->bodies[sph].pos.y, 0.4, 0.01);
    CHECK_NEAR(w->bodies[cap].pos.y, 0.3, 0.01);
    CHECK(v3_len(w->bodies[cap].ang_vel) < 0.05f);
    CHECK_NEAR(w->bodies[hul].pos.y, 0.3, 0.01);
    CHECK(mat3_col(&w->bodies[hul].rot_m, 1).y > 0.999f);
}

/* ---- eklemler ---- */

static f32 pendulum_period(i32 solver, f32 L) {
    PhysWorld* w = world(4);
    phys_world_set_solver(w, solver);
    const f32 th = 0.1f;
    BodyDesc s = phys_desc_sphere(v3(m_sinf(th) * L, 10.0f - m_cosf(th) * L, 0), 0.02f, 1.0f);
    u32 bob = phys_add_body(w, &s);
    JointDesc j = phys_joint_desc(JOINT_BALL, PHYS_NO_BODY, bob, v3(0, 10, 0));
    phys_add_joint(w, &j);
    f32 prev = w->bodies[bob].pos.x;
    f32 t = 0.0f, first = -1.0f, last = 0.0f;
    u32 crossings = 0;
    for (u32 i = 0; i < 600; i++) {
        phys_step(w, DT);
        t += DT;
        f32 x = w->bodies[bob].pos.x;
        if ((prev > 0.0f) != (x > 0.0f)) {
            f32 tc = t - DT * x / (x - prev);   /* doğrusal ara değer */
            if (first < 0.0f) first = tc;
            last = tc;
            crossings++;
        }
        prev = x;
    }
    return crossings > 1 ? 2.0f * (last - first) / (f32)(crossings - 1) : 0.0f;
}

TEST(phys_joint_pendulum_period) {
    const f32 L = 2.0f;
    f32 want = M_TAU_F * m_sqrtf(L / 9.81f);
    f32 soft = pendulum_period(PHYS_SOLVER_SOFT, L), si = pendulum_period(PHYS_SOLVER_SI, L);
    test_note("sarkaç periyodu soft (s)", soft, 4);
    test_note("sarkaç periyodu SI (s)", si, 4);
    test_note("analitik 2π√(L/g)", want, 4);
    CHECK_NEAR(soft, want, 0.015 * want);
    CHECK_NEAR(si, want, 0.015 * want);
}

TEST(phys_joint_chain_and_rope) {
    PhysWorld* w = world(32);
    u32 prev = PHYS_NO_BODY;
    u32 first_joint = w->joint_count;
    for (u32 i = 0; i < 10; i++) {
        BodyDesc c = phys_desc_capsule(v3(0.5f + (f32)i, 10, 0), 0.1f, 0.4f, 1.0f);
        c.rot = quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.5f);   /* x boyunca */
        u32 id = phys_add_body(w, &c);
        JointDesc j = phys_joint_desc(JOINT_BALL, prev, id, v3((f32)i, 10, 0));
        phys_add_joint(w, &j);
        prev = id;
    }
    f32 worst = 0.0f;
    for (u32 s = 0; s < 240; s++) {
        phys_step(w, DT);
        for (u32 k = first_joint; k < w->joint_count; k++) worst = m_maxf(worst, phys_joint_error(w, k));
    }
    test_note("zincir: en büyük eklem hatası (m)", worst, 4);
    CHECK(worst < 0.05f);   /* yumuşak eklemler (60 Hz) en ağır anda birkaç cm esner */
    CHECK(phys_kinetic_energy(w) < 2000.0);

    /* ip: 1 m'den 3 m'ye kadar serbest düşer, sonra gergin kalır */
    PhysWorld* w2 = world(4);
    BodyDesc s = phys_desc_sphere(v3(0, 9, 0), 0.1f, 1.0f);
    u32 b = phys_add_body(w2, &s);
    JointDesc rope = phys_joint_desc(JOINT_DISTANCE, PHYS_NO_BODY, b, v3(0, 10, 0));
    rope.anchor_b = v3(0, 9, 0);
    rope.min_length = 0.0f;
    rope.max_length = 3.0f;
    phys_add_joint(w2, &rope);
    for (u32 i = 0; i < 20; i++) phys_step(w2, DT);
    f32 y_free = w2->bodies[b].pos.y;
    CHECK(y_free < 9.0f && y_free > 7.0f);   /* henüz gerilmedi, serbest düşüşte */
    for (u32 i = 0; i < 300; i++) phys_step(w2, DT);
    test_note("ip: son y", w2->bodies[b].pos.y, 4);
    CHECK_NEAR(w2->bodies[b].pos.y, 7.0, 0.03);

    /* çubuk: iki küre atılıp döner, mesafe sabit */
    PhysWorld* w3 = world(4);
    w3->gravity = v3(0, 0, 0);
    BodyDesc s1 = phys_desc_sphere(v3(0, 0, 0), 0.1f, 1.0f), s2 = phys_desc_sphere(v3(1.5f, 0, 0), 0.1f, 2.0f);
    s1.vel = v3(0, 3, 1);
    s2.vel = v3(1, -2, 0);
    u32 i1 = phys_add_body(w3, &s1), i2 = phys_add_body(w3, &s2);
    JointDesc rod = phys_joint_desc(JOINT_DISTANCE, i1, i2, v3(0, 0, 0));
    rod.anchor_b = v3(1.5f, 0, 0);
    u32 jr = phys_add_joint(w3, &rod);
    f32 rod_err = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys_step(w3, DT);
        rod_err = m_maxf(rod_err, phys_joint_error(w3, jr));
    }
    test_note("çubuk: en büyük uzunluk hatası (m)", rod_err, 5);
    CHECK(rod_err < 0.015f);
}

TEST(phys_joint_hinge_plane_limit_motor) {
    /* menteşe sarkacı: z ekseni etrafında; z yönünde sapma olmamalı */
    PhysWorld* w = world(8);
    BodyDesc b = phys_desc_box(v3(1.5f, 10, 0.3f), v3(0.5f, 0.1f, 0.3f), 1.0f);
    u32 bob = phys_add_body(w, &b);
    JointDesc h = phys_joint_desc(JOINT_HINGE, PHYS_NO_BODY, bob, v3(0, 10, 0.3f));
    h.axis = v3(0, 0, 1);
    u32 jh = phys_add_joint(w, &h);
    f32 zdev = 0.0f, wxy = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys_step(w, DT);
        zdev = m_maxf(zdev, m_absf(w->bodies[bob].pos.z - 0.3f));
        wxy = m_maxf(wxy, m_absf(w->bodies[bob].ang_vel.x) + m_absf(w->bodies[bob].ang_vel.y));
    }
    test_note("menteşe: z sapması", zdev, 6);
    CHECK(zdev < 2e-3f);
    CHECK(wxy < 1e-2f);
    CHECK(phys_joint_error(w, jh) < 0.01f);

    /* açı sınırı: [-0.5, 0.5] rad; yatay başlar, aşağı salınır ama sınırda durur */
    PhysWorld* w2 = world(8);
    BodyDesc b2 = phys_desc_box(v3(1.5f, 10, 0), v3(0.5f, 0.1f, 0.3f), 1.0f);
    u32 bob2 = phys_add_body(w2, &b2);
    JointDesc hl = phys_joint_desc(JOINT_HINGE, PHYS_NO_BODY, bob2, v3(0, 10, 0));
    hl.axis = v3(0, 0, 1);
    hl.limit = TRUE;
    hl.lower = -0.5f;
    hl.upper = 0.5f;
    u32 jl = phys_add_joint(w2, &hl);
    f32 amin = 0.0f, amax = 0.0f;
    for (u32 i = 0; i < 300; i++) {
        phys_step(w2, DT);
        amin = m_minf(amin, w2->joints[jl].angle);
        amax = m_maxf(amax, w2->joints[jl].angle);
    }
    test_note("menteşe sınırı: en küçük açı", amin, 4);
    CHECK(amin > -0.5f - 0.03f);
    CHECK(amax < 0.5f + 0.03f);
    CHECK(amin < -0.45f);   /* sınıra gerçekten ulaştı */

    /* motor: x ekseninde simetrik tekerlek, 5 rad/s */
    PhysWorld* w3 = world(8);
    BodyDesc wheel = phys_desc_box(v3(0, 5, 0), v3(0.1f, 0.5f, 0.5f), 2.0f);
    u32 wh = phys_add_body(w3, &wheel);
    JointDesc hm = phys_joint_desc(JOINT_HINGE, PHYS_NO_BODY, wh, v3(0, 5, 0));
    hm.axis = v3(1, 0, 0);
    hm.motor = TRUE;
    hm.motor_speed = 5.0f;
    hm.max_motor_torque = 50.0f;
    phys_add_joint(w3, &hm);
    for (u32 i = 0; i < 60; i++) phys_step(w3, DT);
    test_note("motor: açısal hız (rad/s)", w3->bodies[wh].ang_vel.x, 4);
    CHECK_NEAR(w3->bodies[wh].ang_vel.x, 5.0, 0.1);
    CHECK_NEAR(w3->bodies[wh].pos.y, 5.0, 0.01);
}

TEST(phys_joint_determinism) {
    u32 hsh[2];
    for (u32 run = 0; run < 2; run++) {
        PhysWorld* w = world(32);
        w->allow_sleep = TRUE;
        BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
        phys_add_body(w, &g);
        u32 prev = PHYS_NO_BODY;
        for (u32 i = 0; i < 6; i++) {
            BodyDesc c = phys_desc_box(v3(0.5f + (f32)i, 6, 0), v3(0.45f, 0.1f, 0.2f), 1.0f);
            u32 id = phys_add_body(w, &c);
            JointDesc j = phys_joint_desc(i ? JOINT_HINGE : JOINT_BALL, prev, id, v3((f32)i, 6, 0));
            j.axis = v3(0, 0, 1);
            phys_add_joint(w, &j);
            prev = id;
        }
        for (u32 k = 0; k < 4; k++) {
            BodyDesc s = phys_desc_sphere(v3(-4.0f + (f32)k, 1 + (f32)k, 0.1f * (f32)k), 0.3f, 2.0f);
            s.vel = v3(6, 3, 0);
            phys_add_body(w, &s);
        }
        for (u32 i = 0; i < 240; i++) phys_step(w, DT);
        hsh[run] = phys_state_hash(w);
    }
    CHECK(hsh[0] == hsh[1]);
}
