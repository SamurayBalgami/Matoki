/* tests/test_phys_fall.c — serbest düşme: motor (soft çözücü, yarı-örtük Euler alt adımları) kapalı formüllerle.
 * Sabit g: t = √(2d/g), v = √(2gd), d = ½gt², v = gt, ortalama hız ½gt = √(2gd)/2.
 * Gezegen çekimi g(h) = GM/(R+h)²: v = √(2GM(1/R − 1/(R+d))), süre = radyal Kepler düşüşü; kısa mesafe
 * yaklaşımı √(2GMd/R²) yalnızca d ≪ R'de geçerli. Enerji ½v² − GM/(R+h) uçuş boyunca korunur (simplektik). */
#include "test.h"
#include "core/math.h"
#include "phys/phys.h"

#define FALL_HZ 240.0f

typedef struct Fall {
    f64 t_hit, v_hit;      /* merkez y = 0'ı geçtiği an (adım içi doğrusal ara değer) */
    f64 d_at, v_at, t_at;  /* t_at anındaki düşülen yol ve hız (adım sınırında) */
    f64 e_drift;           /* gezegen enerjisinin en büyük göreli sapması */
} Fall;

/* gm = 0: sabit g (w->gravity). t_probe: ölçüm anı (adım sınırına yuvarlanır). */
static Fall fall_run(f64 gm, f64 radius, f64 d, f64 t_probe) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), 4, 16);
    w->allow_sleep = FALSE;
    w->planet_gm = gm;
    w->planet_radius = radius;
    BodyDesc s = phys_desc_sphere(v3(0, (f32)d, 0), 0.05f, 1.0f);
    u32 id = phys_add_body(w, &s);
    const f64 dt = 1.0 / FALL_HZ;
    Fall f = { 0 };
    f64 g0 = 9.81;
    f64 e0 = gm > 0.0 ? -gm / (radius + d) : g0 * d;
    f64 t = 0.0, y_prev = d, v_prev = 0.0;
    for (u32 n = 0; n < 200000; n++) {
        phys_step(w, (f32)dt);
        t += dt;
        const Body* b = &w->bodies[id];
        f64 y = b->pos.y, v = b->vel.y;
        if (f.t_at == 0.0 && t >= t_probe - 0.5 * dt) { f.t_at = t; f.d_at = d - y; f.v_at = -v; }
        f64 e = 0.5 * v * v + (gm > 0.0 ? -gm / (radius + y) : g0 * y);
        f64 rel = __builtin_fabs((e - e0) / e0);
        if (y > 0.0 && rel > f.e_drift) f.e_drift = rel;
        if (y <= 0.0) {
            f64 k = y_prev / (y_prev - y);
            f.t_hit = t - dt + k * dt;
            f.v_hit = -(v_prev + k * (v - v_prev));
            break;
        }
        y_prev = y;
        v_prev = v;
    }
    return f;
}

static f64 sq(f64 x) { return __builtin_sqrt(x); }

/* durgunluktan r0'dan r'ye radyal düşüş süresi: √(r0³/2GM)·(√(x(1−x)) + arccos √x), x = r/r0 */
static f64 kepler_fall_time(f64 gm, f64 r0, f64 r) {
    f64 x = r / r0;
    f64 acos_sqrt_x = m_atan2f((f32)sq(1.0 - x), (f32)sq(x));
    return sq(r0 * r0 * r0 / (2.0 * gm)) * (sq(x * (1.0 - x)) + acos_sqrt_x);
}

TEST(fall_constant_g_time_and_speed) {
    static const f64 D[] = { 2.0, 20.0, 80.0 };
    const f64 g = 9.81;
    for (u32 i = 0; i < 3; i++) {
        Fall f = fall_run(0.0, 0.0, D[i], 0.0);
        f64 t = sq(2.0 * D[i] / g), v = sq(2.0 * g * D[i]);
        test_note("sabit g: t motor", f.t_hit, 5);
        test_note("sabit g: t = sqrt(2d/g)", t, 5);
        test_note("sabit g: v motor", f.v_hit, 4);
        test_note("sabit g: v = sqrt(2gd)", v, 4);
        test_note("sabit g: enerji sapmasi", f.e_drift, 6);
        CHECK_NEAR(f.t_hit, t, 1e-3 * t);
        CHECK_NEAR(f.v_hit, v, 1e-3 * v);
        /* yarı-örtük Euler sabit g'de kapalı form: E_n − E_0 = −½·g·h·|v_n| → göreli en çok h·v/(2d), h → 0'da sıfıra
         * gider (birinci mertebe, enerji birikmez: sapma yalnızca o anki hıza bağlı) */
        f64 h = 1.0 / (FALL_HZ * 4.0);
        test_note("sabit g: kuramsal sapma h*v/(2d)", h * v / (2.0 * D[i]), 6);
        CHECK(f.e_drift <= 1.05 * h * v / (2.0 * D[i]));   /* son örnek çarpmadan önce + f32 konum: sınırın altında */
    }
}

TEST(fall_planet_gravity_variable_g) {
    /* küçük gezegen: R = 50 m, yüzeyde g = 9,81; d = R'den düşüş → tepede g yalnızca 9,81/4 */
    const f64 R = 50.0, gm = 9.81 * R * R, d = 50.0;
    Fall f = fall_run(gm, R, d, 0.0);
    f64 v = sq(2.0 * gm * (1.0 / R - 1.0 / (R + d)));
    f64 t = kepler_fall_time(gm, R + d, R);
    f64 v_const = sq(2.0 * 9.81 * d);
    test_note("gezegen: v motor", f.v_hit, 4);
    test_note("gezegen: v = sqrt(2GM(1/r-1/(r+d)))", v, 4);
    test_note("gezegen: sabit g ile v = sqrt(2gd)", v_const, 4);
    test_note("gezegen: t motor", f.t_hit, 5);
    test_note("gezegen: t Kepler", t, 5);
    test_note("gezegen: enerji sapmasi", f.e_drift, 8);
    CHECK_NEAR(f.v_hit, v, 1e-3 * v);
    CHECK_NEAR(f.t_hit, t, 2e-3 * t);
    CHECK(v_const > 1.3 * f.v_hit);   /* sabit g burada belirgin yanlış: motor gerçekten g(h) kullanıyor */
    CHECK(f.e_drift < 1e-3);
}

TEST(fall_earth_gravity_reduces_to_constant) {
    /* Dünya: 100 m'lik düşüşte g(h) değişimi ~3e-5 → iki formül de tutar */
    const f64 R = 6.371e6, gm = 3.986004418e14, d = 100.0;
    f64 g0 = gm / (R * R);
    Fall f = fall_run(gm, R, d, 0.0);
    f64 v = sq(2.0 * gm * (1.0 / R - 1.0 / (R + d)));
    test_note("Dunya: g0", g0, 5);
    test_note("Dunya: v motor", f.v_hit, 5);
    test_note("Dunya: v degisken", v, 5);
    test_note("Dunya: v sabit sqrt(2 g0 d)", sq(2.0 * g0 * d), 5);
    CHECK_NEAR(f.v_hit, v, 1e-3 * v);
    CHECK_NEAR(f.v_hit, sq(2.0 * g0 * d), 1e-3 * v);
    CHECK_NEAR(f.t_hit, sq(2.0 * d / g0), 1e-3 * f.t_hit);
}

/* Fotoğraftaki 8 denklem, sabit g = 9,81 ve gezegen çekimiyle motorun ölçümüne karşı */
TEST(fall_textbook_equations) {
    const f64 g = 9.81, d = 30.0, tp = 1.5;
    Fall f = fall_run(0.0, 0.0, d, tp);
    f64 t = f.t_at;
    /* 1) d = ½gt²   3) v = gt   5) ortalama hız (yol/süre) = ½gt */
    CHECK_NEAR(f.d_at, 0.5 * g * t * t, 2e-3 * f.d_at);
    CHECK_NEAR(f.v_at, g * t, 1e-3 * f.v_at);
    CHECK_NEAR(f.d_at / t, 0.5 * g * t, 2e-3 * f.d_at / t);
    /* 2) t = √(2d/g)   4) v = √(2gd)   6) d yolundaki ortalama hız = √(2gd)/2 */
    CHECK_NEAR(f.t_hit, sq(2.0 * d / g), 1e-3 * f.t_hit);
    CHECK_NEAR(f.v_hit, sq(2.0 * g * d), 1e-3 * f.v_hit);
    CHECK_NEAR(d / f.t_hit, sq(2.0 * g * d) / 2.0, 1e-3 * d / f.t_hit);
    test_note("1) d=gt^2/2: motor", f.d_at, 4);
    test_note("1) formul", 0.5 * g * t * t, 4);
    test_note("3) v=gt: motor", f.v_at, 4);
    test_note("5) ort. hiz: motor", f.d_at / t, 4);
    test_note("6) ort. hiz sqrt(2gd)/2", sq(2.0 * g * d) / 2.0, 4);
    /* 7) v = √(2GMd/r²): kısa mesafe yaklaşımı. d ≪ r'de doğru, d ~ r'de belirgin fazla */
    {
        const f64 R = 1000.0, gm = 9.81 * R * R, dd = 5.0;
        Fall s = fall_run(gm, R, dd, 0.0);
        f64 approx = sq(2.0 * gm * dd / (R * R));
        test_note("7) kisa (d=5, r=1000): motor", s.v_hit, 4);
        test_note("7) sqrt(2GMd/r^2)", approx, 4);
        CHECK_NEAR(s.v_hit, approx, 5e-3 * approx);
    }
    {
        const f64 R = 50.0, gm = 9.81 * R * R, dd = 50.0;
        Fall s = fall_run(gm, R, dd, 0.0);
        f64 approx = sq(2.0 * gm * dd / (R * R));
        f64 exact = sq(2.0 * gm * (1.0 / R - 1.0 / (R + dd)));   /* 8) */
        test_note("7) uzun (d=r=50): yaklasim", approx, 4);
        test_note("8) uzun: kesin sqrt(2GM(1/r-1/(r+d)))", exact, 4);
        test_note("8) uzun: motor", s.v_hit, 4);
        CHECK(approx > 1.3 * s.v_hit);               /* 7. denklem uzun mesafede geçersiz */
        CHECK_NEAR(s.v_hit, exact, 1e-3 * exact);    /* 8. denklem kesin */
    }
}
