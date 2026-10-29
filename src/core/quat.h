/* core/quat.h — birim quaternion (x, y, z, w); v' = q v q*. Fizik (phys/pmath.h), sahne dönüşümü
 * (scene/transform) ve iskelet animasyonu aynı tipi kullanır (D37 faz 3: iki depodaki ayrı Quat birleşti).
 * Fonksiyonlar phys/pmath.h'deki eski hallerinin birebir kopyası → fizik durum hash'leri değişmez.
 * Tümü satır içi ve belirleyici. */
#ifndef CORE_QUAT_H
#define CORE_QUAT_H
#include "core/math.h"

typedef struct Quat {
    f32 x, y, z, w;
} Quat;

static inline Quat quat(f32 x, f32 y, f32 z, f32 w) { Quat q = { x, y, z, w }; return q; }
static inline Quat quat_identity(void) { Quat q = { 0, 0, 0, 1 }; return q; }

static inline Quat quat_mul(Quat a, Quat b) {
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

/* sıfır / NaN uzunluk → birim */
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

#endif
