/* phys/interp.c — fizik ↔ görüntü ara değerlemesi (D39). Konum doğrusal, dönüş nlerp (kısa yol, normalize).
 * Önceki ve şimdiki poz bit bit aynıysa (uyuyan / statik gövde) gövdeye dokunulmaz: ara değer yuvarlaması
 * kımıldamayan gövdeyi titretmez, geçmişi olmayan karede (ör. ölçüm görünümleri) kare birebir aynı kalır. */
#include "phys/interp.h"
#include "core/arena.h"
#include "core/mem.h"

void phys_interp_init(PhysInterp* p, Arena* a, u32 cap) {
    mem_zero(p, sizeof(*p));
    p->prev_pos = ARENA_ARRAY(a, Vec3, cap);
    p->prev_rot = ARENA_ARRAY(a, Quat, cap);
    p->cur_pos = ARENA_ARRAY(a, Vec3, cap);
    p->cur_rot = ARENA_ARRAY(a, Quat, cap);
    p->cur_rot_m = ARENA_ARRAY(a, Mat3, cap);
    if (p->prev_pos && p->prev_rot && p->cur_pos && p->cur_rot && p->cur_rot_m) p->cap = cap;
}

void phys_interp_reset(PhysInterp* p) { p->count = 0; }

void phys_interp_save(PhysInterp* p, const PhysWorld* w) {
    u32 n = w->count < p->cap ? w->count : p->cap;
    for (u32 i = 0; i < n; i++) {
        p->prev_pos[i] = w->bodies[i].pos;
        p->prev_rot[i] = w->bodies[i].rot;
    }
    p->count = n;
}

static b32 v3_same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static b32 quat_same(Quat a, Quat b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

static Vec3 lerp3(Vec3 a, Vec3 b, f32 t) {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

void phys_interp_apply(PhysInterp* p, PhysWorld* w, f32 alpha) {
    u32 n = p->count < w->count ? p->count : w->count;
    p->applied = n;
    for (u32 i = 0; i < n; i++) {
        Body* b = &w->bodies[i];
        p->cur_pos[i] = b->pos;
        p->cur_rot[i] = b->rot;
        p->cur_rot_m[i] = b->rot_m;
        Vec3 p0 = p->prev_pos[i];
        Quat q0 = p->prev_rot[i], q1 = b->rot;
        if (v3_same(p0, b->pos) && quat_same(q0, q1)) continue;
        b->pos = lerp3(p0, b->pos, alpha);
        if (!quat_same(q0, q1)) {
            f32 d = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
            f32 s = d < 0.0f ? -1.0f : 1.0f;   /* q ve −q aynı dönüş: kısa yoldan */
            f32 u = 1.0f - alpha, v = alpha * s;
            b->rot = quat_normalize(quat(q0.x * u + q1.x * v, q0.y * u + q1.y * v, q0.z * u + q1.z * v, q0.w * u + q1.w * v));
            b->rot_m = mat3_from_quat(b->rot);
        }
    }
}

void phys_interp_restore(PhysInterp* p, PhysWorld* w) {
    for (u32 i = 0; i < p->applied; i++) {
        Body* b = &w->bodies[i];
        b->pos = p->cur_pos[i];
        b->rot = p->cur_rot[i];
        b->rot_m = p->cur_rot_m[i];
    }
    p->applied = 0;
}

void phys_interp_pts_init(PhysInterpPts* p, Arena* a, u32 cap) {
    mem_zero(p, sizeof(*p));
    p->prev = ARENA_ARRAY(a, Vec3, cap);
    p->cur = ARENA_ARRAY(a, Vec3, cap);
    if (p->prev && p->cur) p->cap = cap;
}

void phys_interp_pts_reset(PhysInterpPts* p) { p->count = 0; }

void phys_interp_pts_save(PhysInterpPts* p, u32 at, const Vec3* x, u32 n) {
    if (at >= p->cap) return;
    if (n > p->cap - at) n = p->cap - at;
    mem_copy(p->prev + at, x, (usize)n * sizeof(Vec3));
    if (at + n > p->count) p->count = at + n;
}

/* [at, at + n) ∩ [0, count): geçmişi olan bölüm */
static u32 pts_span(const PhysInterpPts* p, u32 at, u32 n) {
    if (at >= p->count) return 0;
    return n < p->count - at ? n : p->count - at;
}

void phys_interp_pts_apply(PhysInterpPts* p, u32 at, Vec3* x, u32 n, f32 alpha) {
    n = pts_span(p, at, n);
    mem_copy(p->cur + at, x, (usize)n * sizeof(Vec3));
    for (u32 i = 0; i < n; i++) {
        if (!v3_same(p->prev[at + i], x[i])) x[i] = lerp3(p->prev[at + i], x[i], alpha);
    }
}

void phys_interp_pts_restore(const PhysInterpPts* p, u32 at, Vec3* x, u32 n) {
    n = pts_span(p, at, n);
    mem_copy(x, p->cur + at, (usize)n * sizeof(Vec3));
}
