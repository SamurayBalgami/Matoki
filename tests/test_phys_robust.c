/* tests/test_phys_robust.c — fizik sağlamlığı (M12 Faz 3).
 * NaN/sonsuz: adımlar arasında yazılan bozuk hız ve adım içinde oluşan (NaN kuvvet) bozuk durum gövdeyi son geçerli
 * poza döndürür; ayrı adadaki yığın temiz koşuyla BİT-BİT aynı kalır. Bozuk BodyDesc temizlenir, aşırı hız sınırlanır.
 * Yük: 300 gövdelik yığın 10 s (sonlu, zeminden geçmez, girişim küçük, enerji yoktan var olmaz), 12 kutuluk kule. */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "phys/phys.h"

static const f32 DT = 1.0f / 60.0f;

static PhysWorld* rworld(u32 bodies) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), bodies, bodies * 8 + 64);
    return w;
}

static b32 all_finite(const PhysWorld* w) {
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        f32 s = b->pos.x + b->pos.y + b->pos.z + b->vel.x + b->vel.y + b->vel.z + b->ang_vel.x + b->ang_vel.y +
                b->ang_vel.z + b->rot.x + b->rot.y + b->rot.z + b->rot.w;
        if (!(m_absf(s) < 1e30f)) return FALSE;
    }
    return TRUE;
}

/* zemin + x = -5 ve x = +5'te 5'er kutu; dönüş: ikinci yığının en üstü */
static u32 two_stacks(PhysWorld* w) {
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &g);
    u32 top = 0;
    for (u32 s = 0; s < 2; s++) {
        for (u32 i = 0; i < 5; i++) {
            BodyDesc b = phys_desc_box(v3(s ? 5.0f : -5.0f, 0.5f + (f32)i * 1.0f, 0.0f), v3(0.5f, 0.5f, 0.5f), 1.0f);
            top = phys_add_body(w, &b);
        }
    }
    return top;
}

TEST(phys_robust_nan_isolated_bit_exact) {
    for (u32 kind = 0; kind < 2; kind++) {   /* 0: adımlar arası NaN hız, 1: adım içinde NaN kuvvet */
        PhysWorld* a = rworld(16);
        PhysWorld* b = rworld(16);
        two_stacks(a);
        u32 bad = two_stacks(b);
        for (u32 s = 0; s < 300; s++) {
            if (s == 30) {
                if (kind == 0) b->bodies[bad].vel = v3(__builtin_nanf(""), 0, 0);
                else b->bodies[bad].force = v3(0, __builtin_inff(), 0);
            }
            phys_step(a, DT);
            phys_step(b, DT);
        }
        CHECK(all_finite(b));
        CHECK(b->stats.nan_resets >= 1);
        u32 diff = 0;
        for (u32 i = 1; i <= 5; i++) {   /* ilk yığın (gövde 1..5): bozuk gövdeye hiç değmez */
            const Body* x = &a->bodies[i];
            const Body* y = &b->bodies[i];
            if (x->pos.x != y->pos.x || x->pos.y != y->pos.y || x->pos.z != y->pos.z || x->rot.w != y->rot.w) diff++;
        }
        CHECK(diff == 0);
        /* bozuk gövdenin yığını da toparlanır: kutular hâlâ üst üste, zeminin üstünde */
        CHECK(b->bodies[bad].pos.y > 3.0f && b->bodies[bad].pos.y < 5.0f);
    }
}

TEST(phys_robust_bad_desc_and_speed_limit) {
    PhysWorld* w = rworld(8);
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &g);
    BodyDesc s = phys_desc_sphere(v3(__builtin_nanf(""), 2.0f, 0.0f), 0.5f, 1.0f);
    s.vel = v3(0, __builtin_inff(), 0);
    u32 id = phys_add_body(w, &s);
    CHECK(w->stats.nan_resets == 1);
    CHECK(w->bodies[id].pos.x == 0.0f && w->bodies[id].vel.y == 0.0f);
    BodyDesc f = phys_desc_sphere(v3(3.0f, 5.0f, 0.0f), 0.3f, 1.0f);
    f.vel = v3(0.0f, -1.0e7f, 0.0f);   /* fiziksel olmayan hız: sınıra çekilir, CCD zeminde tutar */
    u32 fid = phys_add_body(w, &f);
    for (u32 i = 0; i < 120; i++) phys_step(w, DT);
    CHECK(all_finite(w));
    CHECK(w->stats.speed_clamps >= 1);
    CHECK(w->bodies[fid].pos.y > 0.0f);
}

static f64 total_energy(const PhysWorld* w) {
    f64 e = 0.0;
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        if (b->inv_mass <= 0.0f) continue;
        e += 9.81 * (f64)b->pos.y / b->inv_mass;
    }
    return e + phys_kinetic_energy(w);
}

typedef struct PileResult {
    f32 worst_pen, min_y, vmax;
    f64 ke, e_growth;
    u32 awake;
    b32 finite;
} PileResult;

/* n×n×layers karışık (kutu/küre) yığın, 6 m duvarlı çukurda, steps adım */
static PileResult pile_run(u32 n, u32 layers, u32 substeps, u32 steps) {
    PhysWorld* w = rworld(n * n * layers + 8);
    w->allow_sleep = TRUE;
    w->substeps = substeps;
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &g);
    BodyDesc walls[4] = { phys_desc_plane(v3(1, 0, 0), -6.0f), phys_desc_plane(v3(-1, 0, 0), -6.0f),
                          phys_desc_plane(v3(0, 0, 1), -6.0f), phys_desc_plane(v3(0, 0, -1), -6.0f) };
    for (u32 i = 0; i < 4; i++) phys_add_body(w, &walls[i]);
    Rng r = rng_make(300);
    f32 half = (f32)n * 0.5f - 0.5f;
    for (u32 l = 0; l < layers; l++) {
        for (u32 j = 0; j < n; j++) {
            for (u32 i = 0; i < n; i++) {
                Vec3 p = v3(-half + (f32)i + rng_range(&r, -0.1f, 0.1f), 1.0f + (f32)l * 1.6f + rng_range(&r, 0.0f, 0.3f),
                            -half + (f32)j + rng_range(&r, -0.1f, 0.1f));
                BodyDesc b = (i + j + l) % 2 ? phys_desc_sphere(p, 0.4f, 1.0f) : phys_desc_box(p, v3(0.4f, 0.4f, 0.4f), 1.0f);
                phys_add_body(w, &b);
            }
        }
    }
    PileResult res = { 0.0f, 1e9f, 0.0f, 0.0, 0.0, 0, FALSE };
    f64 e0 = total_energy(w), e_max = e0;
    for (u32 s = 0; s < steps; s++) {
        phys_step(w, DT);
        f64 e = total_energy(w);
        if (e > e_max) e_max = e;
        if (s > 120) res.worst_pen = m_maxf(res.worst_pen, phys_max_penetration(w));
    }
    for (u32 i = 5; i < w->count; i++) {
        res.min_y = m_minf(res.min_y, w->bodies[i].pos.y);
        res.vmax = m_maxf(res.vmax, v3_len(w->bodies[i].vel));
    }
    res.ke = phys_kinetic_energy(w);
    res.e_growth = (e_max - e0) / m_absf((f32)e0);
    res.awake = w->stats.awake;
    res.finite = all_finite(w) && w->stats.nan_resets == 0 && w->stats.speed_clamps == 0;
    return res;
}

static void pile_note(const char* tag, const PileResult* r) {
    UNUSED(tag);
    test_note("  en derin girisim (m, 2 s sonrasi)", r->worst_pen, 4);
    test_note("  en alcak merkez (m)", r->min_y, 3);
    test_note("  en hizli govde (m/s)", r->vmax, 4);
    test_note("  kinetik enerji (J)", r->ke, 5);
    test_note("  enerji artisi (goreli)", r->e_growth, 6);
    test_note("  uyanik govde", r->awake, 0);
}

TEST(phys_robust_pile_300_bodies) {
    /* 4 ve 8 alt adım: yığın tamamen durulup uyur. Önceden uyumuyordu (D36): küreler sonsuza dek yuvarlanıyor/yerinde
     * dönüyordu (yuvarlanma + burulma direnci yoktu), uyanan ada o adımda zeminsiz kalıp g·dt düşüyordu (çiftler
     * yeniden toplanmıyordu), değmeyen komşu adalar AABB örtüşmesiyle birbirini sonsuz döngüde uyandırıyordu. */
    PileResult a = pile_run(10, 3, 4, 600);
    test_note("yigin 300 (4 alt adim):", 0, 0);
    pile_note("4", &a);
    CHECK(a.finite);
    CHECK(a.min_y > 0.3f);          /* hiçbiri zeminden geçmedi (yarı boy 0,4) */
    CHECK(a.worst_pen < 0.05f);
    CHECK(a.ke < 1e-3);
    CHECK(a.e_growth <= 0.001);     /* enerji yoktan var olmaz */
    CHECK(a.awake == 0);
    PileResult b = pile_run(10, 3, 8, 600);
    test_note("yigin 300 (8 alt adim):", 0, 0);
    pile_note("8", &b);
    CHECK(b.finite && b.min_y > 0.3f);
    CHECK(b.worst_pen < 0.03f);
    CHECK(b.vmax < 0.02f);
    CHECK(b.e_growth <= 0.001);
    CHECK(b.awake == 0);
}

TEST(phys_robust_pile_50_sleeps) {
    PileResult r = pile_run(5, 2, 4, 600);
    test_note("yigin 50 (4 alt adim):", 0, 0);
    pile_note("50", &r);
    CHECK(r.finite);
    CHECK(r.awake == 0);   /* yuvarlanma + burulma direnci: küreler durur, ada uyur */
}
TEST(phys_robust_tall_tower_stands) {
    PhysWorld* w = rworld(16);
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &g);
    u32 top = 0;
    for (u32 i = 0; i < 12; i++) {
        BodyDesc b = phys_desc_box(v3(0.0f, 0.5f + (f32)i, 0.0f), v3(0.5f, 0.5f, 0.5f), 1.0f);
        top = phys_add_body(w, &b);
    }
    for (u32 s = 0; s < 600; s++) phys_step(w, DT);
    const Body* t = &w->bodies[top];
    f32 drift = m_sqrtf(t->pos.x * t->pos.x + t->pos.z * t->pos.z);
    test_note("kule 12: tepe yatay kayma (m)", drift, 5);
    test_note("kule 12: tepe yukseklik (m)", t->pos.y, 4);
    CHECK(drift < 0.05f);
    CHECK_NEAR(t->pos.y, 11.5, 0.05);
}

TEST(phys_robust_rolling_resistance_decel) {
    /* düzlemde kaymadan yuvarlanan küre: τ = μ_r·m·g·r → a = μ_r·g / (1 + I/(m r²)) = μ_r·g / 1,4 */
    for (u32 solver = 0; solver < PHYS_SOLVER_COUNT; solver++) {
        PhysWorld* w = rworld(4);
        phys_world_set_solver(w, (i32)solver);
        w->allow_sleep = FALSE;
        BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
        phys_add_body(w, &g);
        const f32 r = 0.5f, v0 = 3.0f, mu = 0.05f;
        BodyDesc s = phys_desc_sphere(v3(0, r, 0), r, 2.0f);
        s.rolling = mu;
        s.vel = v3(v0, 0, 0);
        s.ang_vel = v3(0, 0, -v0 / r);   /* +x'e yuvarlanma */
        u32 id = phys_add_body(w, &s);
        for (u32 i = 0; i < 60; i++) phys_step(w, DT);   /* 1 s */
        f64 want = v0 - mu * 9.81 / 1.4 * 1.0;
        test_note(solver ? "yuvarlanma (soft): v 1 s" : "yuvarlanma (SI): v 1 s", w->bodies[id].vel.x, 4);
        test_note("yuvarlanma: beklenen", want, 4);
        CHECK_NEAR(w->bodies[id].vel.x, want, 0.05 * (v0 - want));
    }
}