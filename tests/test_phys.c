/* tests/test_phys.c — fizik motoru testleri (M9 Faz 1–2).
 * Analitik: serbest düşüş (yarı-örtük Euler kapalı formu), dönen küpte açısal momentum,
 * elastik çarpışmalar (eşit/eşit olmayan kütle), eğik düzlemde sürtünme (duran / kayan).
 * Dar faz: kutu-kutu SAT yüz/kenar/ayrık vakaları. Sistem: duran kutu, 5'li yığın, piramit
 * kararlılığı, belirleyicilik, uyku/uyanma. Sistem testleri iki çözücüyle de (SI, soft) koşar. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "phys/phys.h"

static i32 g_solver = PHYS_SOLVER_SI;
static const char* const SOLVER_NAME[] = { "SI", "soft" };

static PhysWorld* new_world(u32 bodies) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), bodies, bodies * 8 + 16);
    phys_world_set_solver(w, g_solver);
    return w;
}

static void note_solver(const char* label, f64 v, u32 dec) {
    char buf[128];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    sb_cstr(&sb, SOLVER_NAME[g_solver]);
    sb_cstr(&sb, ": ");
    sb_cstr(&sb, label);
    buf[sb.len] = 0;
    test_note(buf, v, dec);
}

#define FOR_SOLVERS for (g_solver = 0; g_solver < PHYS_SOLVER_COUNT; g_solver++)

static const f32 DT = 1.0f / 60.0f;

TEST(phys_free_fall_matches_semi_implicit_euler) {
    FOR_SOLVERS {
        PhysWorld* w = new_world(4);
        BodyDesc d = phys_desc_sphere(v3(0, 100, 0), 0.5f, 2.0f);
        d.vel = v3(1.0f, 3.0f, 0.0f);
        u32 id = phys_add_body(w, &d);
        const u32 N = 120;
        for (u32 i = 0; i < N; i++) phys_step(w, DT);
        /* soft: her adım 'substeps' adet h = dt/substeps alt adımı */
        f64 sub = g_solver == PHYS_SOLVER_SOFT ? (f64)w->substeps : 1.0;
        f64 g = 9.81, h = DT / sub, n = N * sub;
        f64 y = 100.0 + 3.0 * n * h - g * h * h * n * (n + 1) * 0.5;
        CHECK_NEAR(w->bodies[id].pos.y, y, 1e-3);
        CHECK_NEAR(w->bodies[id].pos.x, 1.0 * n * h, 1e-4);
        CHECK_NEAR(w->bodies[id].vel.y, 3.0 - g * n * h, 1e-4);
    }
}

TEST(phys_spinning_cube_conserves_angular_momentum) {
    PhysWorld* w = new_world(4);
    w->gravity = v3(0, 0, 0);
    BodyDesc d = phys_desc_box(v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 3.0f);
    d.ang_vel = v3(1.0f, 2.0f, -0.5f);
    u32 id = phys_add_body(w, &d);
    f64 e0 = phys_kinetic_energy(w);
    for (u32 i = 0; i < 600; i++) phys_step(w, DT);
    const Body* b = &w->bodies[id];
    CHECK_NEAR(b->ang_vel.x, 1.0, 1e-6);
    CHECK_NEAR(b->ang_vel.y, 2.0, 1e-6);
    CHECK_NEAR(phys_kinetic_energy(w), e0, 1e-4 * e0);
    f32 qn = b->rot.x * b->rot.x + b->rot.y * b->rot.y + b->rot.z * b->rot.z + b->rot.w * b->rot.w;
    CHECK_NEAR(qn, 1.0, 1e-5);
}

static void elastic_case(TestCtx* t_, f32 m1, f32 m2, f32 v1) {
    PhysWorld* w = new_world(4);
    w->gravity = v3(0, 0, 0);
    BodyDesc a = phys_desc_sphere(v3(-2, 0, 0), 0.5f, m1);
    BodyDesc b = phys_desc_sphere(v3(2, 0, 0), 0.5f, m2);
    a.vel = v3(v1, 0, 0);
    a.restitution = b.restitution = 1.0f;
    a.friction = b.friction = 0.0f;
    u32 ia = phys_add_body(w, &a), ib = phys_add_body(w, &b);
    for (u32 i = 0; i < 120; i++) phys_step(w, DT);
    f32 u1 = (m1 - m2) / (m1 + m2) * v1, u2 = 2.0f * m1 / (m1 + m2) * v1;
    CHECK_NEAR(w->bodies[ia].vel.x, u1, 0.05 * v1);
    CHECK_NEAR(w->bodies[ib].vel.x, u2, 0.05 * v1);
    CHECK_NEAR(m1 * w->bodies[ia].vel.x + m2 * w->bodies[ib].vel.x, m1 * v1, 1e-4 * m1 * v1);
}

TEST(phys_elastic_collisions) {
    FOR_SOLVERS {
        elastic_case(t_, 1.0f, 1.0f, 3.0f);
        elastic_case(t_, 1.0f, 3.0f, 3.0f);
        elastic_case(t_, 5.0f, 1.0f, 4.0f);
    }
}

TEST(phys_sat_box_box_cases) {
    Body A, B;
    Manifold m;
    BodyDesc da = phys_desc_box(v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    PhysWorld* w = new_world(8);
    u32 ia = phys_add_body(w, &da);
    A = w->bodies[ia];

    /* a) yüz-yüz, hizalı: 4 nokta, derinlik 0.1 */
    BodyDesc db = phys_desc_box(v3(0.1f, 0.9f, 0.05f), v3(0.5f, 0.5f, 0.5f), 1.0f);
    u32 ib = phys_add_body(w, &db);
    B = w->bodies[ib];
    CHECK(phys_collide(&A, &B, &m) == 4);
    CHECK_NEAR(m.normal.y, 1.0, 1e-5);
    for (u32 i = 0; i < m.count; i++) CHECK_NEAR(m.p[i].depth, 0.1, 1e-4);

    /* b) ayrık (boşluk > pay) */
    B.pos = v3(0, 1.1f, 0);
    phys_body_update_derived(&B);
    CHECK(phys_collide(&A, &B, &m) == 0);

    /* c) 45° dönmüş üst kutu: sekizgen kırpılır → 4 nokta, normal +y, derinlik 0.05 */
    BodyDesc dc = phys_desc_box(v3(0, 0.95f, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    dc.rot = quat_from_axis_angle(v3(0, 1, 0), M_PI_F * 0.25f);
    B = w->bodies[phys_add_body(w, &dc)];
    CHECK(phys_collide(&A, &B, &m) == 4);
    CHECK_NEAR(m.normal.y, 1.0, 1e-5);
    for (u32 i = 0; i < m.count; i++) CHECK_NEAR(m.p[i].depth, 0.05, 1e-4);

    /* d) kenar-kenar: A z etrafında 45°, B x etrafında 45°, üst üste; temas 1 nokta */
    BodyDesc ea = phys_desc_box(v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    ea.rot = quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.25f);
    BodyDesc eb = phys_desc_box(v3(0, 1.40f, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    eb.rot = quat_from_axis_angle(v3(1, 0, 0), M_PI_F * 0.25f);
    Body EA = w->bodies[phys_add_body(w, &ea)];
    Body EB = w->bodies[phys_add_body(w, &eb)];
    CHECK(phys_collide(&EA, &EB, &m) == 1);
    CHECK_NEAR(m.normal.y, 1.0, 1e-4);
    CHECK_NEAR(m.p[0].depth, 1.41421356 - 1.40, 1e-4);
    CHECK_NEAR(m.p[0].pos.x, 0.0, 1e-4);
    CHECK_NEAR(m.p[0].pos.z, 0.0, 1e-4);

    /* e) sıra değişince normal döner */
    Manifold m2;
    CHECK(phys_collide(&EB, &EA, &m2) == 1);
    CHECK_NEAR(m2.normal.y, -1.0, 1e-4);
}

TEST(phys_sphere_box_plane_cases) {
    PhysWorld* w = new_world(8);
    BodyDesc s = phys_desc_sphere(v3(0, 0.95f, 0), 0.5f, 1.0f);
    BodyDesc b = phys_desc_box(v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    BodyDesc p = phys_desc_plane(v3(0, 1, 0), 0.0f);
    Body S = w->bodies[phys_add_body(w, &s)];
    Body Bx = w->bodies[phys_add_body(w, &b)];
    Body P = w->bodies[phys_add_body(w, &p)];
    Manifold m;
    /* küre kutunun üstünde: kutu → küre normali +y; (S, B) sırasında A=S → normal -y */
    CHECK(phys_collide(&S, &Bx, &m) == 1);
    CHECK_NEAR(m.normal.y, -1.0, 1e-5);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-5);
    CHECK(phys_collide(&Bx, &S, &m) == 1);
    CHECK_NEAR(m.normal.y, 1.0, 1e-5);
    /* küre düzlemde */
    S.pos = v3(3, 0.45f, 0);
    CHECK(phys_collide(&S, &P, &m) == 1);
    CHECK_NEAR(m.normal.y, -1.0, 1e-5);
    CHECK_NEAR(m.p[0].depth, 0.05, 1e-5);
    /* kutu düzlemde: 4 köşe */
    Bx.pos = v3(0, 0.49f, 0);
    phys_body_update_derived(&Bx);
    CHECK(phys_collide(&Bx, &P, &m) == 4);
    for (u32 i = 0; i < 4; i++) CHECK_NEAR(m.p[i].depth, 0.01, 1e-5);
}

TEST(phys_box_rests_on_plane) {
    FOR_SOLVERS {
        PhysWorld* w = new_world(4);
        BodyDesc p = phys_desc_plane(v3(0, 1, 0), 0.0f);
        phys_add_body(w, &p);
        BodyDesc b = phys_desc_box(v3(0, 0.8f, 0), v3(0.5f, 0.5f, 0.5f), 2.0f);
        u32 id = phys_add_body(w, &b);
        for (u32 i = 0; i < 300; i++) phys_step(w, DT);
        const Body* B = &w->bodies[id];
        CHECK_NEAR(B->pos.y, 0.5, 0.01);
        CHECK(v3_len(B->vel) < 1e-2f);
        CHECK(v3_len(B->ang_vel) < 1e-2f);
        CHECK_NEAR(B->rot.w, 1.0, 1e-3);
        CHECK(w->stats.warm_hits > 0);
    }
}

static u32 build_stack(PhysWorld* w, u32 n) {
    BodyDesc p = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &p);
    u32 first = w->count;
    for (u32 i = 0; i < n; i++) {
        BodyDesc b = phys_desc_box(v3(0, 0.5f + (f32)i * 1.0f, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
        phys_add_body(w, &b);
    }
    return first;
}

TEST(phys_stack_of_five_is_stable) {
    FOR_SOLVERS {
        PhysWorld* w = new_world(16);
        u32 first = build_stack(w, 5);
        for (u32 i = 0; i < 600; i++) phys_step(w, DT);
        f32 max_drift = 0.0f;
        for (u32 i = 0; i < 5; i++) {
            const Body* b = &w->bodies[first + i];
            max_drift = m_maxf(max_drift, m_absf(b->pos.x));
            max_drift = m_maxf(max_drift, m_absf(b->pos.z));
            CHECK_NEAR(b->pos.y, 0.5 + i, 0.05);
        }
        note_solver("yığın: en büyük yatay kayma", max_drift, 6);
        note_solver("yığın: kinetik enerji", phys_kinetic_energy(w), 8);
        CHECK(max_drift < 0.01f);
        CHECK(phys_kinetic_energy(w) < 1e-3);
    }
}

static u32 build_pyramid(PhysWorld* w, u32 levels) {
    BodyDesc p = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &p);
    u32 first = w->count;
    for (u32 l = 0; l < levels; l++) {
        u32 n = levels - l;
        for (u32 i = 0; i < n; i++) {
            f32 x = ((f32)i - (f32)(n - 1) * 0.5f) * 1.05f;
            BodyDesc b = phys_desc_box(v3(x, 0.5f + (f32)l, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
            phys_add_body(w, &b);
        }
    }
    return first;
}

TEST(phys_pyramid_is_stable) {
    FOR_SOLVERS {
        PhysWorld* w = new_world(64);
        u32 first = build_pyramid(w, 5);
        u32 n = w->count - first;
        Vec3 start[32];
        for (u32 i = 0; i < n; i++) start[i] = w->bodies[first + i].pos;
        f64 ms = 0.0;
        for (u32 i = 0; i < 600; i++) {
            phys_step(w, DT);
            ms += w->stats.step_ms;
        }
        f32 max_d = 0.0f;
        for (u32 i = 0; i < n; i++) max_d = m_maxf(max_d, v3_len(v3_sub(w->bodies[first + i].pos, start[i])));
        note_solver("piramit (15 kutu): en büyük yer değiştirme", max_d, 5);
        note_solver("piramit: ortalama adım ms", ms / 600.0, 4);
        CHECK(max_d < 0.05f);
    }
}

/* Dönüş: kayma mesafesi; *v0 = ölçüm başındaki hız (oturma adımlarında kazanılan) */
static f32 incline_slide(f32 mu, f32 seconds, f32* v0) {
    PhysWorld* w = new_world(4);
    const f32 th = 20.0f * (M_PI_F / 180.0f);
    Vec3 n = v3(-m_sinf(th), m_cosf(th), 0.0f);
    BodyDesc p = phys_desc_plane(n, 0.0f);
    p.friction = mu;
    phys_add_body(w, &p);
    BodyDesc b = phys_desc_box(v3_scale(n, 0.5f), v3(0.5f, 0.5f, 0.5f), 1.0f);
    b.rot = quat_from_axis_angle(v3(0, 0, 1), th);
    b.friction = mu;
    b.restitution = 0.0f;
    u32 id = phys_add_body(w, &b);
    /* kısa oturma; sonra kayma ölç */
    for (u32 i = 0; i < 6; i++) phys_step(w, DT);
    Vec3 p0 = w->bodies[id].pos;
    *v0 = v3_len(w->bodies[id].vel);
    u32 steps = (u32)(seconds / DT + 0.5f);
    for (u32 i = 0; i < steps; i++) phys_step(w, DT);
    return v3_len(v3_sub(w->bodies[id].pos, p0));
}

TEST(phys_incline_friction) {
    FOR_SOLVERS {
        f32 v0;
        f32 still = incline_slide(0.5f, 2.0f, &v0);            /* tan 20° = 0.364 < 0.5 → durur */
        note_solver("μ=0.5 kayma (m)", still, 5);
        CHECK(still < 0.02f);
        const f32 th = 20.0f * (M_PI_F / 180.0f);
        f32 a = 9.81f * (m_sinf(th) - 0.2f * m_cosf(th));
        f32 got = incline_slide(0.2f, 2.0f, &v0);
        f32 want = v0 * 2.0f + 0.5f * a * 2.0f * 2.0f;
        note_solver("μ=0.2 kayma (m)", got, 4);
        note_solver("μ=0.2 analitik (m)", want, 4);
        CHECK_NEAR(got, want, 0.05 * want);
    }
}

static void spheres_into_pyramid(PhysWorld* w) {
    build_pyramid(w, 4);
    for (u32 k = 0; k < 6; k++) {
        BodyDesc s = phys_desc_sphere(v3(-8.0f + (f32)k * 0.3f, 1.0f + (f32)k * 0.5f, 0.2f * (f32)k), 0.4f, 2.0f);
        s.vel = v3(12.0f, 0.5f, -0.3f * (f32)k);
        phys_add_body(w, &s);
    }
}

TEST(phys_determinism) {
    FOR_SOLVERS {
        for (u32 sleep = 0; sleep < 2; sleep++) {
            u32 h[2];
            for (u32 run = 0; run < 2; run++) {
                PhysWorld* w = new_world(64);
                w->allow_sleep = sleep;
                spheres_into_pyramid(w);
                for (u32 i = 0; i < 300; i++) phys_step(w, DT);
                h[run] = phys_state_hash(w);
            }
            CHECK(h[0] == h[1]);
        }
    }
}

TEST(phys_sleep_and_wake) {
    FOR_SOLVERS {
        PhysWorld* w = new_world(64);
        w->allow_sleep = TRUE;
        u32 first = build_pyramid(w, 5);
        u32 n = w->count - first;
        u32 slept_at = 0;
        for (u32 i = 0; i < 300; i++) {
            phys_step(w, DT);
            if (!slept_at && w->stats.awake == 0) slept_at = i + 1;
        }
        note_solver("piramit uykuya daldığı adım", slept_at, 0);
        CHECK(slept_at > 0 && slept_at < 120);
        CHECK(w->stats.manifolds == 0);
        /* uyurken konum değişmez */
        Vec3 before[16];
        for (u32 i = 0; i < n; i++) before[i] = w->bodies[first + i].pos;
        for (u32 i = 0; i < 60; i++) phys_step(w, DT);
        f32 moved = 0.0f;
        for (u32 i = 0; i < n; i++) moved = m_maxf(moved, v3_len(v3_sub(w->bodies[first + i].pos, before[i])));
        CHECK(moved == 0.0f);
        /* hızlı küre çarpınca ada uyanır ve piramit dağılır */
        BodyDesc s = phys_desc_sphere(v3(-6.0f, 1.0f, 0.0f), 0.4f, 5.0f);
        s.vel = v3(20.0f, 0.0f, 0.0f);
        phys_add_body(w, &s);
        u32 max_awake = 0;
        for (u32 i = 0; i < 60; i++) {
            phys_step(w, DT);
            if (w->stats.awake > max_awake) max_awake = w->stats.awake;
        }
        note_solver("çarpmadan sonra en çok uyanık gövde", max_awake, 0);
        CHECK(max_awake >= n);
        moved = 0.0f;
        for (u32 i = 0; i < n; i++) moved = m_maxf(moved, v3_len(v3_sub(w->bodies[first + i].pos, before[i])));
        CHECK(moved > 0.5f);
    }
}
