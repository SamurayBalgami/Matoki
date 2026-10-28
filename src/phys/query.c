/* phys/query.c — ışın sorguları ve dış itki/uyandırma yardımcıları (araç, karakter, oyun kodu).
 * Raycast: önce AABB slab testiyle eleme, sonra şekle özel kesişim (düzlem, küre, döndürülmüş
 * kutu — yerel uzayda slab). BVH modunda statik + dinamik ağaç taraması (Faz 7), SAP modunda doğrusal;
 * iki yol aynı isabeti verir (en küçük t, eşitlikte büyük indeks). */
#include "phys/phys.h"
#include "phys/shapes.h"

static b32 ray_aabb(Vec3 o, Vec3 inv_d, Vec3 mn, Vec3 mx, f32 tmax) {
    f32 t0 = 0.0f, t1 = tmax;
    const f32 oo[3] = { o.x, o.y, o.z }, id[3] = { inv_d.x, inv_d.y, inv_d.z };
    const f32 lo[3] = { mn.x, mn.y, mn.z }, hi[3] = { mx.x, mx.y, mx.z };
    for (u32 k = 0; k < 3; k++) {
        f32 a = (lo[k] - oo[k]) * id[k], b = (hi[k] - oo[k]) * id[k];
        if (a > b) { f32 t = a; a = b; b = t; }
        t0 = m_maxf(t0, a);
        t1 = m_minf(t1, b);
        if (t0 > t1) return FALSE;
    }
    return TRUE;
}

static b32 ray_plane(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    f32 denom = v3_dot(b->plane_n, d);
    if (denom >= -1e-8f) return FALSE;   /* paralel veya arkadan */
    f32 tt = (b->plane_d - v3_dot(b->plane_n, o)) / denom;
    if (tt < 0.0f || tt > tmax) return FALSE;
    *t = tt;
    *n = b->plane_n;
    return TRUE;
}

static b32 ray_sphere(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    Vec3 m = v3_sub(o, b->pos);
    f32 bb = v3_dot(m, d);
    f32 c = v3_dot(m, m) - b->radius * b->radius;
    if (c > 0.0f && bb > 0.0f) return FALSE;
    f32 disc = bb * bb - c;
    if (disc < 0.0f) return FALSE;
    f32 tt = -bb - m_sqrtf(disc);
    if (tt < 0.0f) tt = 0.0f;   /* içeriden */
    if (tt > tmax) return FALSE;
    *t = tt;
    Vec3 p = v3_add(o, v3_scale(d, tt));
    *n = c <= 0.0f ? v3_scale(d, -1.0f) : v3_norm(v3_sub(p, b->pos));
    return TRUE;
}

static b32 ray_box(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    Vec3 lo = mat3_tmul_v3(&b->rot_m, v3_sub(o, b->pos));
    Vec3 ld = mat3_tmul_v3(&b->rot_m, d);
    const f32 oo[3] = { lo.x, lo.y, lo.z }, dd[3] = { ld.x, ld.y, ld.z };
    const f32 h[3] = { b->half.x, b->half.y, b->half.z };
    f32 t0 = -3.4e38f, t1 = 3.4e38f;
    i32 axis = -1;
    f32 sign = 0.0f;
    for (u32 k = 0; k < 3; k++) {
        if (m_absf(dd[k]) < 1e-12f) {
            if (oo[k] < -h[k] || oo[k] > h[k]) return FALSE;
            continue;
        }
        f32 inv = 1.0f / dd[k];
        f32 a = (-h[k] - oo[k]) * inv, c = (h[k] - oo[k]) * inv;
        f32 s = -1.0f;   /* a'da giriş: -h yüzü */
        if (a > c) { f32 tmp = a; a = c; c = tmp; s = 1.0f; }
        if (a > t0) { t0 = a; axis = (i32)k; sign = s; }
        t1 = m_minf(t1, c);
        if (t0 > t1) return FALSE;
    }
    if (t1 < 0.0f) return FALSE;
    if (t0 < 0.0f || axis < 0) {   /* başlangıç kutunun içinde */
        if (tmax < 0.0f) return FALSE;
        *t = 0.0f;
        *n = v3_scale(d, -1.0f);
        return TRUE;
    }
    if (t0 > tmax) return FALSE;
    *t = t0;
    *n = v3_scale(mat3_col(&b->rot_m, (u32)axis), sign);
    return TRUE;
}

static b32 ray_sphere_at(Vec3 c, f32 r, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    Vec3 m = v3_sub(o, c);
    f32 bb = v3_dot(m, d), cc = v3_dot(m, m) - r * r;
    if (cc > 0.0f && bb > 0.0f) return FALSE;
    f32 disc = bb * bb - cc;
    if (disc < 0.0f) return FALSE;
    f32 tt = m_maxf(-bb - m_sqrtf(disc), 0.0f);
    if (tt > tmax) return FALSE;
    *t = tt;
    *n = v3_norm(v3_sub(v3_add(o, v3_scale(d, tt)), c));
    return TRUE;
}

static b32 ray_capsule(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    Vec3 ax = mat3_col(&b->rot_m, 1);
    Vec3 p0 = v3_sub(b->pos, v3_scale(ax, b->half_height));
    f32 L = 2.0f * b->half_height, r = b->radius;
    Vec3 m = v3_sub(o, p0);
    f32 s0 = m_clampf(v3_dot(m, ax), 0.0f, L);
    Vec3 cl = v3_sub(m, v3_scale(ax, s0));
    if (v3_dot(cl, cl) <= r * r) {   /* içeriden */
        *t = 0.0f;
        *n = v3_scale(d, -1.0f);
        return TRUE;
    }
    b32 hit = FALSE;
    f32 best = tmax;
    Vec3 md = v3_sub(m, v3_scale(ax, v3_dot(m, ax))), dd = v3_sub(d, v3_scale(ax, v3_dot(d, ax)));
    f32 a = v3_dot(dd, dd), bq = v3_dot(md, dd), c = v3_dot(md, md) - r * r;
    if (a > 1e-12f) {
        f32 disc = bq * bq - a * c;
        if (disc >= 0.0f) {
            f32 tt = (-bq - m_sqrtf(disc)) / a;
            f32 s = v3_dot(v3_add(m, v3_scale(d, tt)), ax);
            if (tt >= 0.0f && tt <= best && s >= 0.0f && s <= L) {
                best = tt;
                hit = TRUE;
                *n = v3_norm(v3_sub(v3_add(m, v3_scale(d, tt)), v3_scale(ax, s)));
            }
        }
    }
    const Vec3 ends[2] = { p0, v3_add(p0, v3_scale(ax, L)) };
    for (u32 i = 0; i < 2; i++) {
        f32 tt;
        Vec3 nn;
        if (ray_sphere_at(ends[i], r, o, d, best, &tt, &nn) && tt <= best) {
            best = tt;
            hit = TRUE;
            *n = nn;
        }
    }
    if (hit) *t = best;
    return hit;
}

static b32 ray_hull(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    f32 t0 = 0.0f, t1 = tmax;
    Vec3 nn = v3_scale(d, -1.0f);
    for (u32 f = 0; f < b->hull->fcount; f++) {
        Vec3 fn = mat3_mul_v3(&b->rot_m, b->hull->faces[f].n);
        f32 fd = b->hull->faces[f].d + v3_dot(fn, b->pos);
        f32 denom = v3_dot(fn, d), dist = v3_dot(fn, o) - fd;
        if (m_absf(denom) < 1e-12f) {
            if (dist > 0.0f) return FALSE;
            continue;
        }
        f32 tt = -dist / denom;
        if (denom < 0.0f) {
            if (tt > t0) { t0 = tt; nn = fn; }
        } else {
            t1 = m_minf(t1, tt);
        }
        if (t0 > t1) return FALSE;
    }
    *t = t0;
    *n = nn;
    return TRUE;
}

static b32 ray_mesh(const Body* b, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n) {
    Vec3 lo = mat3_tmul_v3(&b->rot_m, v3_sub(o, b->pos)), ld = mat3_tmul_v3(&b->rot_m, d);
    Vec3 ln;
    u32 tri;
    if (!trimesh_raycast(b->mesh, lo, ld, tmax, t, &ln, &tri)) return FALSE;
    *n = mat3_mul_v3(&b->rot_m, ln);
    return TRUE;
}

/* tek gövde testi; doğrusal tarama ile aynı kural: t ≤ best kabul, eşitlikte büyük indeks kazanır */
typedef struct RayCtx {
    const PhysWorld* w;
    Vec3 o, d, inv;
    u32  ignore;
    b32  any;
    f32  best;
    RayHit* out;
} RayCtx;

static void ray_body(RayCtx* c, u32 i) {
    if (i == c->ignore) return;
    const Body* b = &c->w->bodies[i];
    f32 t;
    Vec3 n;
    b32 hit = FALSE;
    if (b->shape == SHAPE_PLANE) {
        hit = ray_plane(b, c->o, c->d, c->best, &t, &n);
    } else {
        if (!ray_aabb(c->o, c->inv, b->aabb_min, b->aabb_max, c->best)) return;
        switch (b->shape) {
        case SHAPE_SPHERE: hit = ray_sphere(b, c->o, c->d, c->best, &t, &n); break;
        case SHAPE_CAPSULE: hit = ray_capsule(b, c->o, c->d, c->best, &t, &n); break;
        case SHAPE_BOX: hit = ray_box(b, c->o, c->d, c->best, &t, &n); break;
        case SHAPE_HULL: hit = ray_hull(b, c->o, c->d, c->best, &t, &n); break;
        case SHAPE_MESH: hit = ray_mesh(b, c->o, c->d, c->best, &t, &n); break;
        default: break;
        }
    }
    if (!hit || t > c->best) return;
    if (c->any && t == c->best && i < c->out->body) return;
    c->best = t;
    c->any = TRUE;
    c->out->body = i;
    c->out->t = t;
    c->out->point = v3_add(c->o, v3_scale(c->d, t));
    c->out->normal = n;
}

static void ray_tree_cb(void* ctx, u32 body, f32* tmax) {
    RayCtx* c = (RayCtx*)ctx;
    ray_body(c, body);
    *tmax = c->best;
}

b32 phys_raycast(const PhysWorld* w, Vec3 o, Vec3 d, f32 max_t, u32 ignore, RayHit* out) {
    Vec3 inv = v3(d.x != 0.0f ? 1.0f / d.x : 3.4e38f, d.y != 0.0f ? 1.0f / d.y : 3.4e38f, d.z != 0.0f ? 1.0f / d.z : 3.4e38f);
    if (w->broadphase == PHYS_BP_BVH) {
        RayCtx c = { w, o, d, inv, ignore, FALSE, max_t, out };
        for (u32 p = 0; p < w->plane_count; p++) ray_body(&c, w->plane_ids[p]);
        tree_raycast(&w->static_tree, o, d, c.best, ray_tree_cb, &c);
        tree_raycast(&w->dyn_tree, o, d, c.best, ray_tree_cb, &c);
        return c.any;
    }
    b32 any = FALSE;
    f32 best = max_t;
    for (u32 i = 0; i < w->count; i++) {
        if (i == ignore) continue;
        const Body* b = &w->bodies[i];
        f32 t;
        Vec3 n;
        b32 hit = FALSE;
        if (b->shape == SHAPE_PLANE) {
            hit = ray_plane(b, o, d, best, &t, &n);
        } else {
            if (!ray_aabb(o, inv, b->aabb_min, b->aabb_max, best)) continue;
            switch (b->shape) {
            case SHAPE_SPHERE: hit = ray_sphere(b, o, d, best, &t, &n); break;
            case SHAPE_CAPSULE: hit = ray_capsule(b, o, d, best, &t, &n); break;
            case SHAPE_BOX: hit = ray_box(b, o, d, best, &t, &n); break;
            case SHAPE_HULL: hit = ray_hull(b, o, d, best, &t, &n); break;
            case SHAPE_MESH: hit = ray_mesh(b, o, d, best, &t, &n); break;
            default: break;
            }
        }
        if (hit && t <= best) {
            best = t;
            any = TRUE;
            out->body = i;
            out->t = t;
            out->point = v3_add(o, v3_scale(d, t));
            out->normal = n;
        }
    }
    return any;
}

Vec3 phys_point_velocity(const Body* b, Vec3 p) { return v3_add(b->vel, v3_cross(b->ang_vel, v3_sub(p, b->pos))); }

f32 phys_effective_mass(const Body* b, Vec3 p, Vec3 n) {
    Vec3 r = v3_sub(p, b->pos);
    f32 k = b->inv_mass + v3_dot(n, v3_cross(mat3_mul_v3(&b->inv_inertia_world, v3_cross(r, n)), r));
    return k > 0.0f ? 1.0f / k : 0.0f;
}

void phys_apply_impulse_at(PhysWorld* w, u32 id, Vec3 J, Vec3 p) {
    Body* b = &w->bodies[id];
    if (b->inv_mass == 0.0f) return;
    phys_wake_body(w, id);
    b->vel = v3_add(b->vel, v3_scale(J, b->inv_mass));
    b->ang_vel = v3_add(b->ang_vel, mat3_mul_v3(&b->inv_inertia_world, v3_cross(v3_sub(p, b->pos), J)));
}

void phys_apply_force_at(PhysWorld* w, u32 id, Vec3 F, Vec3 p) {
    Body* b = &w->bodies[id];
    if (b->inv_mass == 0.0f) return;
    phys_wake_body(w, id);
    b->force = v3_add(b->force, F);
    b->torque = v3_add(b->torque, v3_cross(v3_sub(p, b->pos), F));
}

void phys_wake_body(PhysWorld* w, u32 id) {
    Body* b = &w->bodies[id];
    if (!(b->flags & BODY_ASLEEP)) return;
    u32 island = b->island;
    for (u32 i = 0; i < w->count; i++) {
        Body* o = &w->bodies[i];
        if ((o->flags & BODY_ASLEEP) && o->island == island) {
            o->flags &= ~BODY_ASLEEP;
            o->sleep_time = 0.0f;
        }
    }
}
