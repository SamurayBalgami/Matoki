/* phys/world.c — dünya, geniş faz (sweep-and-prune) ve sequential impulse çözücü.
 * Adım: hızları entegre et (yerçekimi) → çiftleri bul → manifoldları üret + warm start eşle →
 * ön hesap + warm start → N iterasyon (sürtünme, sonra normal) → konumları entegre et → CCD.
 * Sıra her adımda aynı (gövde dizisi, SAP sırası) → belirleyici. */
#include "phys/phys.h"
#include "phys/joints.h"
#include "phys/shapes.h"
#include "phys/gjk.h"
#include "phys/solver.h"
#include "core/mem.h"
#include "core/sys.h"

#define WARM_MATCH_DIST2 (0.05f * 0.05f)
#define RESTITUTION_THRESHOLD 1.0f
#define PHYS_DEFAULT_JOINTS 256

void phys_world_init(PhysWorld* w, Arena* a, u32 max_bodies, u32 max_manifolds) {
    mem_zero(w, sizeof(*w));
    w->bodies = ARENA_ARRAY(a, Body, max_bodies);
    w->cap = max_bodies;
    w->cur = ARENA_ARRAY(a, Manifold, max_manifolds);
    w->prev = ARENA_ARRAY(a, Manifold, max_manifolds);
    w->manifold_cap = max_manifolds;
    w->prev_index_cap = 1;
    while (w->prev_index_cap < max_manifolds * 2) w->prev_index_cap <<= 1;
    w->prev_index = ARENA_ARRAY(a, u32, w->prev_index_cap);
    w->order = ARENA_ARRAY(a, u32, max_bodies);
    w->uf_parent = ARENA_ARRAY(a, u32, max_bodies);
    w->uf_min_time = ARENA_ARRAY(a, f32, max_bodies);
    w->pair_cap = max_manifolds * 2;
    w->pairs = ARENA_ARRAY(a, u32, w->pair_cap * 2);
    w->joint_cap = PHYS_DEFAULT_JOINTS;
    w->joints = ARENA_ARRAY(a, Joint, w->joint_cap);
    if (max_bodies > 65536) sys_panic("phys: en çok 65536 gövde (çift anahtarı 16+16 bit)");
    tree_init(&w->static_tree, a, max_bodies * 2, 0.0f);
    tree_init(&w->dyn_tree, a, max_bodies * 2, PHYS_TREE_MARGIN);
    w->query_cap = max_bodies;
    w->query_buf = ARENA_ARRAY(a, u32, max_bodies);
    w->pair_tmp = ARENA_ARRAY(a, u32, (usize)w->pair_cap * 2 > max_bodies * 2 ? (usize)w->pair_cap * 2 : (usize)max_bodies * 2);
    w->broadphase = PHYS_BP_BVH;   /* D21 */
    w->pset = ARENA_ARRAY(a, u32, w->pair_cap);
    w->pset_hash_cap = 1;
    while (w->pset_hash_cap < w->pair_cap * 2) w->pset_hash_cap <<= 1;
    w->pset_hash = ARENA_ARRAY(a, u32, w->pset_hash_cap);
    w->moved = ARENA_ARRAY(a, u32, max_bodies);
    w->moved_flag = ARENA_ARRAY(a, u8, max_bodies);
    w->color_words = (max_bodies + 63) / 64;
    w->color_bits = ARENA_ARRAY(a, u64, (usize)w->color_words * PHYS_COLORS);
    w->color_order = ARENA_ARRAY(a, u32, max_manifolds);
    w->manifold_color = ARENA_ARRAY(a, u8, max_manifolds);
    w->graph_color = TRUE;   /* D20 */
#if PHYS_HAVE_SIMD
    w->wide_cap = max_manifolds / 4 + PHYS_COLORS + 1;
    w->wide = arena_alloc(a, solver_simd_bytes(w->wide_cap), 16);
    w->simd = TRUE;
#endif
    w->world_body.rot = quat_identity();
    w->world_body.shape = SHAPE_PLANE;
    w->world_body.rot_m = mat3_from_quat(w->world_body.rot);
    w->sleep_lin = 0.05f;
    w->sleep_ang = 0.05f;
    w->sleep_delay = 0.5f;
    w->gravity = v3(0, -9.81f, 0);
    w->planet_gm = 0.0;
    w->planet_radius = 0.0;
    w->solver = PHYS_SOLVER_SOFT;   /* D16: ölçümle seçildi */
    w->substeps = 4;
    w->contact_hertz = 30.0f;
    w->contact_damping = 10.0f;
    w->iterations = 10;
    w->warm_start = TRUE;
    w->speculative = TRUE;
    w->continuous = TRUE;
    w->mesh_edge_fix = TRUE;
    w->baumgarte = 0.2f;
    w->slop = 0.005f;
}

void phys_world_set_solver(PhysWorld* w, i32 solver) {
    w->solver = (solver >= 0 && solver < PHYS_SOLVER_COUNT) ? (PhysSolver)solver : PHYS_SOLVER_SI;
}

f32 phys_max_penetration(const PhysWorld* w) {
    f32 m = 0.0f;
    for (u32 i = 0; i < w->cur_count; i++) {
        for (u32 k = 0; k < w->cur[i].count; k++) m = m_maxf(m, w->cur[i].p[k].depth);
    }
    return m;
}

void phys_world_clear(PhysWorld* w) {
    w->count = 0;
    w->joint_count = 0;
    w->cur_count = w->prev_count = 0;
    w->order_count = 0;
    w->plane_count = 0;
    tree_clear(&w->static_tree);
    tree_clear(&w->dyn_tree);
    w->pset_count = 0;
    w->pset_valid = FALSE;
    w->moved_count = 0;
    mem_zero(w->moved_flag, w->cap);
    mem_zero(&w->stats, sizeof(w->stats));
}

BodyDesc phys_desc_box(Vec3 pos, Vec3 half, f32 mass) {
    BodyDesc d;
    mem_zero(&d, sizeof(d));
    d.shape = SHAPE_BOX;
    d.pos = pos;
    d.rot = quat_identity();
    d.half = half;
    d.mass = mass;
    d.friction = 0.6f;
    d.restitution = 0.1f;
    return d;
}

BodyDesc phys_desc_sphere(Vec3 pos, f32 radius, f32 mass) {
    BodyDesc d;
    mem_zero(&d, sizeof(d));
    d.shape = SHAPE_SPHERE;
    d.pos = pos;
    d.rot = quat_identity();
    d.radius = radius;
    d.mass = mass;
    d.friction = 0.6f;
    d.restitution = 0.2f;
    return d;
}

BodyDesc phys_desc_plane(Vec3 n, f32 d) {
    BodyDesc b;
    mem_zero(&b, sizeof(b));
    b.shape = SHAPE_PLANE;
    b.rot = quat_identity();
    b.plane_n = v3_norm(n);
    b.plane_d = d;
    b.friction = 0.6f;
    b.restitution = 0.1f;
    return b;
}

BodyDesc phys_desc_capsule(Vec3 pos, f32 radius, f32 half_height, f32 mass) {
    BodyDesc d = phys_desc_sphere(pos, radius, mass);
    d.shape = SHAPE_CAPSULE;
    d.half_height = half_height;
    d.restitution = 0.1f;
    return d;
}

BodyDesc phys_desc_hull(Vec3 pos, const ConvexHull* hull, f32 mass) {
    BodyDesc d = phys_desc_box(pos, v3(0, 0, 0), mass);
    d.shape = SHAPE_HULL;
    d.hull = hull;
    return d;
}

BodyDesc phys_desc_mesh(Vec3 pos, const TriMesh* mesh) {
    BodyDesc d = phys_desc_box(pos, v3(0, 0, 0), 0.0f);
    d.shape = SHAPE_MESH;
    d.mesh = mesh;
    return d;
}

/* yerel AABB (mn, mx) → dünya AABB (Arvo) + pay */
static void world_aabb(Body* b, Vec3 mn, Vec3 mx, f32 pad) {
    Vec3 c = v3_scale(v3_add(mn, mx), 0.5f), e = v3_scale(v3_sub(mx, mn), 0.5f);
    const f32* m = b->rot_m.m;
    Vec3 wc = v3_add(b->pos, mat3_mul_v3(&b->rot_m, c));
    Vec3 we = v3(m_absf(m[0]) * e.x + m_absf(m[3]) * e.y + m_absf(m[6]) * e.z + pad,
                 m_absf(m[1]) * e.x + m_absf(m[4]) * e.y + m_absf(m[7]) * e.z + pad,
                 m_absf(m[2]) * e.x + m_absf(m[5]) * e.y + m_absf(m[8]) * e.z + pad);
    b->aabb_min = v3_sub(wc, we);
    b->aabb_max = v3_add(wc, we);
}

/* asal eylemsizlik eksenlerinin dünya yönleri: hull'da R·Q, diğerlerinde R */
static Mat3 inertia_frame(const Body* b) {
    if (b->shape != SHAPE_HULL) return b->rot_m;
    const Mat3* q = &b->hull->inertia_axes;
    Mat3 o;
    for (u32 c = 0; c < 3; c++) {
        Vec3 col = mat3_mul_v3(&b->rot_m, v3(q->m[c * 3], q->m[c * 3 + 1], q->m[c * 3 + 2]));
        o.m[c * 3] = col.x;
        o.m[c * 3 + 1] = col.y;
        o.m[c * 3 + 2] = col.z;
    }
    return o;
}

void phys_body_update_derived(Body* b) {
    b->rot_m = mat3_from_quat(b->rot);
    Mat3 f = inertia_frame(b);
    b->inv_inertia_world = mat3_rotate_diag(&f, b->inv_inertia_local);
    if (b->shape == SHAPE_CAPSULE) {
        Vec3 ax = v3_scale(mat3_col(&b->rot_m, 1), b->half_height);
        f32 r = b->radius + PHYS_MARGIN;
        Vec3 p0 = v3_sub(b->pos, ax), p1 = v3_add(b->pos, ax);
        b->aabb_min = v3(m_minf(p0.x, p1.x) - r, m_minf(p0.y, p1.y) - r, m_minf(p0.z, p1.z) - r);
        b->aabb_max = v3(m_maxf(p0.x, p1.x) + r, m_maxf(p0.y, p1.y) + r, m_maxf(p0.z, p1.z) + r);
    } else if (b->shape == SHAPE_HULL) {
        world_aabb(b, b->hull->aabb_min, b->hull->aabb_max, PHYS_MARGIN);
    } else if (b->shape == SHAPE_MESH) {
        world_aabb(b, b->mesh->aabb_min, b->mesh->aabb_max, PHYS_MARGIN);
    } else if (b->shape == SHAPE_SPHERE) {
        Vec3 r = v3(b->radius + PHYS_MARGIN, b->radius + PHYS_MARGIN, b->radius + PHYS_MARGIN);
        b->aabb_min = v3_sub(b->pos, r);
        b->aabb_max = v3_add(b->pos, r);
    } else if (b->shape == SHAPE_BOX) {
        const f32* m = b->rot_m.m;
        Vec3 e = v3(m_absf(m[0]) * b->half.x + m_absf(m[3]) * b->half.y + m_absf(m[6]) * b->half.z + PHYS_MARGIN,
                    m_absf(m[1]) * b->half.x + m_absf(m[4]) * b->half.y + m_absf(m[7]) * b->half.z + PHYS_MARGIN,
                    m_absf(m[2]) * b->half.x + m_absf(m[5]) * b->half.y + m_absf(m[8]) * b->half.z + PHYS_MARGIN);
        b->aabb_min = v3_sub(b->pos, e);
        b->aabb_max = v3_add(b->pos, e);
    }
}

static inline void mark_moved(PhysWorld* w, u32 i) {
    if (w->moved_flag[i]) return;
    w->moved_flag[i] = 1;
    w->moved[w->moved_count++] = i;
}

u32 phys_add_body(PhysWorld* w, const BodyDesc* d) {
    if (w->count >= w->cap) sys_panic("phys: gövde kapasitesi dolu");
    u32 id = w->count++;
    Body* b = &w->bodies[id];
    mem_zero(b, sizeof(*b));
    b->shape = d->shape;
    b->pos = d->pos;
    b->rot = quat_normalize(d->rot);
    b->vel = d->vel;
    b->ang_vel = d->ang_vel;
    b->half = d->half;
    b->radius = d->radius;
    b->half_height = d->half_height;
    b->hull = d->hull;
    b->mesh = d->mesh;
    b->plane_n = d->plane_n;
    b->plane_d = d->plane_d;
    b->friction = d->friction;
    b->restitution = d->restitution;
    if (d->mass > 0.0f && d->shape != SHAPE_PLANE && d->shape != SHAPE_MESH) {
        f32 m = d->mass;
        b->inv_mass = 1.0f / m;
        if (d->shape == SHAPE_SPHERE) {
            f32 i = 0.4f * m * d->radius * d->radius;
            b->inv_inertia_local = v3(1.0f / i, 1.0f / i, 1.0f / i);
        } else if (d->shape == SHAPE_CAPSULE) {
            /* silindir (h = 2·half_height) + iki yarım küre; kütle hacimle bölünür */
            f32 r = d->radius, h = 2.0f * d->half_height;
            f32 vc = M_PI_F * r * r * h, vs = (4.0f / 3.0f) * M_PI_F * r * r * r;
            f32 mc = m * vc / (vc + vs), ms = m * vs / (vc + vs);   /* ms: iki yarım kürenin toplamı */
            f32 iy = mc * r * r * 0.5f + ms * 0.4f * r * r;
            f32 ix = mc * (h * h / 12.0f + r * r * 0.25f) + ms * (0.4f * r * r + h * h * 0.25f + 0.375f * h * r);
            b->inv_inertia_local = v3(1.0f / ix, 1.0f / iy, 1.0f / ix);
        } else if (d->shape == SHAPE_HULL) {
            /* gerçek tensör, asal eksenlerde (D18) */
            Vec3 in = d->hull->inertia;
            b->inv_inertia_local = v3(1.0f / (m * in.x), 1.0f / (m * in.y), 1.0f / (m * in.z));
        } else {
            Vec3 hh = d->half;
            f32 x2 = hh.x * hh.x, y2 = hh.y * hh.y, z2 = hh.z * hh.z;
            b->inv_inertia_local = v3(3.0f / (m * (y2 + z2)), 3.0f / (m * (x2 + z2)), 3.0f / (m * (x2 + y2)));
        }
    }
    switch (b->shape) {
    case SHAPE_SPHERE: b->min_extent = b->max_extent = b->radius; break;
    case SHAPE_CAPSULE: b->min_extent = b->radius; b->max_extent = b->radius + b->half_height; break;
    case SHAPE_BOX:
        b->min_extent = m_minf(b->half.x, m_minf(b->half.y, b->half.z));
        b->max_extent = v3_len(b->half);
        break;
    case SHAPE_HULL: b->min_extent = b->hull->inner_radius; b->max_extent = b->hull->outer_radius; break;
    default: break;
    }
    phys_body_update_derived(b);
    b->pos0 = b->pos;
    b->rot0 = b->rot;
    b->proxy = TREE_NULL;
    if (b->shape != SHAPE_PLANE) {
        w->order[w->order_count++] = id;
        b->proxy = tree_insert(b->inv_mass > 0.0f ? &w->dyn_tree : &w->static_tree, b->aabb_min, b->aabb_max, id);
        mark_moved(w, id);
    } else if (w->plane_count < PHYS_MAX_PLANES) {
        w->plane_ids[w->plane_count++] = id;
    }
    return id;
}

void phys_update_tree(PhysWorld* w) {
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        if (b->inv_mass > 0.0f && b->proxy != TREE_NULL && tree_move(&w->dyn_tree, b->proxy, b->aabb_min, b->aabb_max)) {
            w->stats.tree_moves++;
            mark_moved(w, i);
        }
    }
}

/* artan sıralama: küçük dizide ekleme, büyükte 8 bitlik geçişli radix (passes çift: 2 → değer < 2¹⁶,
 * 4 → tam 32 bit); sonuç v'de */
static void sort_u32(u32* v, u32 n, u32* tmp, u32 passes) {
    if (n <= 32) {
        for (u32 i = 1; i < n; i++) {
            u32 x = v[i], j = i;
            while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
            v[j] = x;
        }
        return;
    }
    u32* src = v;
    u32* dst = tmp;
    for (u32 shift = 0; shift < passes * 8; shift += 8) {
        u32 cnt[256] = { 0 };
        for (u32 i = 0; i < n; i++) cnt[(src[i] >> shift) & 255u]++;
        u32 sum = 0;
        for (u32 k = 0; k < 256; k++) { u32 c = cnt[k]; cnt[k] = sum; sum += c; }
        for (u32 i = 0; i < n; i++) dst[cnt[(src[i] >> shift) & 255u]++] = src[i];
        u32* t = src; src = dst; dst = t;
    }
}

static inline b32 aabb_overlap_box(const Body* a, Vec3 mn, Vec3 mx) {
    return a->aabb_min.x <= mx.x && a->aabb_max.x >= mn.x && a->aabb_min.y <= mx.y && a->aabb_max.y >= mn.y &&
           a->aabb_min.z <= mx.z && a->aabb_max.z >= mn.z;
}

u32 phys_query_aabb(PhysWorld* w, Vec3 mn, Vec3 mx, u32* out, u32 max) {
    u32 k = 0;
    if (w->broadphase == PHYS_BP_SAP) {
        for (u32 i = 0; i < w->count && k < max; i++) {
            const Body* b = &w->bodies[i];
            if (b->shape != SHAPE_PLANE && aabb_overlap_box(b, mn, mx)) out[k++] = i;
        }
        return k;
    }
    u32 n = tree_query(&w->static_tree, mn, mx, w->query_buf, w->query_cap);
    n += tree_query(&w->dyn_tree, mn, mx, w->query_buf + n, w->query_cap - n);
    for (u32 i = 0; i < n; i++) {
        if (aabb_overlap_box(&w->bodies[w->query_buf[i]], mn, mx)) w->query_buf[k++] = w->query_buf[i];
    }
    sort_u32(w->query_buf, k, w->pair_tmp, 2);
    if (k > max) k = max;
    if (out != w->query_buf) mem_copy(out, w->query_buf, sizeof(u32) * k);
    return k;
}

/* ---- geniş faz ---- */

static inline b32 aabb_overlap(const Body* a, const Body* b) {
    return a->aabb_min.x <= b->aabb_max.x && a->aabb_max.x >= b->aabb_min.x &&
           a->aabb_min.y <= b->aabb_max.y && a->aabb_max.y >= b->aabb_min.y &&
           a->aabb_min.z <= b->aabb_max.z && a->aabb_max.z >= b->aabb_min.z;
}

static inline u32 pair_key(u32 a, u32 b, u32 sub) { return ((a << 16) ^ b ^ (b << 7)) + sub * 0x9E3779B1u; }

static Manifold* prev_find(const PhysWorld* w, u32 a, u32 b, u32 sub) {
    u32 mask = w->prev_index_cap - 1;
    u32 h = (pair_key(a, b, sub) * 2654435761u) & mask;
    while (w->prev_index[h]) {
        Manifold* m = &w->prev[w->prev_index[h] - 1];
        if (m->a == a && m->b == b && m->sub == sub) return m;
        h = (h + 1) & mask;
    }
    return NULL;
}

static inline b32 awake_dynamic(const Body* b) { return b->inv_mass > 0.0f && !(b->flags & BODY_ASLEEP); }

static void wake_island(PhysWorld* w, u32 island) {
    for (u32 i = 0; i < w->count; i++) {
        Body* b = &w->bodies[i];
        if ((b->flags & BODY_ASLEEP) && b->island == island) {
            b->flags &= ~BODY_ASLEEP;
            b->sleep_time = 0.0f;
        }
    }
}

void phys_wake_all(PhysWorld* w) {
    for (u32 i = 0; i < w->count; i++) {
        w->bodies[i].flags &= ~BODY_ASLEEP;
        w->bodies[i].sleep_time = 0.0f;
    }
}

Vec3 phys_gravity_at(const PhysWorld* w, Vec3 pos) {
    if (w->planet_gm <= 0.0) return w->gravity;
    /* f64: Dünya'da R ≈ 6,4e6 m, birkaç metrelik h'nin etkisi ~1e-6 (f32'de birkaç ulp) */
    f64 r = w->planet_radius + (f64)pos.y;
    if (r < 1e-3) r = 1e-3;
    return v3(0.0f, (f32)(-w->planet_gm / (r * r)), 0.0f);
}

/* yeni manifold: malzeme, yerel noktalar, önceki adımdan warm start; cur_count'u artırır */
static void finalize_manifold(PhysWorld* w, Manifold* m) {
    u32 ia = m->a, ib = m->b;
    const Body* A = &w->bodies[ia];
    const Body* B = &w->bodies[ib];
    m->friction = m_sqrtf(A->friction * B->friction);
    m->restitution = m_maxf(A->restitution, B->restitution);
    for (u32 i = 0; i < m->count; i++) m->p[i].local_a = mat3_tmul_v3(&A->rot_m, v3_sub(m->p[i].pos, A->pos));
    /* warm start: önceki adımdaki aynı çiftin yakın noktası */
    const Manifold* pm = w->warm_start ? prev_find(w, ia, ib, m->sub) : NULL;
    if (pm) {
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            for (u32 k = 0; k < pm->count; k++) {
                Vec3 d = v3_sub(c->local_a, pm->p[k].local_a);
                if (v3_dot(d, d) < WARM_MATCH_DIST2) {
                    c->jn = pm->p[k].jn;
                    c->jt1 = pm->p[k].jt1;
                    c->jt2 = pm->p[k].jt2;
                    w->stats.warm_hits++;
                    break;
                }
            }
        }
    }
    w->stats.contacts += m->count;
    w->cur_count++;
}

/* Hıza bağlı spekülatif pay (küre-küre, küre-düzlem): bu adımda kapanabilecek boşluk = yaklaşma hızı · dt.
 * Sabit 2 cm'lik pay hızlı iki dinamik topu (CCD yalnızca statiklere bakar) çarpışmadan önce yakalamaz;
 * iç içe geçip ayrılırken restitüsyon yanlış hızla uygulanır (üst üste düşen toplar, D31). */
static f32 spec_margin(const PhysWorld* w, const Body* A, const Body* B) {
    if (!w->speculative || (A->shape != SHAPE_SPHERE && A->shape != SHAPE_PLANE)) return PHYS_MARGIN;
    if (B->shape != SHAPE_SPHERE && B->shape != SHAPE_PLANE) return PHYS_MARGIN;
    Vec3 n, dv;
    if (A->shape == SHAPE_PLANE || B->shape == SHAPE_PLANE) {
        const Body* s = A->shape == SHAPE_PLANE ? B : A;
        const Body* p = A->shape == SHAPE_PLANE ? A : B;
        n = p->plane_n;                                     /* düzlemden küreye */
        dv = v3_add(s->vel, v3_scale(phys_gravity_at(w, s->pos), w->step_dt));
    } else {
        Vec3 d = v3_sub(B->pos, A->pos);
        f32 l = v3_len(d);
        if (l < 1e-6f) return PHYS_MARGIN;
        n = v3_scale(d, 1.0f / l);                          /* A'dan B'ye */
        dv = v3_sub(B->vel, A->vel);
    }
    f32 closing = -v3_dot(dv, n);
    return closing > 0.0f ? PHYS_MARGIN + closing * w->step_dt : PHYS_MARGIN;
}

static void narrow(PhysWorld* w, u32 ia, u32 ib) {
    if (ia > ib) { u32 t = ia; ia = ib; ib = t; }
    if (joints_block_pair(w, ia, ib)) return;
    const Body* A = &w->bodies[ia];
    const Body* B = &w->bodies[ib];
    w->stats.pairs++;
    if (w->cur_count >= w->manifold_cap) return;
    if (A->shape == SHAPE_MESH || B->shape == SHAPE_MESH) {
        Manifold* out = &w->cur[w->cur_count];
        u32 room = w->manifold_cap - w->cur_count;
        u32 n = phys_collide_mesh(A, B, out, room > 16 ? 16 : room, w->mesh_edge_fix);
        for (u32 k = 0; k < n; k++) {
            Manifold* m = &w->cur[w->cur_count];
            m->a = ia;
            m->b = ib;
            finalize_manifold(w, m);
        }
        return;
    }
    Manifold* m = &w->cur[w->cur_count];
    m->a = ia;
    m->b = ib;
    if (!phys_collide_margin(A, B, m, spec_margin(w, A, B))) return;
    m->sub = 0;
    finalize_manifold(w, m);
}

static inline void add_pair(PhysWorld* w, u32 a, u32 b) {
    if (w->pair_count >= w->pair_cap) return;
    w->pairs[w->pair_count * 2] = a < b ? a : b;
    w->pairs[w->pair_count * 2 + 1] = a < b ? b : a;
    w->pair_count++;
}

/* Çiftler: düzlem olmayan (a, b), sıkı AABB'ler kesişir, en az biri uyanık hareketli. SAP ve BVH aynı
 * kümeyi üretir; ardından (a < b) anahtarıyla sıralanır → dar faz ve manifold sırası geniş fazdan bağımsız. */
static inline const TreeNode* proxy_node(const PhysWorld* w, u32 i) {
    const Body* b = &w->bodies[i];
    return &(b->inv_mass > 0.0f ? &w->dyn_tree : &w->static_tree)->nodes[b->proxy];
}

static void pset_add(PhysWorld* w, u32 i, u32 j) {
    u32 key = i < j ? (i << 16) | j : (j << 16) | i;
    u32 mask = w->pset_hash_cap - 1;
    u32 h = (key * 2654435761u) & mask;
    while (w->pset_hash[h]) {
        if (w->pset_hash[h] == key + 1) return;
        h = (h + 1) & mask;
    }
    if (w->pset_count >= w->pair_cap) return;
    w->pset_hash[h] = key + 1;
    w->pset[w->pset_count++] = key;
}

/* Kalıcı çift kümesi P = şişman AABB'leri kesişen tüm (dinamik, dinamik|statik) çiftler.
 * Değişmez: şişman kutu yalnızca yeniden eklemede değişir → yeni kesişimler yalnızca taşınan vekillerin
 * sorgusundan çıkar. Adımın çiftleri = P'de sıkı AABB'leri kesişen ve biri uyanık hareketli olanlar
 * (sıkı ⊆ şişman olduğundan SAP kümesiyle aynı). */
static void pairs_bvh(PhysWorld* w) {
    if (!w->pset_valid) {   /* ilk kullanım / SAP'tan geçiş: her şeyi taşınmış say */
        w->pset_count = 0;
        for (u32 i = 0; i < w->count; i++) {
            if (w->bodies[i].proxy != TREE_NULL) mark_moved(w, i);
        }
        w->pset_valid = TRUE;
    }
    phys_update_tree(w);
    /* 1) artık kesişmeyen çiftleri at */
    u32 kept = 0;
    for (u32 k = 0; k < w->pset_count; k++) {
        u32 key = w->pset[k];
        const TreeNode* a = proxy_node(w, key >> 16);
        const TreeNode* b = proxy_node(w, key & 0xFFFFu);
        if (a->mn.x <= b->mx.x && a->mx.x >= b->mn.x && a->mn.y <= b->mx.y && a->mx.y >= b->mn.y && a->mn.z <= b->mx.z && a->mx.z >= b->mn.z) {
            w->pset[kept++] = key;
        }
    }
    w->pset_count = kept;
    /* 2) taşınan vekillerin yeni kesişimleri */
    if (w->moved_count) {
        mem_zero(w->pset_hash, sizeof(u32) * w->pset_hash_cap);
        u32 mask = w->pset_hash_cap - 1;
        for (u32 k = 0; k < w->pset_count; k++) {
            u32 h = (w->pset[k] * 2654435761u) & mask;
            while (w->pset_hash[h]) h = (h + 1) & mask;
            w->pset_hash[h] = w->pset[k] + 1;
        }
        for (u32 m = 0; m < w->moved_count; m++) {
            u32 i = w->moved[m];
            w->moved_flag[i] = 0;
            const Body* a = &w->bodies[i];
            if (a->proxy == TREE_NULL) continue;
            const TreeNode* na = proxy_node(w, i);
            u32 n = tree_query(&w->dyn_tree, na->mn, na->mx, w->query_buf, w->query_cap);
            for (u32 q = 0; q < n; q++) {
                if (w->query_buf[q] != i) pset_add(w, i, w->query_buf[q]);
            }
            if (a->inv_mass > 0.0f) {
                n = tree_query(&w->static_tree, na->mn, na->mx, w->query_buf, w->query_cap);
                for (u32 q = 0; q < n; q++) pset_add(w, i, w->query_buf[q]);
            }
        }
        w->moved_count = 0;
    }
    w->stats.pset = w->pset_count;
    /* 3) adımın çiftleri */
    w->pair_count = 0;
    for (u32 k = 0; k < w->pset_count; k++) {
        u32 ia = w->pset[k] >> 16, ib = w->pset[k] & 0xFFFFu;
        const Body* a = &w->bodies[ia];
        const Body* b = &w->bodies[ib];
        if ((awake_dynamic(a) || awake_dynamic(b)) && aabb_overlap(a, b)) add_pair(w, ia, ib);
    }
}

static void pairs_sap(PhysWorld* w) {
    /* ekleme sıralaması (önceki sıra neredeyse sıralı); eşitlikte indeks → belirleyici */
    u32* o = w->order;
    for (u32 i = 1; i < w->order_count; i++) {
        u32 v = o[i];
        f32 key = w->bodies[v].aabb_min.x;
        u32 j = i;
        while (j > 0) {
            const Body* p = &w->bodies[o[j - 1]];
            if (p->aabb_min.x < key || (p->aabb_min.x == key && o[j - 1] < v)) break;
            o[j] = o[j - 1];
            j--;
        }
        o[j] = v;
    }
    /* 1) aday çiftler: en az biri uyanık hareketli (uyuyan-uyuyan ve uyuyan-statik gereksiz;
     *    uyuyanı uyandıracak çift zaten uyanık tarafı içerir) */
    w->pair_count = 0;
    for (u32 i = 0; i < w->order_count; i++) {
        const Body* a = &w->bodies[o[i]];
        b32 a_awake = awake_dynamic(a);
        for (u32 j = i + 1; j < w->order_count; j++) {
            const Body* b = &w->bodies[o[j]];
            if (b->aabb_min.x > a->aabb_max.x) break;
            if ((a_awake || awake_dynamic(b)) && aabb_overlap(a, b)) add_pair(w, o[i], o[j]);
        }
    }
}

static void broadphase(PhysWorld* w) {
    f64 t0 = sys_now_ms();
    if (w->broadphase == PHYS_BP_BVH) {
        pairs_bvh(w);
    } else {
        pairs_sap(w);
        w->pset_valid = FALSE;
    }
    for (u32 p = 0; p < w->count; p++) {
        if (w->bodies[p].shape != SHAPE_PLANE) continue;
        for (u32 i = 0; i < w->count; i++) {
            if (w->bodies[i].shape != SHAPE_PLANE && awake_dynamic(&w->bodies[i])) add_pair(w, i, p);
        }
    }
    /* kanonik sıra: (a < b) anahtarı a·2¹⁶ + b, 4 geçişli radix */
    for (u32 k = 0; k < w->pair_count; k++) w->pair_tmp[k] = (w->pairs[k * 2] << 16) | w->pairs[k * 2 + 1];
    sort_u32(w->pair_tmp, w->pair_count, w->pair_tmp + w->pair_count, 4);
    for (u32 k = 0; k < w->pair_count; k++) {
        w->pairs[k * 2] = w->pair_tmp[k] >> 16;
        w->pairs[k * 2 + 1] = w->pair_tmp[k] & 0xFFFFu;
    }
    w->stats.broad_ms = sys_now_ms() - t0;
    /* 2) uyanık bir gövdeye değen uyuyan adaları uyandır (yayılım için birkaç tur) */
    for (u32 round = 0; round < 8; round++) {
        b32 changed = FALSE;
        for (u32 k = 0; k < w->pair_count; k++) {
            Body* a = &w->bodies[w->pairs[k * 2]];
            Body* b = &w->bodies[w->pairs[k * 2 + 1]];
            if (awake_dynamic(a) && (b->flags & BODY_ASLEEP)) { wake_island(w, b->island); changed = TRUE; }
            else if (awake_dynamic(b) && (a->flags & BODY_ASLEEP)) { wake_island(w, a->island); changed = TRUE; }
        }
        /* eklemle bağlı gövdeler de birlikte uyanır */
        for (u32 k = 0; k < w->joint_count; k++) {
            const Joint* j = &w->joints[k];
            if (j->a == PHYS_NO_BODY || j->b == PHYS_NO_BODY) continue;
            Body* a = &w->bodies[j->a];
            Body* b = &w->bodies[j->b];
            if (awake_dynamic(a) && (b->flags & BODY_ASLEEP)) { wake_island(w, b->island); changed = TRUE; }
            else if (awake_dynamic(b) && (a->flags & BODY_ASLEEP)) { wake_island(w, a->island); changed = TRUE; }
        }
        if (!changed) break;
    }
    /* 3) dar faz: en az biri uyanık hareketli */
    for (u32 k = 0; k < w->pair_count; k++) {
        u32 ia = w->pairs[k * 2], ib = w->pairs[k * 2 + 1];
        if (awake_dynamic(&w->bodies[ia]) || awake_dynamic(&w->bodies[ib])) narrow(w, ia, ib);
    }
}

/* ---- adalar ve uyku ---- */

static u32 uf_find(u32* p, u32 x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static void uf_union(u32* p, u32 a, u32 b) {
    a = uf_find(p, a);
    b = uf_find(p, b);
    if (a == b) return;
    if (a < b) p[b] = a;   /* küçük indeks kök → belirleyici */
    else p[a] = b;
}

/* Temaslı uyanık gövdeler ada oluşturur (statik gövdeler bağlamaz). Bir adanın tüm gövdeleri
 * sleep_delay süresince eşiğin altındaysa ada birlikte uyur. */
static void update_sleep(PhysWorld* w, f32 dt) {
    u32* p = w->uf_parent;
    for (u32 i = 0; i < w->count; i++) p[i] = i;
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        const Manifold* m = &w->cur[mi];
        if (awake_dynamic(&w->bodies[m->a]) && awake_dynamic(&w->bodies[m->b])) uf_union(p, m->a, m->b);
    }
    for (u32 k = 0; k < w->joint_count; k++) {
        const Joint* j = &w->joints[k];
        if (j->a == PHYS_NO_BODY || j->b == PHYS_NO_BODY) continue;
        if (awake_dynamic(&w->bodies[j->a]) && awake_dynamic(&w->bodies[j->b])) uf_union(p, j->a, j->b);
    }
    f32 lin2 = w->sleep_lin * w->sleep_lin, ang2 = w->sleep_ang * w->sleep_ang;
    for (u32 i = 0; i < w->count; i++) {
        Body* b = &w->bodies[i];
        w->uf_min_time[i] = 3.4e38f;
        if (!awake_dynamic(b)) continue;
        if (b->flags & BODY_NO_SLEEP) b->sleep_time = 0.0f;
        else if (v3_dot(b->vel, b->vel) < lin2 && v3_dot(b->ang_vel, b->ang_vel) < ang2) b->sleep_time += dt;
        else b->sleep_time = 0.0f;
    }
    u32 islands = 0;
    for (u32 i = 0; i < w->count; i++) {
        if (!awake_dynamic(&w->bodies[i])) continue;
        u32 r = uf_find(p, i);
        if (r == i) islands++;
        w->uf_min_time[r] = m_minf(w->uf_min_time[r], w->bodies[i].sleep_time);
    }
    for (u32 i = 0; i < w->count; i++) {
        Body* b = &w->bodies[i];
        if (!awake_dynamic(b)) continue;
        u32 r = uf_find(p, i);
        if (w->uf_min_time[r] >= w->sleep_delay) {
            b->flags |= BODY_ASLEEP;
            b->island = w->island_seq + r;
            b->vel = v3(0, 0, 0);
            b->ang_vel = v3(0, 0, 0);
        }
    }
    w->island_seq += w->cap;
    w->stats.islands = islands;
}

/* ---- çözücü ---- */

static inline Vec3 point_vel(const Body* b, Vec3 r) { return v3_add(b->vel, v3_cross(b->ang_vel, r)); }

static inline f32 eff_mass(const Body* A, const Body* B, Vec3 ra, Vec3 rb, Vec3 n) {
    Vec3 ca = v3_cross(mat3_mul_v3(&A->inv_inertia_world, v3_cross(ra, n)), ra);
    Vec3 cb = v3_cross(mat3_mul_v3(&B->inv_inertia_world, v3_cross(rb, n)), rb);
    f32 k = A->inv_mass + B->inv_mass + v3_dot(n, v3_add(ca, cb));
    return k > 0.0f ? 1.0f / k : 0.0f;
}

static inline void apply_impulse(Body* A, Body* B, Vec3 ra, Vec3 rb, Vec3 P) {
    A->vel = v3_sub(A->vel, v3_scale(P, A->inv_mass));
    A->ang_vel = v3_sub(A->ang_vel, mat3_mul_v3(&A->inv_inertia_world, v3_cross(ra, P)));
    B->vel = v3_add(B->vel, v3_scale(P, B->inv_mass));
    B->ang_vel = v3_add(B->ang_vel, mat3_mul_v3(&B->inv_inertia_world, v3_cross(rb, P)));
}

/* İki geçiş: önce TÜM temasların kütle/bias'ı warm start ÖNCESİ hızlarla hesaplanır, sonra warm
 * start uygulanır. Tek geçişte, önceki manifoldların warm start itkileri (ör. yığın tabanındaki
 * büyük destek itkisi) sonraki temaslarda sahte bir yaklaşma hızı (vn < -1) gösterir ve
 * restitüsyon olmayan bir sıçramayı enerjiye çevirir. */
static void prestep(PhysWorld* w, f32 dt) {
    f32 inv_dt = 1.0f / dt;
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        Manifold* m = &w->cur[mi];
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            c->ra = v3_sub(c->pos, A->pos);
            c->rb = v3_sub(c->pos, B->pos);
            c->mass_n = eff_mass(A, B, c->ra, c->rb, m->normal);
            c->mass_t1 = eff_mass(A, B, c->ra, c->rb, m->t1);
            c->mass_t2 = eff_mass(A, B, c->ra, c->rb, m->t2);
            f32 vn = v3_dot(v3_sub(point_vel(B, c->rb), point_vel(A, c->ra)), m->normal);
            f32 bias;
            if (c->depth < 0.0f) bias = w->speculative ? c->depth * inv_dt : 0.0f;   /* spekülatif: boşluğu kapatacak kadar yaklaşabilir */
            else bias = w->baumgarte * inv_dt * m_maxf(c->depth - w->slop, 0.0f);
            if (vn < -RESTITUTION_THRESHOLD) bias = m_maxf(bias, -m->restitution * vn);
            c->bias = bias;
        }
    }
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        Manifold* m = &w->cur[mi];
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            Vec3 P = v3_add(v3_scale(m->normal, c->jn), v3_add(v3_scale(m->t1, c->jt1), v3_scale(m->t2, c->jt2)));
            apply_impulse(A, B, c->ra, c->rb, P);
        }
    }
}

static void solve(PhysWorld* w) {
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        Manifold* m = &w->cur[mi];
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            /* sürtünme: Coulomb diski |(jt1, jt2)| <= μ jn (eksen başına kenetleme kayma yönüne göre
             * sürtünmeyi √2'ye kadar şişirirdi) */
            f32 limit = m->friction * c->jn;
            Vec3 dv = v3_sub(point_vel(B, c->rb), point_vel(A, c->ra));
            f32 n1 = c->jt1 - c->mass_t1 * v3_dot(dv, m->t1);
            f32 n2 = c->jt2 - c->mass_t2 * v3_dot(dv, m->t2);
            f32 len2 = n1 * n1 + n2 * n2;
            if (len2 > limit * limit) {
                f32 s = limit / m_sqrtf(len2);
                n1 *= s;
                n2 *= s;
            }
            Vec3 Pt = v3_add(v3_scale(m->t1, n1 - c->jt1), v3_scale(m->t2, n2 - c->jt2));
            c->jt1 = n1;
            c->jt2 = n2;
            apply_impulse(A, B, c->ra, c->rb, Pt);
            /* normal */
            dv = v3_sub(point_vel(B, c->rb), point_vel(A, c->ra));
            f32 vn = v3_dot(dv, m->normal);
            f32 dn = c->mass_n * (c->bias - vn);
            f32 nn = m_maxf(c->jn + dn, 0.0f);
            dn = nn - c->jn;
            c->jn = nn;
            apply_impulse(A, B, c->ra, c->rb, v3_scale(m->normal, dn));
        }
    }
}

/* ---- soft step (Box2D v3 tarzı): alt adımlar, yumuşak kısıt, relax, restitüsyon ---- */

/* ω = 2π·hz; a1 = 2ζ + hω; a2 = hω·a1; a3 = 1/(1+a2) */
static Softness make_soft(f32 hertz, f32 zeta, f32 h) {
    Softness s;
    f32 omega = M_TAU_F * hertz;
    f32 a1 = 2.0f * zeta + h * omega;
    f32 a2 = h * omega * a1;
    f32 a3 = 1.0f / (1.0f + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

static Vec3 body_accel(const PhysWorld* w, const Body* b) {
    if (!awake_dynamic(b)) return v3(0, 0, 0);
    return v3_add(phys_gravity_at(w, b->pos), v3_scale(b->force, b->inv_mass));
}

static void soft_prepare(PhysWorld* w) {
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        Manifold* m = &w->cur[mi];
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            c->ra = v3_sub(c->pos, A->pos);
            c->rb = v3_sub(c->pos, B->pos);
            c->la = mat3_tmul_v3(&A->rot_m, c->ra);
            c->lb = mat3_tmul_v3(&B->rot_m, c->rb);
            c->s0 = -c->depth;
            c->mass_n = eff_mass(A, B, c->ra, c->rb, m->normal);
            c->mass_t1 = eff_mass(A, B, c->ra, c->rb, m->t1);
            c->mass_t2 = eff_mass(A, B, c->ra, c->rb, m->t2);
            c->rel_vel = v3_dot(v3_sub(point_vel(B, c->rb), point_vel(A, c->ra)), m->normal);
            c->max_jn = 0.0f;
            /* spekülatif boşluk: restitüsyon hedefi çarpma anındaki hız. Sabit dış ivmeyle (yerçekimi)
             * yaklaşan çift boşluğu kapatana dek hızlanır: v² + 2·a·s. Bu olmadan zemine düşen top adım
             * başındaki (g·dt kadar küçük) hızla seker → her sıçramada ~%6 yükseklik kaybı (D31). */
            if (c->s0 > 0.0f && c->rel_vel <= 0.0f) {
                f32 an = -v3_dot(v3_sub(body_accel(w, B), body_accel(w, A)), m->normal);
                if (an > 0.0f) c->rel_vel = -m_sqrtf(c->rel_vel * c->rel_vel + 2.0f * an * c->s0);
            }
        }
    }
}

/* ---- kısıt grafiği renklendirmesi (Box2D v3) ----
 * Açgözlü: her manifold, iki dinamik gövdesinin de henüz kullanmadığı en küçük renge. Statik gövdeler
 * renk tüketmez (yazılmazlar; skalerde de itki × 0 bit-bit etkisiz). Sığmayan → taşma (en sonda, skaler). */
static void color_manifolds(PhysWorld* w) {
    u32 words = w->color_words;
    mem_zero(w->color_bits, sizeof(u64) * words * PHYS_COLORS);
    u32 count[PHYS_COLORS + 1] = { 0 };
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        const Manifold* m = &w->cur[mi];
        b32 da = w->bodies[m->a].inv_mass > 0.0f, db = w->bodies[m->b].inv_mass > 0.0f;
        u32 c = PHYS_COLORS;
        for (u32 k = 0; k < PHYS_COLORS; k++) {
            const u64* bits = &w->color_bits[k * words];
            if (da && (bits[m->a >> 6] >> (m->a & 63) & 1u)) continue;
            if (db && (bits[m->b >> 6] >> (m->b & 63) & 1u)) continue;
            c = k;
            break;
        }
        if (c < PHYS_COLORS) {
            u64* bits = &w->color_bits[c * words];
            if (da) bits[m->a >> 6] |= (u64)1 << (m->a & 63);
            if (db) bits[m->b >> 6] |= (u64)1 << (m->b & 63);
        }
        w->manifold_color[mi] = (u8)c;
        count[c]++;
    }
    u32 at = 0;
    w->stats.colors = 0;
    for (u32 c = 0; c <= PHYS_COLORS; c++) {
        w->color_start[c] = at;
        at += count[c];
        if (c < PHYS_COLORS && count[c]) w->stats.colors = c + 1;
    }
    w->color_start[PHYS_COLORS + 1] = at;
    w->stats.overflow = count[PHYS_COLORS];
    u32 fill[PHYS_COLORS + 1];
    for (u32 c = 0; c <= PHYS_COLORS; c++) fill[c] = w->color_start[c];
    for (u32 mi = 0; mi < w->cur_count; mi++) w->color_order[fill[w->manifold_color[mi]]++] = mi;
}

/* çözüm sırasındaki k. manifold */
static inline u32 solve_index(const PhysWorld* w, u32 k) { return w->graph_color ? w->color_order[k] : k; }

/* Aşağıdaki iki fonksiyonun işlem sırası solver_simd_wasm.c'de birebir tekrarlanır (bit-bit eşitlik). */
static void warm_manifold(PhysWorld* w, Manifold* m) {
    Body* A = &w->bodies[m->a];
    Body* B = &w->bodies[m->b];
    for (u32 i = 0; i < m->count; i++) {
        ContactPoint* c = &m->p[i];
        Vec3 P = v3_add(v3_scale(m->normal, c->jn), v3_add(v3_scale(m->t1, c->jt1), v3_scale(m->t2, c->jt2)));
        apply_impulse(A, B, c->ra, c->rb, P);
    }
}

static void solve_manifold(PhysWorld* w, Manifold* m, f32 inv_h, Softness soft, b32 use_bias) {
    {
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            /* güncel ayrılma: çapalar gövdelerin güncel dönüşüyle */
            Vec3 pa = v3_add(A->pos, mat3_mul_v3(&A->rot_m, c->la));
            Vec3 pb = v3_add(B->pos, mat3_mul_v3(&B->rot_m, c->lb));
            f32 s = v3_dot(v3_sub(pb, pa), m->normal) + c->s0;
            f32 bias = 0.0f, mass_scale = 1.0f, impulse_scale = 0.0f;
            if (s > 0.0f) {
                bias = s * inv_h;   /* spekülatif */
            } else if (use_bias) {
                bias = m_maxf(soft.bias_rate * s, -SOFT_MAX_BIAS_VEL);
                mass_scale = soft.mass_scale;
                impulse_scale = soft.impulse_scale;
            }
            Vec3 dv = v3_sub(point_vel(B, c->rb), point_vel(A, c->ra));
            f32 vn = v3_dot(dv, m->normal);
            f32 imp = -c->mass_n * mass_scale * (vn + bias) - impulse_scale * c->jn;
            f32 nn = m_maxf(c->jn + imp, 0.0f);
            imp = nn - c->jn;
            c->jn = nn;
            c->max_jn = m_maxf(c->max_jn, nn);
            apply_impulse(A, B, c->ra, c->rb, v3_scale(m->normal, imp));

            f32 limit = m->friction * c->jn;
            dv = v3_sub(point_vel(B, c->rb), point_vel(A, c->ra));
            f32 n1 = c->jt1 - c->mass_t1 * v3_dot(dv, m->t1);
            f32 n2 = c->jt2 - c->mass_t2 * v3_dot(dv, m->t2);
            f32 len2 = n1 * n1 + n2 * n2;
            if (len2 > limit * limit) {
                f32 k = limit / m_sqrtf(len2);
                n1 *= k;
                n2 *= k;
            }
            Vec3 Pt = v3_add(v3_scale(m->t1, n1 - c->jt1), v3_scale(m->t2, n2 - c->jt2));
            c->jt1 = n1;
            c->jt2 = n2;
            apply_impulse(A, B, c->ra, c->rb, Pt);
        }
    }
}

static b32 use_simd(const PhysWorld* w) { return PHYS_HAVE_SIMD && w->graph_color && w->simd; }

static void soft_warm_start(PhysWorld* w) {
    u32 k = 0;
#if PHYS_HAVE_SIMD
    if (use_simd(w)) {
        solver_simd_warm_start(w);
        k = w->color_start[PHYS_COLORS];   /* taşma rengi skaler */
    }
#endif
    for (; k < w->cur_count; k++) warm_manifold(w, &w->cur[solve_index(w, k)]);
}

static void soft_solve(PhysWorld* w, f32 h, Softness soft, b32 use_bias) {
    f32 inv_h = 1.0f / h;
    u32 k = 0;
#if PHYS_HAVE_SIMD
    if (use_simd(w)) {
        solver_simd_solve(w, inv_h, soft, use_bias);
        k = w->color_start[PHYS_COLORS];
    }
#endif
    for (; k < w->cur_count; k++) solve_manifold(w, &w->cur[solve_index(w, k)], inv_h, soft, use_bias);
}

static void soft_restitution(PhysWorld* w) {
    for (u32 k = 0; k < w->cur_count; k++) {
        Manifold* m = &w->cur[solve_index(w, k)];
        if (m->restitution == 0.0f) continue;
        Body* A = &w->bodies[m->a];
        Body* B = &w->bodies[m->b];
        for (u32 i = 0; i < m->count; i++) {
            ContactPoint* c = &m->p[i];
            if (c->rel_vel > -RESTITUTION_THRESHOLD || c->max_jn == 0.0f) continue;
            f32 vn = v3_dot(v3_sub(point_vel(B, c->rb), point_vel(A, c->ra)), m->normal);
            f32 imp = -c->mass_n * (vn + m->restitution * c->rel_vel);
            f32 nn = m_maxf(c->jn + imp, 0.0f);
            imp = nn - c->jn;
            c->jn = nn;
            apply_impulse(A, B, c->ra, c->rb, v3_scale(m->normal, imp));
        }
    }
}

static void integrate_velocities(PhysWorld* w, f32 dt) {
    for (u32 i = 0; i < w->count; i++) {
        Body* b = &w->bodies[i];
        if (!awake_dynamic(b)) continue;
        b->vel = v3_add(b->vel, v3_scale(v3_add(phys_gravity_at(w, b->pos), v3_scale(b->force, b->inv_mass)), dt));
        b->ang_vel = v3_add(b->ang_vel, v3_scale(mat3_mul_v3(&b->inv_inertia_world, b->torque), dt));
    }
}

static void integrate_positions(PhysWorld* w, f32 dt) {
    for (u32 i = 0; i < w->count; i++) {
        Body* b = &w->bodies[i];
        if (!awake_dynamic(b)) continue;
        b->pos = v3_add(b->pos, v3_scale(b->vel, dt));
        b->rot = quat_integrate(b->rot, b->ang_vel, dt);
        phys_body_update_derived(b);
    }
}

static void soft_step(PhysWorld* w, f32 dt) {
    u32 n = w->substeps ? w->substeps : 1;
    f32 h = dt / (f32)n;
    /* Box2D: temas sertliği alt adım frekansının çeyreğini aşmasın */
    f32 hz = m_minf(w->contact_hertz, 0.25f / h);
    Softness soft = make_soft(hz, w->contact_damping, h);
    soft_prepare(w);
    joints_prepare(w, h);
    if (!w->warm_start) {   /* hazırlık itkileri kullanmaz → döngüden önce sıfırlamak sonucu değiştirmez */
        for (u32 mi = 0; mi < w->cur_count; mi++) {
            for (u32 i = 0; i < w->cur[mi].count; i++) w->cur[mi].p[i].jn = w->cur[mi].p[i].jt1 = w->cur[mi].p[i].jt2 = 0.0f;
        }
    }
    if (w->graph_color) color_manifolds(w);
#if PHYS_HAVE_SIMD
    if (use_simd(w)) solver_simd_prepare(w);
#endif
    for (u32 sub = 0; sub < n; sub++) {
        integrate_velocities(w, h);
        if (w->warm_start) {
            joints_warm_start(w);
            soft_warm_start(w);
        }
        joints_solve(w, h, TRUE, TRUE);
        soft_solve(w, h, soft, TRUE);
        integrate_positions(w, h);
        joints_solve(w, h, FALSE, TRUE);
        soft_solve(w, h, soft, FALSE);
    }
#if PHYS_HAVE_SIMD
    if (use_simd(w)) solver_simd_store(w);
#endif
    soft_restitution(w);
}

/* ---- CCD: hızlı gövdeyi adım sonunda ilk çarpma anına geri çek (Box2D v3 yaklaşımı) ----
 * Adım başı (pos0, rot0) → adım sonu poz arası doğrusal konum + sabit eksenli dönüş süpürülür.
 * Muhafazakâr ilerleme: t += (d − hedef) / (Δp·n + θ·max_extent); d GJK çekirdek mesafesi.
 * Gövde TOI pozuna konur, hız korunur; sonraki adımın spekülatif teması (boşluk < PHYS_MARGIN) durdurur.
 * Adım başında zaten değiyorsa (t = 0) merkezdeki küçük küreyle (0,25·min_extent) yeniden süpürülür:
 * zeminde hızla kayan gövde takılmaz ama merkezi engelden geçemez. */
#define CCD_TARGET (0.25f * PHYS_MARGIN)
#define CCD_TOL    (0.25f * CCD_TARGET)
#define CCD_MAX_TRIS 256

typedef struct CcdSweep {
    Body body;         /* süpürülen gövdenin kopyası (poz t'ye göre güncellenir) */
    Vec3 p0, dp;
    Quat q0;
    Vec3 axis;
    f32  angle;
    b32  small;        /* merkez küresi geri dönüşü */
} CcdSweep;

typedef struct CcdObstacle {
    b32  is_plane;
    Vec3 pn;
    f32  pd;
    b32  is_tri;       /* mesh üçgeni: tp köşe, tn yüz normali */
    Vec3 tp, tn;
    Core core;
} CcdObstacle;

static void sweep_core(CcdSweep* s, f32 t, Core* c) {
    Body* b = &s->body;
    b->pos = v3_add(s->p0, v3_scale(s->dp, t));
    b->rot = s->angle > 0.0f ? quat_normalize(quat_mul(quat_from_axis_angle(s->axis, s->angle * t), s->q0)) : s->q0;
    b->rot_m = mat3_from_quat(b->rot);
    if (s->small) {
        c->kind = CORE_POINT;
        c->p0 = c->p1 = b->pos;
        c->radius = 0.25f * b->min_extent;
        c->hull = NULL;
        return;
    }
    phys_body_core(b, c);
}

/* gövdeden engele mesafe ve normal */
static f32 ccd_distance(const Core* c, const CcdObstacle* o, Vec3* n, b32* overlap) {
    *overlap = FALSE;
    if (o->is_plane) {
        *n = v3_scale(o->pn, -1.0f);
        Vec3 sp = core_support(c, *n);
        return v3_dot(o->pn, sp) - o->pd - c->radius;
    }
    GjkResult g;
    gjk_distance(c, &o->core, &g);
    if (g.overlap || g.dist <= 1e-6f) { *overlap = TRUE; return 0.0f; }
    *n = v3_scale(v3_sub(g.pb, g.pa), 1.0f / g.dist);
    return g.dist - c->radius - o->core.radius;
}

/* TOI ∈ [0, 1]; 1 = çarpma yok */
static f32 ccd_toi(CcdSweep* s, const CcdObstacle* o) {
    f32 rot_bound = s->small ? 0.0f : s->angle * s->body.max_extent;
    f32 t = 0.0f;
    for (u32 it = 0; it < 32; it++) {
        Core c;
        sweep_core(s, t, &c);
        Vec3 n;
        b32 overlap;
        f32 d = ccd_distance(&c, o, &n, &overlap);
        if (overlap || d < CCD_TARGET + CCD_TOL) return t;
        f32 bound = v3_dot(s->dp, n) + rot_bound;
        if (bound <= 1e-9f) return 1.0f;
        t += (d - CCD_TARGET) / bound;
        if (t >= 1.0f) return 1.0f;
    }
    return t;
}

/* Adım sonu pozunda engelle ilişki sığ mı (o zaman normal temas çözer, geri çekmeye gerek yok)?
 * Sığ: boşluk < PHYS_MARGIN ya da girişim < 0,25·min_extent. Bu olmadan eş düzlemli komşu üçgenler /
 * karolar üzerinde hızla kayan gövde her kenarda TOI'ye takılır. Üçgende merkez düzlemin öbür
 * yanına geçtiyse her zaman derin (tünel). */
static b32 ccd_end_shallow(CcdSweep* s, const CcdObstacle* o) {
    Core c;
    s->small = FALSE;
    sweep_core(s, 1.0f, &c);
    f32 lim = 0.25f * s->body.min_extent;
    if (o->is_plane) return v3_dot(o->pn, core_support(&c, v3_scale(o->pn, -1.0f))) - o->pd - c.radius > -lim;
    if (o->is_tri) {
        f32 s1 = v3_dot(v3_sub(s->p0, o->tp), o->tn), s2 = v3_dot(v3_sub(v3_add(s->p0, s->dp), o->tp), o->tn);
        if ((s1 > 0.0f) != (s2 > 0.0f)) return FALSE;
    }
    GjkResult g;
    gjk_distance(&c, &o->core, &g);
    f32 r = c.radius + o->core.radius;
    if (!g.overlap && g.dist > 1e-6f) return g.dist - r < PHYS_MARGIN;
    if (c.kind != CORE_POLY && o->core.kind != CORE_POLY) return r - g.dist < lim;
    EpaResult e;
    epa_penetration(&c, &o->core, &g, &e);
    return e.ok && e.depth + r < lim;
}

/* engel için TOI, t = 0'da küçük küre geri dönüşüyle; 0 = yok say (zaten içinde) */
static f32 ccd_obstacle(CcdSweep* s, const CcdObstacle* o) {
    s->small = FALSE;
    f32 t = ccd_toi(s, o);
    if (t > 0.0f) return t;
    s->small = TRUE;
    t = ccd_toi(s, o);
    s->small = FALSE;
    return t > 0.0f ? t : 1.0f;
}

static void body_core_obstacle(const Body* b, CcdObstacle* o) {
    o->is_tri = FALSE;
    o->is_plane = b->shape == SHAPE_PLANE;
    if (o->is_plane) {
        o->pn = b->plane_n;
        o->pd = b->plane_d;
    } else {
        phys_body_core(b, &o->core);
    }
}

static void ccd_body(PhysWorld* w, u32 i) {
    Body* b = &w->bodies[i];
    CcdSweep s;
    s.body = *b;
    s.p0 = b->pos0;
    s.dp = v3_sub(b->pos, b->pos0);
    s.q0 = b->rot0;
    s.small = FALSE;
    /* dönüş farkı dq = q1 q0*, kısa yol */
    Quat c0 = { -b->rot0.x, -b->rot0.y, -b->rot0.z, b->rot0.w };
    Quat dq = quat_mul(b->rot, c0);
    if (dq.w < 0.0f) { dq.x = -dq.x; dq.y = -dq.y; dq.z = -dq.z; dq.w = -dq.w; }
    f32 sl = m_sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
    s.angle = sl > 1e-7f ? 2.0f * m_atan2f(sl, dq.w) : 0.0f;
    s.axis = sl > 1e-7f ? v3(dq.x / sl, dq.y / sl, dq.z / sl) : v3(0, 1, 0);
    /* süpürme AABB'si */
    f32 ext = b->max_extent + PHYS_MARGIN;
    Vec3 mn = v3(m_minf(b->pos0.x, b->pos.x) - ext, m_minf(b->pos0.y, b->pos.y) - ext, m_minf(b->pos0.z, b->pos.z) - ext);
    Vec3 mx = v3(m_maxf(b->pos0.x, b->pos.x) + ext, m_maxf(b->pos0.y, b->pos.y) + ext, m_maxf(b->pos0.z, b->pos.z) + ext);
    b32 bullet = (b->flags & BODY_BULLET) != 0;
    f32 best = 1.0f;
    /* adaylar: düzlemler + süpürme kutusuyla kesişenler (sonuç = en küçük TOI, sıradan bağımsız) */
    u32* cand = w->query_buf;
    u32 nc = phys_query_aabb(w, mn, mx, cand, w->query_cap);
    for (u32 p = 0; p < w->plane_count && nc < w->query_cap; p++) cand[nc++] = w->plane_ids[p];
    for (u32 ci = 0; ci < nc; ci++) {
        u32 k = cand[ci];
        if (k == i) continue;
        const Body* o = &w->bodies[k];
        b32 is_static = o->inv_mass == 0.0f;
        if (!is_static && !(bullet && !(o->flags & BODY_BULLET))) continue;
        if (o->shape != SHAPE_PLANE) {
            if (o->aabb_min.x > mx.x || o->aabb_max.x < mn.x || o->aabb_min.y > mx.y || o->aabb_max.y < mn.y ||
                o->aabb_min.z > mx.z || o->aabb_max.z < mn.z) continue;
        }
        if (joints_block_pair(w, i < k ? i : k, i < k ? k : i)) continue;
        if (o->shape == SHAPE_MESH) {
            Vec3 lmn = v3(3.4e38f, 3.4e38f, 3.4e38f), lmx = v3(-3.4e38f, -3.4e38f, -3.4e38f);
            for (u32 c = 0; c < 8; c++) {
                Vec3 p = v3((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z);
                Vec3 l = mat3_tmul_v3(&o->rot_m, v3_sub(p, o->pos));
                lmn = v3(m_minf(lmn.x, l.x), m_minf(lmn.y, l.y), m_minf(lmn.z, l.z));
                lmx = v3(m_maxf(lmx.x, l.x), m_maxf(lmx.y, l.y), m_maxf(lmx.z, l.z));
            }
            u32 tris[CCD_MAX_TRIS];
            u32 nt = trimesh_query_aabb(o->mesh, lmn, lmx, tris, CCD_MAX_TRIS);
            for (u32 t = 0; t < nt; t++) {
                Vec3 p0, p1, p2;
                trimesh_tri(o->mesh, tris[t], &p0, &p1, &p2);
                p0 = v3_add(o->pos, mat3_mul_v3(&o->rot_m, p0));
                p1 = v3_add(o->pos, mat3_mul_v3(&o->rot_m, p1));
                p2 = v3_add(o->pos, mat3_mul_v3(&o->rot_m, p2));
                Vec3 tn = v3_cross(v3_sub(p1, p0), v3_sub(p2, p0));
                if (v3_dot(tn, tn) < 1e-14f) continue;
                CcdObstacle ob;
                ob.is_plane = FALSE;
                ob.is_tri = TRUE;
                ob.tp = p0;
                ob.tn = v3_norm(tn);
                core_triangle(&ob.core, p0, p1, p2);
                f32 toi = ccd_obstacle(&s, &ob);
                if (toi < best && !ccd_end_shallow(&s, &ob)) best = toi;
            }
            continue;
        }
        CcdObstacle ob;
        body_core_obstacle(o, &ob);
        f32 toi = ccd_obstacle(&s, &ob);
        if (toi < best && !ccd_end_shallow(&s, &ob)) best = toi;
    }
    if (best < 1.0f) {
        Core c;
        sweep_core(&s, best, &c);
        b->pos = s.body.pos;
        b->rot = s.body.rot;
        phys_body_update_derived(b);
        w->stats.ccd_hits++;
    }
}

static void continuous(PhysWorld* w, f32 dt) {
    /* önce normal hızlı gövdeler (statiklere karşı), sonra mermiler (son pozlara karşı) */
    for (u32 pass = 0; pass < 2; pass++) {
        if (w->broadphase == PHYS_BP_BVH) phys_update_tree(w);   /* entegrasyon / 1. geçiş sonrası AABB'ler */
        for (u32 i = 0; i < w->count; i++) {
            Body* b = &w->bodies[i];
            if (!awake_dynamic(b) || b->min_extent <= 0.0f) continue;
            if (((b->flags & BODY_BULLET) != 0) != (pass == 1)) continue;
            f32 disp = v3_len(v3_sub(b->pos, b->pos0)) + v3_len(b->ang_vel) * dt * b->max_extent;
            if (disp <= 0.5f * b->min_extent) continue;
            w->stats.ccd_bodies++;
            ccd_body(w, i);
        }
    }
}

void phys_step(PhysWorld* w, f32 dt) {
    f64 t0 = sys_now_ms();
    w->stats.pairs = w->stats.contacts = w->stats.warm_hits = 0;
    w->stats.ccd_bodies = w->stats.ccd_hits = 0;
    w->stats.tree_moves = 0;
    for (u32 i = 0; i < w->count; i++) {
        w->bodies[i].pos0 = w->bodies[i].pos;
        w->bodies[i].rot0 = w->bodies[i].rot;
    }

    if (w->solver == PHYS_SOLVER_SI) integrate_velocities(w, dt);
    w->step_dt = dt;
    if (w->speculative) {
        /* uyanık kürelerin AABB'si adım içi yol kadar uzar → hızlı yaklaşan çift geniş fazdan geçer
         * (spec_margin); entegrasyondaki phys_body_update_derived sıkı kutuya geri döndürür */
        for (u32 i = 0; i < w->count; i++) {
            Body* b = &w->bodies[i];
            if (b->shape != SHAPE_SPHERE || !awake_dynamic(b)) continue;
            Vec3 d = v3_scale(v3_add(b->vel, v3_scale(phys_gravity_at(w, b->pos), dt)), dt);
            b->aabb_min = v3_add(b->aabb_min, v3(m_minf(d.x, 0.0f), m_minf(d.y, 0.0f), m_minf(d.z, 0.0f)));
            b->aabb_max = v3_add(b->aabb_max, v3(m_maxf(d.x, 0.0f), m_maxf(d.y, 0.0f), m_maxf(d.z, 0.0f)));
        }
    }

    /* önceki manifoldları ara tabloya koy */
    Manifold* t = w->prev;
    w->prev = w->cur;
    w->cur = t;
    w->prev_count = w->cur_count;
    w->cur_count = 0;
    mem_zero(w->prev_index, sizeof(u32) * w->prev_index_cap);
    u32 mask = w->prev_index_cap - 1;
    for (u32 i = 0; i < w->prev_count; i++) {
        u32 h = (pair_key(w->prev[i].a, w->prev[i].b, w->prev[i].sub) * 2654435761u) & mask;
        while (w->prev_index[h]) h = (h + 1) & mask;
        w->prev_index[h] = i + 1;
    }

    broadphase(w);
    f64 t1 = sys_now_ms();
    if (w->solver == PHYS_SOLVER_SOFT) {
        soft_step(w, dt);
    } else {
        prestep(w, dt);
        joints_prepare(w, dt);
        if (w->warm_start) joints_warm_start(w);
        for (u32 it = 0; it < w->iterations; it++) {
            joints_solve(w, dt, TRUE, FALSE);
            solve(w);
        }
        integrate_positions(w, dt);
    }
    if (w->continuous) continuous(w, dt);
    for (u32 i = 0; i < w->count; i++) {
        w->bodies[i].force = v3(0, 0, 0);
        w->bodies[i].torque = v3(0, 0, 0);
    }
    if (w->allow_sleep) update_sleep(w, dt);
    /* adım sonu: ağaç güncel AABB'lerle (adımlar arası raycast / yumuşak cisim sorguları için) */
    if (w->broadphase == PHYS_BP_BVH) phys_update_tree(w);
    w->max_disp = 0.0f;
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        if (b->inv_mass > 0.0f) w->max_disp = m_maxf(w->max_disp, v3_len(v3_sub(b->pos, b->pos0)));
    }
    f64 t2 = sys_now_ms();
    w->stats.bodies = w->count;
    w->stats.manifolds = w->cur_count;
    w->stats.awake = 0;
    for (u32 i = 0; i < w->count; i++) w->stats.awake += awake_dynamic(&w->bodies[i]);
    w->stats.collide_ms = t1 - t0;
    w->stats.solve_ms = t2 - t1;
    w->stats.step_ms = t2 - t0;
}

f64 phys_kinetic_energy(const PhysWorld* w) {
    f64 e = 0.0;
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        if (b->inv_mass == 0.0f) continue;
        f64 m = 1.0 / b->inv_mass;
        e += 0.5 * m * (f64)v3_dot(b->vel, b->vel);
        /* açısal: 0.5 wᵀ I w, I = (I⁻¹)⁻¹ asal eksenlerde köşegen */
        Mat3 f = inertia_frame(b);
        Vec3 wl = mat3_tmul_v3(&f, b->ang_vel);
        e += 0.5 * ((f64)wl.x * wl.x / b->inv_inertia_local.x + (f64)wl.y * wl.y / b->inv_inertia_local.y +
                    (f64)wl.z * wl.z / b->inv_inertia_local.z);
    }
    return e;
}

u32 phys_state_hash(const PhysWorld* w) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        const f32 v[13] = { b->pos.x, b->pos.y, b->pos.z, b->rot.x, b->rot.y, b->rot.z, b->rot.w,
                            b->vel.x, b->vel.y, b->vel.z, b->ang_vel.x, b->ang_vel.y, b->ang_vel.z };
        for (u32 k = 0; k < 13; k++) h = (h ^ m_f32_bits(v[k])) * 16777619u;
    }
    return h;
}
