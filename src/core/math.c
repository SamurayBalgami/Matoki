/* core/math.c — math.h'nin satır içi olmayan kısımları.
 * sin/cos: [-π, π]'ye indirgeme + [-π/2, π/2]'ye yansıtma + 11. derece Taylor
 * (|hata| < 1e-7 bu aralıkta; çok büyük argümanlarda indirgeme hassasiyeti düşer). */
#include "core/math.h"

f32 m_sinf(f32 x) {
    if (x != x) return x;
    f32 k = m_floorf(x * (1.0f / M_TAU_F) + 0.5f);
    x = x - k * M_TAU_F;
    if (x > M_PI_F * 0.5f) x = M_PI_F - x;
    else if (x < -M_PI_F * 0.5f) x = -M_PI_F - x;
    f32 x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f +
                 x2 * (1.0f / 362880.0f + x2 * (-1.0f / 39916800.0f))))));
}

f32 m_cosf(f32 x) { return m_sinf(x + M_PI_F * 0.5f); }

f32 m_tanf(f32 x) { return m_sinf(x) / m_cosf(x); }

/* |x| <= 1 için atan (Abramowitz–Stegun tarzı 11. derece minimax) */
static f32 atan_unit(f32 x) {
    f32 x2 = x * x;
    return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f +
                x2 * (0.05265332f + x2 * (-0.01172120f))))));
}

f32 m_atan2f(f32 y, f32 x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    if (m_absf(x) >= m_absf(y)) {
        f32 a = atan_unit(y / x);
        if (x < 0.0f) a += y >= 0.0f ? M_PI_F : -M_PI_F;
        return a;
    }
    f32 a = atan_unit(x / y);
    return (y > 0.0f ? M_PI_F * 0.5f : -M_PI_F * 0.5f) - a;
}

Mat4 mat4_identity(void) {
    Mat4 r = { { 0 } };
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (u32 c = 0; c < 4; c++) {
        for (u32 row = 0; row < 4; row++) {
            f32 s = 0.0f;
            for (u32 k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Vec4 mat4_mul_v4(const Mat4* m, Vec4 v) {
    const f32* a = m->m;
    return v4(a[0] * v.x + a[4] * v.y + a[8] * v.z + a[12] * v.w,
              a[1] * v.x + a[5] * v.y + a[9] * v.z + a[13] * v.w,
              a[2] * v.x + a[6] * v.y + a[10] * v.z + a[14] * v.w,
              a[3] * v.x + a[7] * v.y + a[11] * v.z + a[15] * v.w);
}

Vec3 mat4_mul_point(const Mat4* m, Vec3 p) {
    const f32* a = m->m;
    return v3(a[0] * p.x + a[4] * p.y + a[8] * p.z + a[12],
              a[1] * p.x + a[5] * p.y + a[9] * p.z + a[13],
              a[2] * p.x + a[6] * p.y + a[10] * p.z + a[14]);
}

Vec3 mat4_mul_dir(const Mat4* m, Vec3 d) {
    const f32* a = m->m;
    return v3(a[0] * d.x + a[4] * d.y + a[8] * d.z,
              a[1] * d.x + a[5] * d.y + a[9] * d.z,
              a[2] * d.x + a[6] * d.y + a[10] * d.z);
}

Mat4 mat4_translate(Vec3 t) {
    Mat4 r = mat4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Mat4 mat4_scale(Vec3 s) {
    Mat4 r = mat4_identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

Mat4 mat4_rotate_x(f32 a) {
    Mat4 r = mat4_identity();
    f32 c = m_cosf(a), s = m_sinf(a);
    r.m[5] = c;  r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

Mat4 mat4_rotate_y(f32 a) {
    Mat4 r = mat4_identity();
    f32 c = m_cosf(a), s = m_sinf(a);
    r.m[0] = c; r.m[2] = -s;
    r.m[8] = s; r.m[10] = c;
    return r;
}

Mat4 mat4_rotate_z(f32 a) {
    Mat4 r = mat4_identity();
    f32 c = m_cosf(a), s = m_sinf(a);
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

/* z_klip = zf/(zn-zf) * z_görüş + zn*zf/(zn-zf), w = -z_görüş.
 * z_görüş = -zn → derinlik 0; z_görüş = -zf → derinlik 1. */
Mat4 mat4_perspective(f32 fov_y, f32 aspect, f32 znear, f32 zfar) {
    Mat4 r = { { 0 } };
    f32 f = 1.0f / m_tanf(fov_y * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = znear * zfar / (znear - zfar);
    return r;
}

Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = v3_norm(v3_sub(center, eye));
    Vec3 s = v3_norm(v3_cross(f, up));
    Vec3 u = v3_cross(s, f);
    Mat4 r = mat4_identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;   r.m[12] = -v3_dot(s, eye);
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;   r.m[13] = -v3_dot(u, eye);
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = v3_dot(f, eye);
    return r;
}

f32 mat4_max_scale(const Mat4* m) {
    const f32* a = m->m;
    f32 sx = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    f32 sy = a[4] * a[4] + a[5] * a[5] + a[6] * a[6];
    f32 sz = a[8] * a[8] + a[9] * a[9] + a[10] * a[10];
    return m_sqrtf(m_maxf(sx, m_maxf(sy, sz)));
}

/* Sütunları c0,c1,c2 olan M için kofaktör matrisinin sütunları
 * (c1×c2, c2×c0, c0×c1); ters-transpoz = kofaktör / det. */
Mat3 mat3_normal_from_mat4(const Mat4* m) {
    const f32* a = m->m;
    Vec3 c0 = v3(a[0], a[1], a[2]);
    Vec3 c1 = v3(a[4], a[5], a[6]);
    Vec3 c2 = v3(a[8], a[9], a[10]);
    Vec3 k0 = v3_cross(c1, c2);
    Vec3 k1 = v3_cross(c2, c0);
    Vec3 k2 = v3_cross(c0, c1);
    f32 det = v3_dot(c0, k0);
    f32 inv = det != 0.0f ? 1.0f / det : 0.0f;
    Mat3 r;
    r.m[0] = k0.x * inv; r.m[1] = k0.y * inv; r.m[2] = k0.z * inv;
    r.m[3] = k1.x * inv; r.m[4] = k1.y * inv; r.m[5] = k1.z * inv;
    r.m[6] = k2.x * inv; r.m[7] = k2.y * inv; r.m[8] = k2.z * inv;
    return r;
}

Vec3 mat3_mul_v3(const Mat3* m, Vec3 v) {
    const f32* a = m->m;
    return v3(a[0] * v.x + a[3] * v.y + a[6] * v.z,
              a[1] * v.x + a[4] * v.y + a[7] * v.z,
              a[2] * v.x + a[5] * v.y + a[8] * v.z);
}
