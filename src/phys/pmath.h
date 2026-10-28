/* phys/pmath.h — fizik matematiği: quaternion, rotasyon matrisi, ortonormal taban.
 * Quat (x, y, z, w), birim; v' = q v q*. Mat3 sütun-öncelikli (core/math.h): sütun k =
 * gövdenin yerel k ekseninin dünya yönü. Tüm fonksiyonlar satır içi ve belirleyici. */
#ifndef PHYS_PMATH_H
#define PHYS_PMATH_H
#include "core/math.h"

typedef struct Quat {
    f32 x, y, z, w;
} Quat;

static inline Quat quat_identity(void) { Quat q = { 0, 0, 0, 1 }; return q; }

static inline Quat quat_mul(Quat a, Quat b) {
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static inline Quat quat_normalize(Quat q) {
    f32 l = m_sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 0.0f)) return quat_identity();
    f32 s = 1.0f / l;
    Quat r = { q.x * s, q.y * s, q.z * s, q.w * s };
    return r;
}

static inline Quat quat_from_axis_angle(Vec3 axis, f32 angle) {
    Vec3 a = v3_norm(axis);
    f32 s = m_sinf(angle * 0.5f);
    Quat q = { a.x * s, a.y * s, a.z * s, m_cosf(angle * 0.5f) };
    return q;
}

/* v + 2w(q×v) + 2 q×(q×v) */
static inline Vec3 quat_rotate(Quat q, Vec3 v) {
    Vec3 u = v3(q.x, q.y, q.z);
    Vec3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}

/* q += 0.5 * dt * (w, 0) * q; normalize */
static inline Quat quat_integrate(Quat q, Vec3 w, f32 dt) {
    Quat wq = { w.x, w.y, w.z, 0.0f };
    Quat d = quat_mul(wq, q);
    f32 h = 0.5f * dt;
    Quat r = { q.x + d.x * h, q.y + d.y * h, q.z + d.z * h, q.w + d.w * h };
    return quat_normalize(r);
}

static inline Mat3 mat3_from_quat(Quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat3 m;
    m.m[0] = 1.0f - 2.0f * (yy + zz); m.m[1] = 2.0f * (xy + wz);        m.m[2] = 2.0f * (xz - wy);
    m.m[3] = 2.0f * (xy - wz);        m.m[4] = 1.0f - 2.0f * (xx + zz); m.m[5] = 2.0f * (yz + wx);
    m.m[6] = 2.0f * (xz + wy);        m.m[7] = 2.0f * (yz - wx);        m.m[8] = 1.0f - 2.0f * (xx + yy);
    return m;
}

static inline Vec3 mat3_col(const Mat3* m, u32 c) { return v3(m->m[c * 3], m->m[c * 3 + 1], m->m[c * 3 + 2]); }

/* R^T v (dünya → yerel) */
static inline Vec3 mat3_tmul_v3(const Mat3* m, Vec3 v) {
    return v3(m->m[0] * v.x + m->m[1] * v.y + m->m[2] * v.z,
              m->m[3] * v.x + m->m[4] * v.y + m->m[5] * v.z,
              m->m[6] * v.x + m->m[7] * v.y + m->m[8] * v.z);
}

/* R diag(d) R^T */
static inline Mat3 mat3_rotate_diag(const Mat3* r, Vec3 d) {
    const f32 dd[3] = { d.x, d.y, d.z };
    Mat3 o;
    for (u32 c = 0; c < 3; c++) {
        for (u32 row = 0; row < 3; row++) {
            f32 s = 0.0f;
            for (u32 k = 0; k < 3; k++) s += r->m[k * 3 + row] * dd[k] * r->m[k * 3 + c];
            o.m[c * 3 + row] = s;
        }
    }
    return o;
}

/* n'ye dik ortonormal t1, t2 (Duff vd. 2017, dalsız) */
static inline void ortho_basis(Vec3 n, Vec3* t1, Vec3* t2) {
    f32 s = n.z >= 0.0f ? 1.0f : -1.0f;
    f32 a = -1.0f / (s + n.z);
    f32 b = n.x * n.y * a;
    *t1 = v3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    *t2 = v3(b, s + n.y * n.y * a, -n.y);
}

static inline Mat4 mat4_from_pos_rot_scale(Vec3 p, Quat q, Vec3 s) {
    Mat3 r = mat3_from_quat(q);
    Mat4 m = mat4_identity();
    for (u32 c = 0; c < 3; c++) {
        f32 k = c == 0 ? s.x : (c == 1 ? s.y : s.z);
        m.m[c * 4 + 0] = r.m[c * 3 + 0] * k;
        m.m[c * 4 + 1] = r.m[c * 3 + 1] * k;
        m.m[c * 4 + 2] = r.m[c * 3 + 2] * k;
    }
    m.m[12] = p.x;
    m.m[13] = p.y;
    m.m[14] = p.z;
    return m;
}

#endif
