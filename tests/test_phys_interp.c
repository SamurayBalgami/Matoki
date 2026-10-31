/* tests/test_phys_interp.c — fizik ↔ görüntü ara değerlemesi (phys/interp, D39).
 * Restore bit bit geri koyar (fizik durumu değişmez); α = 0 / 1 uçları, dönüşte yarı yol; kımıldamayan, yeni eklenen
 * ve geçmişi silinen gövdeye dokunulmaz. Ölçüm: 60 Hz fizik, 144 / 75 Hz kare — çizilen konumun kare başı hız
 * değişiminin serbest düşmeden (−g·Δt) sapması (RMS); ara değerlemesiz sürüm "bir kare durur, bir kare atlar". */
#include "test.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/sys.h"
#include "phys/phys.h"
#include "phys/interp.h"

#define IP_DT (1.0f / 60.0f)

static PhysWorld* ip_world(void) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), 8, 32);
    w->allow_sleep = FALSE;
    return w;
}

TEST(phys_interp_restore_is_bit_exact) {
    PhysWorld* w = ip_world();
    BodyDesc d = phys_desc_box(v3(0, 5, 0), v3(0.5f, 0.3f, 0.2f), 1.0f);
    d.ang_vel = v3(1.3f, -2.1f, 0.7f);
    phys_add_body(w, &d);
    PhysInterp ip;
    phys_interp_init(&ip, test_arena(), 8);
    phys_interp_save(&ip, w);
    phys_step(w, IP_DT);
    Body before = w->bodies[0];
    phys_interp_apply(&ip, w, 0.37f);
    CHECK(!mem_equal(&before, &w->bodies[0], sizeof(Body)));   /* gerçekten ara poz yazıldı */
    phys_interp_restore(&ip, w);
    CHECK(mem_equal(&before, &w->bodies[0], sizeof(Body)));
    /* ara değerlemeli ve ara değerlemesiz koşu aynı fizik durumuna varır */
    PhysWorld* w2 = ip_world();
    phys_add_body(w2, &d);
    phys_step(w2, IP_DT);
    for (u32 n = 0; n < 50; n++) {
        phys_interp_save(&ip, w);
        phys_step(w, IP_DT);
        phys_step(w2, IP_DT);
        phys_interp_apply(&ip, w, (f32)(n % 7) / 7.0f);
        phys_interp_restore(&ip, w);
    }
    CHECK(mem_equal(&w->bodies[0], &w2->bodies[0], sizeof(Body)));
}

TEST(phys_interp_endpoints_and_halfway_rotation) {
    PhysWorld* w = ip_world();
    w->gravity = v3(0, 0, 0);
    BodyDesc d = phys_desc_box(v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
    d.vel = v3(6.0f, 0, 0);
    d.ang_vel = v3(0, 3.0f, 0);   /* adımda 0,05 rad */
    phys_add_body(w, &d);
    PhysInterp ip;
    phys_interp_init(&ip, test_arena(), 8);
    phys_interp_save(&ip, w);
    Vec3 p0 = w->bodies[0].pos;
    phys_step(w, IP_DT);
    Vec3 p1 = w->bodies[0].pos;
    Quat q1 = w->bodies[0].rot;
    phys_interp_apply(&ip, w, 0.0f);
    CHECK(w->bodies[0].pos.x == p0.x);
    CHECK_NEAR(w->bodies[0].rot.w, 1.0, 1e-6);
    phys_interp_restore(&ip, w);
    phys_interp_apply(&ip, w, 1.0f);
    CHECK_NEAR(w->bodies[0].pos.x, p1.x, 1e-6);
    CHECK_NEAR(w->bodies[0].rot.y, q1.y, 1e-6);
    phys_interp_restore(&ip, w);
    phys_interp_apply(&ip, w, 0.5f);
    CHECK_NEAR(w->bodies[0].pos.x, 0.5 * (p0.x + p1.x), 1e-6);
    /* y ekseni etrafında θ: q = (0, sin θ/2, 0, cos θ/2) → yarı yolda θ/2 */
    f64 th1 = 2.0 * m_atan2f(q1.y, q1.w);
    f64 th = 2.0 * m_atan2f(w->bodies[0].rot.y, w->bodies[0].rot.w);
    CHECK_NEAR(th, 0.5 * th1, 1e-4);
    /* rot_m dönüşle tutarlı */
    Mat3 m = mat3_from_quat(w->bodies[0].rot);
    CHECK(mem_equal(&m, &w->bodies[0].rot_m, sizeof(Mat3)));
    phys_interp_restore(&ip, w);
}

TEST(phys_interp_leaves_still_new_and_reset_bodies) {
    PhysWorld* w = ip_world();
    BodyDesc fixed = phys_desc_box(v3(0, -1, 0), v3(5, 1, 5), 0.0f);
    phys_add_body(w, &fixed);
    BodyDesc ball = phys_desc_sphere(v3(0, 4, 0), 0.3f, 1.0f);
    phys_add_body(w, &ball);
    PhysInterp ip;
    phys_interp_init(&ip, test_arena(), 8);
    phys_interp_save(&ip, w);
    phys_step(w, IP_DT);
    phys_add_body(w, &ball);   /* geçmişi yok */
    Body st = w->bodies[0], fresh = w->bodies[2];
    phys_interp_apply(&ip, w, 0.5f);
    CHECK(mem_equal(&st, &w->bodies[0], sizeof(Body)));
    CHECK(mem_equal(&fresh, &w->bodies[2], sizeof(Body)));
    CHECK(w->bodies[1].pos.y > 4.0f - 0.01f && w->bodies[1].pos.y < 4.0f);   /* ara poz */
    phys_interp_restore(&ip, w);
    phys_interp_reset(&ip);   /* ışınlama / sahne kurulumu */
    Body moving = w->bodies[1];
    phys_interp_apply(&ip, w, 0.5f);
    CHECK(mem_equal(&moving, &w->bodies[1], sizeof(Body)));
    phys_interp_restore(&ip, w);
}

TEST(phys_interp_points_offsets) {
    PhysInterpPts p;
    phys_interp_pts_init(&p, test_arena(), 5);
    Vec3 a[2] = { v3(0, 0, 0), v3(1, 1, 1) }, b[3] = { v3(2, 0, 0), v3(3, 0, 0), v3(4, 0, 0) };
    phys_interp_pts_save(&p, 0, a, 2);
    phys_interp_pts_save(&p, 2, b, 3);
    CHECK(p.count == 5);
    a[0] = v3(2, 0, 0);
    b[2] = v3(8, 0, 0);
    phys_interp_pts_apply(&p, 0, a, 2, 0.25f);
    phys_interp_pts_apply(&p, 2, b, 3, 0.25f);
    CHECK(a[0].x == 0.5f && a[1].x == 1.0f && b[2].x == 5.0f && b[0].x == 2.0f);
    phys_interp_pts_restore(&p, 0, a, 2);
    phys_interp_pts_restore(&p, 2, b, 3);
    CHECK(a[0].x == 2.0f && b[2].x == 8.0f);
    phys_interp_pts_reset(&p);
    phys_interp_pts_apply(&p, 0, a, 2, 0.25f);
    CHECK(a[0].x == 2.0f);
}

/* demo döngüsünün eşi: birikimli sabit adım, kare başına en çok 4 adım; çizilen y her kare kaydedilir.
 * Dönüş: kare başı hız değişiminin (Δy_i − Δy_{i−1}) / Δt serbest düşmedeki −g·Δt'den RMS sapması (m/s). */
static f64 ip_judder(f32 frame_hz, b32 interp) {
    PhysWorld* w = ip_world();
    BodyDesc s = phys_desc_sphere(v3(0, 100, 0), 0.2f, 1.0f);
    phys_add_body(w, &s);
    PhysInterp ip;
    phys_interp_init(&ip, test_arena(), 8);
    f32 fdt = 1.0f / frame_hz, acc = 0.0f;
    f64 y_prev = 0.0, d_prev = 0.0, sum = 0.0;
    u32 n = 0;
    for (u32 f = 0; f < 3u * (u32)frame_hz; f++) {
        acc += fdt;
        for (u32 k = 0; acc >= IP_DT && k < 4; k++) {
            phys_interp_save(&ip, w);
            phys_step(w, IP_DT);
            acc -= IP_DT;
        }
        if (interp) phys_interp_apply(&ip, w, m_clampf(acc / IP_DT, 0.0f, 1.0f));
        f64 y = w->bodies[0].pos.y;
        if (interp) phys_interp_restore(&ip, w);
        f64 d = y - y_prev;
        if (f >= 30) {
            f64 e = (d - d_prev) / fdt + 9.81 * fdt;
            sum += e * e;
            n++;
        }
        y_prev = y;
        d_prev = d;
    }
    return __builtin_sqrt(sum / (f64)n);
}

TEST(phys_interp_removes_judder) {
    f64 off144 = ip_judder(144.0f, FALSE), on144 = ip_judder(144.0f, TRUE);
    f64 off75 = ip_judder(75.0f, FALSE), on75 = ip_judder(75.0f, TRUE);
    test_note("144 Hz kare / 60 Hz fizik, sarsıntı RMS (m/s) ara değersiz", off144, 4);
    test_note("144 Hz kare / 60 Hz fizik, sarsıntı RMS (m/s) ara değerli", on144, 4);
    test_note("75 Hz kare / 60 Hz fizik, sarsıntı RMS (m/s) ara değersiz", off75, 4);
    test_note("75 Hz kare / 60 Hz fizik, sarsıntı RMS (m/s) ara değerli", on75, 4);
    CHECK(on144 < 0.02 * off144);
    CHECK(on75 < 0.02 * off75);
}

/* maliyet: 1024 gövde (demo kapasitesi), hepsi hareketli; adım başı save + kare başı apply + restore */
TEST(phys_interp_cost_1024_bodies) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), 1024, 64);
    w->allow_sleep = FALSE;
    w->gravity = v3(0, 0, 0);
    for (u32 i = 0; i < 1024; i++) {
        BodyDesc d = phys_desc_box(v3((f32)(i % 32) * 3.0f, (f32)(i / 32) * 3.0f, 0), v3(0.4f, 0.4f, 0.4f), 1.0f);
        d.ang_vel = v3(0.5f, 1.0f, 0.25f);
        phys_add_body(w, &d);
    }
    PhysInterp ip;
    phys_interp_init(&ip, test_arena(), 1024);
    phys_interp_save(&ip, w);
    phys_step(w, IP_DT);
    const u32 reps = 200;
    f64 t0 = sys_now_ms();
    for (u32 r = 0; r < reps; r++) phys_interp_save(&ip, w);
    f64 t1 = sys_now_ms();
    phys_interp_save(&ip, w);
    phys_step(w, IP_DT);
    for (u32 r = 0; r < reps; r++) {
        phys_interp_apply(&ip, w, 0.5f);
        phys_interp_restore(&ip, w);
    }
    f64 t2 = sys_now_ms();
    test_note("save, 1024 gövde (µs)", (t1 - t0) * 1000.0 / reps, 2);
    test_note("apply + restore, 1024 gövde (µs)", (t2 - t1) * 1000.0 / reps, 2);
    CHECK(ip.count == 1024 && ip.applied == 0);
}
