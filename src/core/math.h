/* core/math.h — MATOKI vektör/matris matematiği ve libc'siz skaler fonksiyonlar.
 * Mat4 sütun-öncelikli (m[c*4 + r]); vektörler sütun vektörü (M * v).
 * Projeksiyon WebGPU/D3D konvansiyonunda: sağ el dünya, kamera -Z'ye bakar,
 * klip derinliği 0 (yakın) .. 1 (uzak). Ayrıntı: docs/DECISIONS.md. */
#ifndef CORE_MATH_H
#define CORE_MATH_H
#include "core/types.h"

#define M_PI_F  3.14159265358979f
#define M_TAU_F 6.28318530717959f
#define M_HALF_PI_F 1.57079632679490f
#define M_DEG2RAD_F 0.01745329251994f
#define M_RAD2DEG_F 57.2957795130823f

typedef struct IVec2 { i32 x, y; } IVec2;
typedef struct Vec2 { f32 x, y; } Vec2;
typedef struct Vec3 { f32 x, y, z; } Vec3;
typedef struct Vec4 { f32 x, y, z, w; } Vec4;
typedef struct Mat3 { f32 m[9]; } Mat3;   /* sütun-öncelikli: m[c*3 + r] */
typedef struct Mat4 { f32 m[16]; } Mat4;  /* sütun-öncelikli: m[c*4 + r] */

/* ---- skaler ---- */
static inline f32 m_sqrtf(f32 x) { return __builtin_sqrtf(x); }
static inline f32 m_absf(f32 x) { return __builtin_fabsf(x); }
static inline f32 m_floorf(f32 x) { return __builtin_floorf(x); }
static inline f32 m_ceilf(f32 x) { return __builtin_ceilf(x); }
static inline f32 m_truncf(f32 x) { return __builtin_truncf(x); }
static inline f32 m_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 m_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 m_clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline f32 m_lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

static inline u32 m_f32_bits(f32 x) {
    union { f32 f; u32 u; } c;
    c.f = x;
    return c.u;
}
static inline f32 m_bits_f32(u32 x) {
    union { f32 f; u32 u; } c;
    c.u = x;
    return c.f;
}

/* Doygun f32 → i32 (NaN → 0); wasm i32.trunc_sat_f32_s ile aynı sonuç. */
static inline i32 m_f2i_sat(f32 x) {
    if (x != x) return 0;
    if (x >= 2147483648.0f) return 2147483647;
    if (x <= -2147483648.0f) return -2147483647 - 1;
    return (i32)x;
}

f32 m_sinf(f32 x);
f32 m_cosf(f32 x);
f32 m_tanf(f32 x);
f32 m_atan2f(f32 y, f32 x);   /* minimax polinom, |hata| < 2e-5 rad */
/* Üstel / logaritma; Matoki exp2/log2/exp/log/pow ile BİT-BİT aynı işlem sırası (göreli hata ~2e-7).
 * exp2: x [-150, 128]'e kenetlenir (→ 0 / inf); log2: x<0 → NaN, ±0 ve denormal → -inf. */
f32 m_exp2f(f32 x);
f32 m_log2f(f32 x);
static inline f32 m_expf(f32 x) { return m_exp2f(x * 1.44269504f); }
static inline f32 m_logf(f32 x) { return m_log2f(x) * 0.693147181f; }
static inline f32 m_powf(f32 x, f32 y) { return m_exp2f(y * m_log2f(x)); }

/* ---- IVec2 / Vec2 ---- */
static inline IVec2 iv2(i32 x, i32 y) { IVec2 r = { x, y }; return r; }
static inline Vec2 v2(f32 x, f32 y) { Vec2 r = { x, y }; return r; }
static inline Vec2 v2_add(Vec2 a, Vec2 b) { return v2(a.x + b.x, a.y + b.y); }
static inline Vec2 v2_sub(Vec2 a, Vec2 b) { return v2(a.x - b.x, a.y - b.y); }
static inline Vec2 v2_scale(Vec2 a, f32 s) { return v2(a.x * s, a.y * s); }
static inline f32  v2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
static inline f32  v2_len(Vec2 a) { return m_sqrtf(v2_dot(a, a)); }
static inline Vec2 v2_norm(Vec2 a) {
    f32 l = v2_len(a);
    return l > 0.0f ? v2_scale(a, 1.0f / l) : a;
}

/* ---- Vec3 ---- */
static inline Vec3 v3(f32 x, f32 y, f32 z) { Vec3 r = { x, y, z }; return r; }
static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 v3_mul(Vec3 a, Vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline Vec3 v3_scale(Vec3 a, f32 s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline f32  v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 v3_cross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline f32  v3_len(Vec3 a) { return m_sqrtf(v3_dot(a, a)); }
static inline Vec3 v3_norm(Vec3 a) {
    f32 l = v3_len(a);
    return l > 0.0f ? v3_scale(a, 1.0f / l) : a;
}
static inline Vec3 v3_lerp(Vec3 a, Vec3 b, f32 t) {
    return v3(m_lerpf(a.x, b.x, t), m_lerpf(a.y, b.y, t), m_lerpf(a.z, b.z, t));
}

/* ---- Vec4 ---- */
static inline Vec4 v4(f32 x, f32 y, f32 z, f32 w) { Vec4 r = { x, y, z, w }; return r; }
static inline Vec4 v4_from3(Vec3 a, f32 w) { return v4(a.x, a.y, a.z, w); }
static inline Vec4 v4_add(Vec4 a, Vec4 b) { return v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline Vec4 v4_sub(Vec4 a, Vec4 b) { return v4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
static inline Vec4 v4_scale(Vec4 a, f32 s) { return v4(a.x * s, a.y * s, a.z * s, a.w * s); }
static inline f32  v4_dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/* ---- Mat4 ---- */
Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);                 /* a * b */
Vec4 mat4_mul_v4(const Mat4* m, Vec4 v);
Vec3 mat4_mul_point(const Mat4* m, Vec3 p);    /* w = 1, bölme yok (afin) */
Vec3 mat4_mul_dir(const Mat4* m, Vec3 d);      /* w = 0 */
Mat4 mat4_translate(Vec3 t);
Mat4 mat4_scale(Vec3 s);
Mat4 mat4_rotate_x(f32 a);
Mat4 mat4_rotate_y(f32 a);
Mat4 mat4_rotate_z(f32 a);
Mat4 mat4_perspective(f32 fov_y, f32 aspect, f32 znear, f32 zfar);
Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up);
f32  mat4_max_scale(const Mat4* m);            /* üst 3x3 sütunlarının en büyük uzunluğu */
/* genel ters (kofaktör); |det| < 1e-30 → birim matris */
Mat4 mat4_inverse(Mat4 m);

/* ---- Mat3: 2D afin (gfx2d / ui) ---- */
Mat3 mat3_identity(void);
Mat3 mat3_mul(const Mat3* a, const Mat3* b);   /* a * b */
Mat3 mat3_2d(f32 tx, f32 ty, f32 rot, f32 sx, f32 sy);   /* öteleme · dönme · ölçek */
Vec2 mat3_mul_point(const Mat3* m, Vec2 p);    /* (x, y, 1) */

/* Normal matrisi: üst 3x3'ün ters-transpozu (kofaktör / determinant). */
Mat3 mat3_normal_from_mat4(const Mat4* m);
Vec3 mat3_mul_v3(const Mat3* m, Vec3 v);

#endif
