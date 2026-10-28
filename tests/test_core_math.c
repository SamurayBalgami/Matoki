/* tests/test_core_math.c — çekirdek matematik testleri.
 * sin/cos doğruluğu, projeksiyon derinlik konvansiyonu (0..1), look_at ve normal matrisi.
 * Referans değerler f64 Taylor serisinden hesaplanır (libm yok). */
#include "test.h"
#include "core/math.h"

static f64 ref_sin(f64 x) {
    const f64 tau = 6.283185307179586;
    x -= tau * __builtin_floor(x / tau + 0.5);
    f64 term = x, sum = x;
    for (u32 n = 1; n < 20; n++) {
        term *= -x * x / (f64)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

TEST(math_sin_cos_accuracy) {
    f64 max_err = 0;
    for (i32 i = -2000; i <= 2000; i++) {
        f32 x = (f32)i * 0.01f;
        f64 e1 = (f64)m_sinf(x) - ref_sin((f64)x);
        f64 e2 = (f64)m_cosf(x) - ref_sin((f64)x + 1.5707963267948966);
        if (e1 < 0) e1 = -e1;
        if (e2 < 0) e2 = -e2;
        if (e1 > max_err) max_err = e1;
        if (e2 > max_err) max_err = e2;
    }
    test_note("sin/cos en büyük hata", max_err, 9);
    CHECK(max_err < 2e-6);
}

TEST(math_atan2_accuracy) {
    f64 max_err = 0;
    for (i32 i = 0; i < 3600; i++) {
        f64 ang = (f64)i * (6.283185307179586 / 3600.0) - 3.141592653589793 + 1e-4;
        f64 r = 0.5 + (f64)(i % 7);
        f32 y = (f32)(r * ref_sin(ang)), x = (f32)(r * ref_sin(ang + 1.5707963267948966));
        f64 e = (f64)m_atan2f(y, x) - ang;
        if (e > 3.141592653589793) e -= 6.283185307179586;
        if (e < -3.141592653589793) e += 6.283185307179586;
        if (e < 0) e = -e;
        if (e > max_err) max_err = e;
    }
    test_note("atan2 en büyük hata (rad)", max_err, 8);
    CHECK(max_err < 2e-5);
    CHECK(m_atan2f(0.0f, 0.0f) == 0.0f);
}

TEST(math_perspective_depth_range) {
    Mat4 p = mat4_perspective(1.0f, 1.5f, 0.5f, 100.0f);
    Vec4 n = mat4_mul_v4(&p, v4(0, 0, -0.5f, 1));
    Vec4 f = mat4_mul_v4(&p, v4(0, 0, -100.0f, 1));
    CHECK_NEAR(n.z / n.w, 0.0, 1e-6);
    CHECK_NEAR(f.z / f.w, 1.0, 1e-5);
    Vec4 b = mat4_mul_v4(&p, v4(0, 0, 1.0f, 1));   /* kameranın arkası → z < 0 */
    CHECK(b.z < 0.0f);
}

TEST(math_look_at_and_normal_matrix) {
    Mat4 v = mat4_look_at(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
    Vec3 o = mat4_mul_point(&v, v3(0, 0, 0));
    CHECK_NEAR(o.z, -5.0, 1e-5);
    Vec3 r = mat4_mul_point(&v, v3(1, 0, 5));
    CHECK_NEAR(r.x, 1.0, 1e-5);

    /* ölçek (2,1,1): normal (1,1,0)/√2 → ters-transpoz sonrası (0.5,1,0) yönünde */
    Mat4 s = mat4_scale(v3(2, 1, 1));
    Mat3 nm = mat3_normal_from_mat4(&s);
    Vec3 n = v3_norm(mat3_mul_v3(&nm, v3(1, 1, 0)));
    Vec3 e = v3_norm(v3(0.5f, 1, 0));
    CHECK_NEAR(n.x, e.x, 1e-5);
    CHECK_NEAR(n.y, e.y, 1e-5);

    Mat4 a = mat4_mul(mat4_translate(v3(1, 2, 3)), mat4_rotate_y(0.7f));
    Vec3 p = mat4_mul_point(&a, v3(0, 0, 0));
    CHECK_NEAR(p.x, 1.0, 1e-6);
    CHECK_NEAR(p.z, 3.0, 1e-6);
}
