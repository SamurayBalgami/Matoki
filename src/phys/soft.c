/* phys/soft.c — XPBD yumuşak cisim çözücüsü, kumaş/jöle kurucuları, rijit dünyayla iki yönlü çarpışma.
 * Alt adım: rüzgâr + yerçekimi → tahmin → mesafe ve hacim kısıtları → kumaş öz-çarpışması → yumuşak-yumuşak
 * parçacık itmesi → yakalanan parçacıklar → rijit gövdelerle çarpışma (gövde yolu alt adımlara bölünür, temas
 * itkileri yolu da yavaşlatır) → rijit-üçgen teması → hız = Δx / h, sönüm.
 * Rijit yüzey uzaklığı şekle göre: düzlem, küre, kapsül, kutu analitik; hull yüz düzlemleri (dışarıda GJK);
 * mesh BVH sorgusu + en yakın üçgen noktası (alt adımda düzlem geçildiyse önceki tarafa itilir). */
#include "phys/soft.h"
#include "phys/gjk.h"
#include "phys/shapes.h"
#include "core/mem.h"
#include "core/sys.h"

#define SOFT_MAX_CAND   64    /* yumuşak gövde başına aday rijit gövde */
#define SOFT_SWEPT_MIN  5.0f  /* m/s: üzerinde gövde "mermi" sayılır: iz silindiri + yakalama (kumaş ağ gibi tutar) */
#define SOFT_RELEASE    1.0f  /* m/s: yakalanan parçacıklar bu hızın altında (ya da gövde geri dönünce) bırakılır */
#define SOFT_MAX_BODIES 16    /* adımlanan yumuşak gövde (fazlası yok sayılır) */

/* ---- kurucular ---- */

static SoftBody* soft_alloc(Arena* a, u32 n, u32 max_dist, u32 max_tets, u32 max_tris) {
    SoftBody* b = ARENA_NEW(a, SoftBody);
    b->count = n;
    b->x = ARENA_ARRAY(a, Vec3, n);
    b->prev = ARENA_ARRAY(a, Vec3, n);
    b->v = ARENA_ARRAY(a, Vec3, n);
    b->w = ARENA_ARRAY(a, f32, n);
    b->dist = ARENA_ARRAY(a, SoftDist, max_dist ? max_dist : 1);
    b->tets = ARENA_ARRAY(a, SoftTet, max_tets ? max_tets : 1);
    b->tris = ARENA_ARRAY(a, u32, max_tris * 3 + 3);
    b->radius = 0.05f;
    b->friction = 0.5f;
    b->damping = 0.0f;
    return b;
}

static void add_dist(SoftBody* b, u32 i, u32 j, f32 compliance, u32 kind) {
    SoftDist* d = &b->dist[b->dist_count++];
    d->a = i;
    d->b = j;
    d->rest = v3_len(v3_sub(b->x[i], b->x[j]));
    d->compliance = compliance;
    d->kind = kind;
}

SoftBody* soft_cloth(Arena* a, Vec3 origin, Vec3 du, Vec3 dv, u32 nu, u32 nv, f32 mass,
                     f32 stretch_compliance, f32 shear_compliance, f32 bend_compliance) {
    if (nu < 2 || nv < 2) sys_panic("soft_cloth: en az 2×2");
    u32 n = nu * nv;
    u32 nd = (nu - 1) * nv + nu * (nv - 1) + 2 * (nu - 1) * (nv - 1) + (nu > 2 ? (nu - 2) * nv : 0) + (nv > 2 ? nu * (nv - 2) : 0);
    SoftBody* b = soft_alloc(a, n, nd, 0, 2 * (nu - 1) * (nv - 1));
    for (u32 j = 0; j < nv; j++) {
        for (u32 i = 0; i < nu; i++) {
            u32 k = j * nu + i;
            b->x[k] = v3_add(origin, v3_add(v3_scale(du, (f32)i / (f32)(nu - 1)), v3_scale(dv, (f32)j / (f32)(nv - 1))));
            b->prev[k] = b->x[k];
            b->w[k] = (f32)n / mass;
        }
    }
    b->mass = mass;
    for (u32 j = 0; j < nv; j++) {
        for (u32 i = 0; i < nu; i++) {
            u32 k = j * nu + i;
            if (i + 1 < nu) add_dist(b, k, k + 1, stretch_compliance, SOFT_STRETCH);
            if (j + 1 < nv) add_dist(b, k, k + nu, stretch_compliance, SOFT_STRETCH);
            if (i + 1 < nu && j + 1 < nv) {
                add_dist(b, k, k + nu + 1, shear_compliance, SOFT_SHEAR);
                add_dist(b, k + 1, k + nu, shear_compliance, SOFT_SHEAR);
            }
            if (i + 2 < nu) add_dist(b, k, k + 2, bend_compliance, SOFT_BEND);
            if (j + 2 < nv) add_dist(b, k, k + 2 * nu, bend_compliance, SOFT_BEND);
        }
    }
    for (u32 j = 0; j + 1 < nv; j++) {
        for (u32 i = 0; i + 1 < nu; i++) {
            u32 p00 = j * nu + i, p10 = p00 + 1, p01 = p00 + nu, p11 = p01 + 1;
            u32* t = &b->tris[b->tri_count * 3];
            t[0] = p00; t[1] = p10; t[2] = p11;
            t[3] = p00; t[4] = p11; t[5] = p01;
            b->tri_count += 2;
        }
    }
    /* yarıçap > 0,354·aralık: başka katmanın parçacığı dört komşunun arasından geçemez */
    b->radius = 0.4f * m_minf(v3_len(du) / (f32)(nu - 1), v3_len(dv) / (f32)(nv - 1));
    b->drag = 0.7f;   /* ½ · 1,2 kg/m³ · C_d ≈ 1,2 */
    b->damping = 0.1f;
    b->x0 = ARENA_ARRAY(a, Vec3, n);
    mem_copy(b->x0, b->x, sizeof(Vec3) * n);
    b->hash_cap = 1;
    while (b->hash_cap < 2 * n) b->hash_cap <<= 1;
    b->hash_start = ARENA_ARRAY(a, u32, b->hash_cap + 1);
    b->hash_items = ARENA_ARRAY(a, u32, n);
    b->self_collide = TRUE;
    b->tri_collide = TRUE;
    b->catch_fast = TRUE;
    return b;
}

static f32 tet_volume(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3) {
    return v3_dot(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)), v3_sub(p3, p0)) / 6.0f;
}

SoftBody* soft_jelly(Arena* a, Vec3 center, f32 size, u32 cells, f32 mass, f32 edge_compliance, f32 volume_compliance) {
    if (cells < 1) cells = 1;
    u32 n1 = cells + 1, n = n1 * n1 * n1, ntet = cells * cells * cells * 5;
    SoftBody* b = soft_alloc(a, n, ntet * 6, ntet, 12 * cells * cells);
    /* kafes adımı cs, yarıçap cs/2: komşu parçacıklar değer (başka gövdenin parçacığı arasından geçemez) */
    f32 cs = size / (f32)(cells + 1), lat = cs * (f32)cells;
    Vec3 base = v3_sub(center, v3(lat * 0.5f, lat * 0.5f, lat * 0.5f));
    for (u32 k = 0; k < n1; k++) {
        for (u32 j = 0; j < n1; j++) {
            for (u32 i = 0; i < n1; i++) {
                u32 q = i + n1 * (j + n1 * k);
                b->x[q] = v3_add(base, v3((f32)i * cs, (f32)j * cs, (f32)k * cs));
                b->prev[q] = b->x[q];
            }
        }
    }
    /* 5 tetrahedronlu hücre; tek pariteli hücrelerde x aynalanır → komşu yüzler uyumlu */
    static const u8 T[5][4][3] = {
        { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
        { { 1, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 1, 1 } },
        { { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 }, { 1, 1, 1 } },
        { { 0, 1, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 1, 1 } },
        { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 1 } },
    };
    f32* m = ARENA_ARRAY(a, f32, n);
    f32 density = mass / (lat * lat * lat);
    for (u32 k = 0; k < cells; k++) {
        for (u32 j = 0; j < cells; j++) {
            for (u32 i = 0; i < cells; i++) {
                b32 odd = ((i + j + k) & 1u) != 0;
                for (u32 t = 0; t < 5; t++) {
                    SoftTet* tt = &b->tets[b->tet_count++];
                    for (u32 c = 0; c < 4; c++) {
                        u32 dx = odd ? 1u - T[t][c][0] : T[t][c][0];
                        tt->v[c] = (i + dx) + n1 * ((j + T[t][c][1]) + n1 * (k + T[t][c][2]));
                    }
                    f32 vol = tet_volume(b->x[tt->v[0]], b->x[tt->v[1]], b->x[tt->v[2]], b->x[tt->v[3]]);
                    if (vol < 0.0f) {
                        u32 s = tt->v[2]; tt->v[2] = tt->v[3]; tt->v[3] = s;
                        vol = -vol;
                    }
                    tt->rest_vol = vol;
                    for (u32 c = 0; c < 4; c++) m[tt->v[c]] += vol * density * 0.25f;
                }
            }
        }
    }
    for (u32 q = 0; q < n; q++) b->w[q] = m[q] > 0.0f ? 1.0f / m[q] : 0.0f;
    b->mass = mass;
    b->volume_compliance = volume_compliance;
    /* tetrahedron kenarları, tekilleştirilmiş (açık adresli küme) */
    usize mark = arena_mark(a);
    u32 cap = 1;
    while (cap < ntet * 12) cap <<= 1;
    u64* set = ARENA_ARRAY(a, u64, cap);
    static const u8 E[6][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 } };
    for (u32 t = 0; t < b->tet_count; t++) {
        for (u32 e = 0; e < 6; e++) {
            u32 p = b->tets[t].v[E[e][0]], q = b->tets[t].v[E[e][1]];
            u32 lo = p < q ? p : q, hi = p < q ? q : p;
            u64 key = ((u64)lo << 32 | hi) + 1;
            u32 h = (u32)((lo * 73856093u) ^ (hi * 19349663u)) & (cap - 1);
            while (set[h] && set[h] != key) h = (h + 1) & (cap - 1);
            if (set[h]) continue;
            set[h] = key;
            add_dist(b, lo, hi, edge_compliance, SOFT_EDGE);
        }
    }
    arena_reset(a, mark);
    /* yüzey: büyük küpün 6 yüzü, dışa CCW */
    for (u32 ax = 0; ax < 3; ax++) {
        for (u32 side = 0; side < 2; side++) {
            u32 ua = (ax + 1) % 3, va = (ax + 2) % 3;
            for (u32 v = 0; v < cells; v++) {
                for (u32 u = 0; u < cells; u++) {
                    u32 c[4][3];
                    const u32 du[4] = { 0, 1, 1, 0 }, dv[4] = { 0, 0, 1, 1 };
                    u32 id[4];
                    for (u32 r = 0; r < 4; r++) {
                        c[r][ax] = side * cells;
                        c[r][ua] = u + du[r];
                        c[r][va] = v + dv[r];
                        id[r] = c[r][0] + n1 * (c[r][1] + n1 * c[r][2]);
                    }
                    u32* tr = &b->tris[b->tri_count * 3];
                    if (side) { tr[0] = id[0]; tr[1] = id[1]; tr[2] = id[2]; tr[3] = id[0]; tr[4] = id[2]; tr[5] = id[3]; }
                    else      { tr[0] = id[0]; tr[1] = id[2]; tr[2] = id[1]; tr[3] = id[0]; tr[4] = id[3]; tr[5] = id[2]; }
                    b->tri_count += 2;
                }
            }
        }
    }
    b->radius = 0.5f * cs;
    b->damping = 0.5f;
    b->friction = 0.6f;
    return b;
}

void soft_pin(SoftBody* b, u32 i) {
    if (i < b->count) b->w[i] = 0.0f;
}

void soft_add_tethers(Arena* a, SoftBody* b) {
    SoftDist* nd = ARENA_ARRAY(a, SoftDist, b->dist_count + b->count);
    mem_copy(nd, b->dist, sizeof(SoftDist) * b->dist_count);
    b->dist = nd;
    for (u32 i = 0; i < b->count; i++) {
        if (b->w[i] == 0.0f) continue;
        u32 best = PHYS_NO_BODY;
        f32 bd = 3.4e38f;
        for (u32 k = 0; k < b->count; k++) {
            if (b->w[k] != 0.0f) continue;
            Vec3 d = v3_sub(b->x[i], b->x[k]);
            f32 l = v3_dot(d, d);
            if (l < bd) { bd = l; best = k; }
        }
        if (best != PHYS_NO_BODY) add_dist(b, i, best, 0.0f, SOFT_TETHER);
    }
}

void soft_set_velocity(SoftBody* b, Vec3 v) {
    for (u32 i = 0; i < b->count; i++) b->v[i] = b->w[i] > 0.0f ? v : v3(0, 0, 0);
}

/* ---- ölçümler ---- */

f32 soft_volume(const SoftBody* b) {
    f32 v = 0.0f;
    for (u32 t = 0; t < b->tet_count; t++) {
        const u32* q = b->tets[t].v;
        v += tet_volume(b->x[q[0]], b->x[q[1]], b->x[q[2]], b->x[q[3]]);
    }
    return v;
}

f32 soft_rest_volume(const SoftBody* b) {
    f32 v = 0.0f;
    for (u32 t = 0; t < b->tet_count; t++) v += b->tets[t].rest_vol;
    return v;
}

f32 soft_max_stretch(const SoftBody* b) {
    f32 m = 0.0f;
    for (u32 k = 0; k < b->dist_count; k++) {
        const SoftDist* d = &b->dist[k];
        if (d->kind != SOFT_STRETCH && d->kind != SOFT_EDGE) continue;
        f32 l = v3_len(v3_sub(b->x[d->a], b->x[d->b]));
        m = m_maxf(m, m_absf(l - d->rest) / d->rest);
    }
    return m;
}

Vec3 soft_momentum(const SoftBody* b) {
    Vec3 p = v3(0, 0, 0);
    for (u32 i = 0; i < b->count; i++) {
        if (b->w[i] > 0.0f) p = v3_add(p, v3_scale(b->v[i], 1.0f / b->w[i]));
    }
    return p;
}

u32 soft_hash(const SoftWorld* sw) {
    u32 h = 2166136261u;
    for (u32 k = 0; k < sw->count; k++) {
        const SoftBody* b = sw->bodies[k];
        for (u32 i = 0; i < b->count; i++) {
            const f32 v[6] = { b->x[i].x, b->x[i].y, b->x[i].z, b->v[i].x, b->v[i].y, b->v[i].z };
            for (u32 c = 0; c < 6; c++) h = (h ^ m_f32_bits(v[c])) * 16777619u;
        }
    }
    return h;
}

/* ---- dünya ---- */

void soft_world_init(SoftWorld* sw, Arena* a, u32 cap) {
    mem_zero(sw, sizeof(*sw));
    sw->bodies = ARENA_ARRAY(a, SoftBody*, cap);
    sw->cap = cap;
    sw->substeps = 10;
    sw->gravity = v3(0, -9.81f, 0);
    sw->two_way = TRUE;
    sw->attach = ARENA_ARRAY(a, SoftAttach, SOFT_MAX_ATTACH);
}

u32 soft_world_add(SoftWorld* sw, SoftBody* b) {
    if (sw->count >= sw->cap) sys_panic("soft: kapasite dolu");
    sw->bodies[sw->count] = b;
    return sw->count++;
}

/* ---- rijit yüzey uzaklığı (dışarı +), n dışa normal ---- */

/* Ericson, Real-Time Collision Detection 5.1.5 */
static Vec3 closest_on_tri(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    Vec3 ab = v3_sub(b, a), ac = v3_sub(c, a), ap = v3_sub(p, a);
    f32 d1 = v3_dot(ab, ap), d2 = v3_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    Vec3 bp = v3_sub(p, b);
    f32 d3 = v3_dot(ab, bp), d4 = v3_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return v3_add(a, v3_scale(ab, d1 / (d1 - d3)));
    Vec3 cp = v3_sub(p, c);
    f32 d5 = v3_dot(ab, cp), d6 = v3_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return v3_add(a, v3_scale(ac, d2 / (d2 - d6)));
    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return v3_add(b, v3_scale(v3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }
    f32 den = 1.0f / (va + vb + vc);
    return v3_add(a, v3_add(v3_scale(ab, vb * den), v3_scale(ac, vc * den)));
}

static f32 body_sdf(const Body* b, Vec3 p, Vec3 prev, f32 reach, Vec3* n) {
    switch (b->shape) {
    case SHAPE_PLANE:
        *n = b->plane_n;
        return v3_dot(b->plane_n, p) - b->plane_d;
    case SHAPE_SPHERE: {
        Vec3 d = v3_sub(p, b->pos);
        f32 l = v3_len(d);
        *n = l > 1e-9f ? v3_scale(d, 1.0f / l) : v3(0, 1, 0);
        return l - b->radius;
    }
    case SHAPE_CAPSULE: {
        Vec3 ax = mat3_col(&b->rot_m, 1);
        f32 t = m_clampf(v3_dot(v3_sub(p, b->pos), ax), -b->half_height, b->half_height);
        Vec3 d = v3_sub(p, v3_add(b->pos, v3_scale(ax, t)));
        f32 l = v3_len(d);
        *n = l > 1e-9f ? v3_scale(d, 1.0f / l) : v3(0, 1, 0);
        return l - b->radius;
    }
    case SHAPE_BOX: {
        Vec3 q = mat3_tmul_v3(&b->rot_m, v3_sub(p, b->pos));
        const f32 qq[3] = { q.x, q.y, q.z }, hh[3] = { b->half.x, b->half.y, b->half.z };
        f32 d[3], o[3], best = -3.4e38f;
        u32 ax = 0;
        b32 outside = FALSE;
        for (u32 k = 0; k < 3; k++) {
            d[k] = m_absf(qq[k]) - hh[k];
            o[k] = m_maxf(d[k], 0.0f) * (qq[k] >= 0.0f ? 1.0f : -1.0f);
            if (d[k] > 0.0f) outside = TRUE;
            if (d[k] > best) { best = d[k]; ax = k; }
        }
        if (outside) {
            Vec3 ov = v3(o[0], o[1], o[2]);
            f32 l = v3_len(ov);
            *n = mat3_mul_v3(&b->rot_m, v3_scale(ov, 1.0f / l));
            return l;
        }
        Vec3 nl = v3(ax == 0 ? 1.0f : 0.0f, ax == 1 ? 1.0f : 0.0f, ax == 2 ? 1.0f : 0.0f);
        if (qq[ax] < 0.0f) nl = v3_scale(nl, -1.0f);
        *n = mat3_mul_v3(&b->rot_m, nl);
        return best;
    }
    case SHAPE_HULL: {
        Vec3 q = mat3_tmul_v3(&b->rot_m, v3_sub(p, b->pos));
        const ConvexHull* h = b->hull;
        f32 best = -3.4e38f;
        u32 bf = 0;
        for (u32 f = 0; f < h->fcount; f++) {
            f32 s = v3_dot(h->faces[f].n, q) - h->faces[f].d;
            if (s > best) { best = s; bf = f; }
        }
        if (best > 0.0f) {
            if (best > reach) { *n = mat3_mul_v3(&b->rot_m, h->faces[bf].n); return best; }
            /* dışarıda: kesin uzaklık GJK ile (yüz düzlemi köşelerde uzaklığı küçük gösterir) */
            Core pc, hc;
            pc.kind = CORE_POINT;
            pc.p0 = pc.p1 = p;
            pc.radius = 0.0f;
            pc.hull = NULL;
            phys_body_core(b, &hc);
            GjkResult g;
            gjk_distance(&pc, &hc, &g);
            if (!g.overlap && g.dist > 1e-6f) {
                *n = v3_scale(v3_sub(g.pa, g.pb), 1.0f / g.dist);
                return g.dist;
            }
        }
        *n = mat3_mul_v3(&b->rot_m, h->faces[bf].n);
        return best;
    }
    case SHAPE_MESH: {
        const TriMesh* tm = b->mesh;
        Vec3 pl = mat3_tmul_v3(&b->rot_m, v3_sub(p, b->pos));
        Vec3 ql = mat3_tmul_v3(&b->rot_m, v3_sub(prev, b->pos));
        Vec3 e = v3(reach, reach, reach);
        Vec3 mn = v3_sub(v3(m_minf(pl.x, ql.x), m_minf(pl.y, ql.y), m_minf(pl.z, ql.z)), e);
        Vec3 mx = v3_add(v3(m_maxf(pl.x, ql.x), m_maxf(pl.y, ql.y), m_maxf(pl.z, ql.z)), e);
        u32 tris[64];
        u32 nt = trimesh_query_aabb(tm, mn, mx, tris, 64);
        f32 best = 3.4e38f;
        Vec3 bn = v3(0, 1, 0);
        for (u32 i = 0; i < nt; i++) {
            Vec3 a0, a1, a2;
            trimesh_tri(tm, tris[i], &a0, &a1, &a2);
            Vec3 fn = v3_cross(v3_sub(a1, a0), v3_sub(a2, a0));
            f32 fl = v3_len(fn);
            if (!(fl > 0.0f)) continue;
            fn = v3_scale(fn, 1.0f / fl);
            Vec3 c = closest_on_tri(pl, a0, a1, a2);
            Vec3 d = v3_sub(pl, c);
            f32 l = v3_len(d);
            f32 s_now = v3_dot(v3_sub(pl, a0), fn), s_prev = v3_dot(v3_sub(ql, a0), fn);
            f32 dist;
            Vec3 nn;
            b32 crossed = (s_now < 0.0f) != (s_prev < 0.0f);
            if (crossed) {   /* düzlemi üçgenin içinden mi geçti? (izdüşüm üçgende) */
                Vec3 pp = v3_sub(pl, v3_scale(fn, s_now));
                Vec3 dq = v3_sub(closest_on_tri(pp, a0, a1, a2), pp);
                crossed = v3_dot(dq, dq) < 1e-10f;
            }
            if (crossed) {
                /* alt adımda üçgenin içinden geçti: önceki tarafa it */
                nn = s_prev >= 0.0f ? fn : v3_scale(fn, -1.0f);
                dist = -m_absf(s_now);
            } else {
                dist = l;
                nn = l > 1e-7f ? v3_scale(d, 1.0f / l) : (s_prev >= 0.0f ? fn : v3_scale(fn, -1.0f));
            }
            if (dist < best) { best = dist; bn = nn; }
        }
        *n = mat3_mul_v3(&b->rot_m, bn);
        return best;
    }
    default:
        *n = v3(0, 1, 0);
        return 3.4e38f;
    }
}

/* ---- adım ---- */

typedef struct Cand {
    u32  id;
    Body pose;    /* alt adıma ara değerlenmiş poz */
    Vec3 p0, p1;
    Quat q0, q1;
    b32  moving;
    u32  path;    /* g_paths indeksi */
} Cand;

/* Hareketli rijit gövdenin adım içi yolu (yumuşak gövdeler arasında ortak). Temas olana dek eski davranış:
 * p = lerp(pos0, pos, t). İlk temas itkisinden sonra p += v·h ve itkiler v'yi de değiştirir → gövde aynı
 * karede yavaşlar (kumaş mermiyi adım içinde tutabilir). Adım sonunda dokunulan gövdenin konumu p'ye yazılır. */
typedef struct RigidPath {
    u32  id;
    Vec3 p0, p1, p, p_prev, v;
    b32  moving, touched;
} RigidPath;

#define SOFT_MAX_PATHS (SOFT_MAX_BODIES * SOFT_MAX_CAND)
static RigidPath g_paths[SOFT_MAX_PATHS];
static u32 g_path_count;

static u32 path_of(const Body* rb, u32 id, f32 dt, b32 moving) {
    for (u32 i = 0; i < g_path_count; i++) {
        if (g_paths[i].id == id) return i;
    }
    RigidPath* p = &g_paths[g_path_count];
    p->id = id;
    p->p0 = rb->pos0;
    p->p1 = rb->pos;
    p->p = p->p_prev = rb->pos0;
    p->v = v3_scale(v3_sub(rb->pos, rb->pos0), 1.0f / dt);
    p->moving = moving;
    p->touched = FALSE;
    return g_path_count++;
}

/* parçacıkların kazandığı momentum P → gövdeye −P (ve yolun hızına) */
static void rigid_impulse(SoftWorld* sw, PhysWorld* pw, const Cand* c, Vec3 P, Vec3 point) {
    Body* real = &pw->bodies[c->id];
    if (!sw->two_way || !(real->inv_mass > 0.0f)) return;
    b32 asleep = (real->flags & BODY_ASLEEP) != 0;
    if (asleep && !(v3_len(P) * real->inv_mass > 0.5f * pw->sleep_lin)) return;
    phys_apply_impulse_at(pw, c->id, v3_scale(P, -1.0f), point);
    RigidPath* rp = &g_paths[c->path];
    rp->v = v3_sub(rp->v, v3_scale(P, real->inv_mass));
    rp->touched = TRUE;
}

static void soft_aabb(SoftBody* b, f32 pad) {
    Vec3 mn = b->x[0], mx = b->x[0];
    for (u32 i = 1; i < b->count; i++) {
        mn = v3(m_minf(mn.x, b->x[i].x), m_minf(mn.y, b->x[i].y), m_minf(mn.z, b->x[i].z));
        mx = v3(m_maxf(mx.x, b->x[i].x), m_maxf(mx.y, b->x[i].y), m_maxf(mx.z, b->x[i].z));
    }
    Vec3 e = v3(pad, pad, pad);
    b->aabb_min = v3_sub(mn, e);
    b->aabb_max = v3_add(mx, e);
}

static void solve_constraints(SoftBody* b, f32 inv_h2) {
    for (u32 k = 0; k < b->dist_count; k++) {
        const SoftDist* d = &b->dist[k];
        f32 wa = b->w[d->a], wb = b->w[d->b];
        f32 ws = wa + wb + d->compliance * inv_h2;
        if (!(ws > 0.0f) || wa + wb == 0.0f) continue;
        Vec3 dx = v3_sub(b->x[d->a], b->x[d->b]);
        f32 l = v3_len(dx);
        if (l < 1e-9f || (d->kind == SOFT_TETHER && l <= d->rest)) continue;
        f32 dl = -(l - d->rest) / ws;
        Vec3 g = v3_scale(dx, dl / l);
        b->x[d->a] = v3_add(b->x[d->a], v3_scale(g, wa));
        b->x[d->b] = v3_sub(b->x[d->b], v3_scale(g, wb));
    }
    /* hacim: C = V − V0, ∇_i V (Müller'in sırası) */
    static const u8 ORD[4][3] = { { 1, 3, 2 }, { 0, 2, 3 }, { 0, 3, 1 }, { 0, 1, 2 } };
    f32 alpha = b->volume_compliance * inv_h2;
    for (u32 t = 0; t < b->tet_count; t++) {
        const SoftTet* tt = &b->tets[t];
        Vec3 g[4];
        f32 ws = 0.0f;
        for (u32 c = 0; c < 4; c++) {
            Vec3 p0 = b->x[tt->v[ORD[c][0]]], p1 = b->x[tt->v[ORD[c][1]]], p2 = b->x[tt->v[ORD[c][2]]];
            g[c] = v3_scale(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)), 1.0f / 6.0f);
            ws += b->w[tt->v[c]] * v3_dot(g[c], g[c]);
        }
        ws += alpha;
        if (!(ws > 0.0f)) continue;
        f32 C = tet_volume(b->x[tt->v[0]], b->x[tt->v[1]], b->x[tt->v[2]], b->x[tt->v[3]]) - tt->rest_vol;
        f32 dl = -C / ws;
        for (u32 c = 0; c < 4; c++) b->x[tt->v[c]] = v3_add(b->x[tt->v[c]], v3_scale(g[c], dl * b->w[tt->v[c]]));
    }
}

static void wind_forces(SoftBody* b, Vec3 wind, f32 h) {
    for (u32 t = 0; t < b->tri_count; t++) {
        u32 i0 = b->tris[t * 3], i1 = b->tris[t * 3 + 1], i2 = b->tris[t * 3 + 2];
        Vec3 nn = v3_cross(v3_sub(b->x[i1], b->x[i0]), v3_sub(b->x[i2], b->x[i0]));
        f32 len = v3_len(nn);
        if (len < 1e-12f) continue;
        Vec3 nh = v3_scale(nn, 1.0f / len);
        Vec3 vrel = v3_sub(v3_scale(v3_add(v3_add(b->v[i0], b->v[i1]), b->v[i2]), 1.0f / 3.0f), wind);
        f32 vn = v3_dot(vrel, nh);
        /* F = −½ρC_d·A·v_n|v_n|·n̂, A = len/2; köşelere üçte bir. Açık (explicit) ikinci dereceden sürükleme
         * yüksek hızda bir alt adımda göreli normal hızı tersine çevirip patlar → köşe başına |Δv_n| ≤ |v_n|. */
        f32 fmag = b->drag * 0.5f * len * vn * vn * (h / 3.0f);
        const u32 ids[3] = { i0, i1, i2 };
        f32 wmax = m_maxf(b->w[i0], m_maxf(b->w[i1], b->w[i2]));
        if (fmag * wmax > m_absf(vn)) fmag = m_absf(vn) / wmax;
        Vec3 F = v3_scale(nh, vn > 0.0f ? -fmag : fmag);
        for (u32 c = 0; c < 3; c++) b->v[ids[c]] = v3_add(b->v[ids[c]], v3_scale(F, b->w[ids[c]]));
    }
}

static void soft_pair(SoftWorld* sw, SoftBody* A, SoftBody* B) {
    if (A->aabb_min.x > B->aabb_max.x || A->aabb_max.x < B->aabb_min.x || A->aabb_min.y > B->aabb_max.y ||
        A->aabb_max.y < B->aabb_min.y || A->aabb_min.z > B->aabb_max.z || A->aabb_max.z < B->aabb_min.z) return;
    f32 r = A->radius + B->radius, r2 = r * r;
    for (u32 i = 0; i < A->count; i++) {
        Vec3 xi = A->x[i];
        if (xi.x < B->aabb_min.x || xi.x > B->aabb_max.x || xi.y < B->aabb_min.y || xi.y > B->aabb_max.y ||
            xi.z < B->aabb_min.z || xi.z > B->aabb_max.z) continue;
        for (u32 j = 0; j < B->count; j++) {
            Vec3 d = v3_sub(A->x[i], B->x[j]);
            f32 l2 = v3_dot(d, d);
            if (l2 >= r2 || l2 < 1e-18f) continue;
            f32 wa = A->w[i], wb = B->w[j];
            if (wa + wb == 0.0f) continue;
            f32 l = m_sqrtf(l2);
            Vec3 n = v3_scale(d, 1.0f / l);
            f32 depth = r - l;
            Vec3 corr = v3_scale(n, depth / (wa + wb));
            A->x[i] = v3_add(A->x[i], v3_scale(corr, wa));
            B->x[j] = v3_sub(B->x[j], v3_scale(corr, wb));
            /* sürtünme: göreli teğetsel yer değiştirme */
            Vec3 rel = v3_sub(v3_sub(A->x[i], A->prev[i]), v3_sub(B->x[j], B->prev[j]));
            Vec3 tg = v3_sub(rel, v3_scale(n, v3_dot(rel, n)));
            f32 lt = v3_len(tg);
            if (lt > 0.0f) {
                f32 mu = m_sqrtf(A->friction * B->friction);
                Vec3 fr = lt < mu * depth ? tg : v3_scale(tg, mu * depth / lt);
                fr = v3_scale(fr, 1.0f / (wa + wb));
                A->x[i] = v3_sub(A->x[i], v3_scale(fr, wa));
                B->x[j] = v3_add(B->x[j], v3_scale(fr, wb));
            }
            sw->soft_contacts++;
        }
    }
}

static void attach_add(SoftWorld* sw, u32 k, u32 i, u32 body, Vec3 offset, Vec3 dir) {
    for (u32 a = 0; a < sw->attach_count; a++) {
        const SoftAttach* t = &sw->attach[a];
        if (t->soft == k && t->particle == i) return;
    }
    if (sw->attach_count >= SOFT_MAX_ATTACH) return;
    SoftAttach* t = &sw->attach[sw->attach_count++];
    t->soft = k;
    t->particle = i;
    t->body = body;
    t->offset = offset;
    t->dir = dir;
}

/* yakalanan parçacıkları gövdeye taşı; kumaşın (kısıtların) çektiği kadar momentum gövdeye.
 * Parçacık bir alt adımda gövde boyundan fazla çekildiyse (kumaş gerildi / gövde döndü) bırakılır. */
static void solve_attach(SoftWorld* sw, PhysWorld* pw, u32 k, SoftBody* b, Cand* cand, u32 nc, f32 h) {
    u32 kept = 0;
    for (u32 a = 0; a < sw->attach_count; a++) {
        SoftAttach t = sw->attach[a];
        b32 keep = TRUE;
        if (t.soft == k && b->w[t.particle] != 0.0f) {
            const Cand* cd = NULL;
            for (u32 c = 0; c < nc && !cd; c++) if (cand[c].id == t.body) cd = &cand[c];
            keep = cd != NULL;
            if (cd) {
                Vec3 target = v3_add(g_paths[cd->path].p, t.offset);
                Vec3 corr = v3_sub(target, b->x[t.particle]);
                if (v3_len(corr) > pw->bodies[t.body].max_extent + b->radius) {
                    keep = FALSE;
                } else {
                    b->x[t.particle] = target;
                    rigid_impulse(sw, pw, cd, v3_scale(corr, 1.0f / (b->w[t.particle] * h)), target);
                }
            }
        }
        if (keep) sw->attach[kept++] = t;
    }
    sw->attach_count = kept;
}

static void collide_rigid(SoftWorld* sw, PhysWorld* pw, u32 k, SoftBody* b, Cand* cand, u32 nc, f32 h) {
    f32 r = b->radius;
    f32 inv_h = 1.0f / h;
    for (u32 c = 0; c < nc; c++) {
        Body* rb = &cand[c].pose;
        Body* real = &pw->bodies[cand[c].id];
        b32 dynamic = sw->two_way && real->inv_mass > 0.0f;
        f32 mu = m_sqrtf(b->friction * real->friction);
        /* Sürekli (hareketli gövde, tri_collide'lı kumaş): parçacığın gövdeye göre hareketi alt adımda gövde
         * küresine (küre: kendisi; diğerleri: iç küre min_extent) girdiyse geldiği yandan yüzeye konur.
         * Hızlı gövde böylece her alt adım izdüşüm alanındaki tüm parçacıkları önüne katar (mermi geçemez). */
        b32 swept = b->catch_fast && cand[c].moving && rb->shape != SHAPE_PLANE && rb->shape != SHAPE_MESH;
        f32 R = (rb->shape == SHAPE_SPHERE ? rb->radius : real->min_extent) + r;
        Vec3 c0 = swept ? g_paths[cand[c].path].p_prev : rb->pos, c1 = rb->pos;
        Vec3 su = v3(0, 0, 0);
        if (swept) {
            Vec3 pv = g_paths[cand[c].path].v;
            f32 sp = v3_len(pv);
            swept = sp > SOFT_SWEPT_MIN;
            if (swept) su = v3_scale(pv, 1.0f / sp);
        }
        for (u32 i = 0; i < b->count; i++) {
            if (b->w[i] == 0.0f) continue;
            Vec3 p = b->x[i];
            if (swept) {
                /* İz silindiri: hareket yönü u, parçacığın gövde eksenine yanal uzaklığı < R ve adım başında
                 * gövde merkezinin önündeyse (u boyunca), ön yarıkürenin önünde kalmalı. Silindir (küre değil):
                 * gerili kumaşın parçacıkları kürenin yanından kayıp arkaya geçemez → kumaş cep olur, gövdeyi tutar. */
                Vec3 a0 = v3_sub(b->prev[i], c0), e1 = v3_sub(p, c1);
                f32 along0 = v3_dot(a0, su), along = v3_dot(e1, su);
                Vec3 lat = v3_sub(e1, v3_scale(su, along));
                f32 l2 = v3_dot(lat, lat);
                if (along0 >= 0.0f && l2 < R * R) {
                    f32 front = m_sqrtf(R * R - l2);
                    if (along < front) {
                        Vec3 np = v3_add(c1, v3_add(lat, v3_scale(su, front)));
                        Vec3 corr = v3_sub(np, p);
                        b->x[i] = np;
                        sw->rigid_contacts++;
                        if (dynamic) {
                            rigid_impulse(sw, pw, &cand[c], v3_scale(corr, inv_h / b->w[i]), v3_sub(np, v3_scale(su, r)));
                            attach_add(sw, k, i, cand[c].id, v3_sub(np, c1), su);
                        }
                        continue;
                    }
                }
            }
            if (rb->shape != SHAPE_PLANE &&
                (p.x < rb->aabb_min.x - r || p.x > rb->aabb_max.x + r || p.y < rb->aabb_min.y - r ||
                 p.y > rb->aabb_max.y + r || p.z < rb->aabb_min.z - r || p.z > rb->aabb_max.z + r)) continue;
            Vec3 n;
            f32 dist = body_sdf(rb, p, b->prev[i], 2.0f * r, &n);
            if (dist >= r) continue;
            f32 depth = r - dist;
            Vec3 corr = v3_scale(n, depth);
            p = v3_add(p, corr);
            /* sürtünme: gövde yüzeyine göre teğetsel yer değiştirme */
            Vec3 vb = v3_add(real->vel, v3_cross(real->ang_vel, v3_sub(p, rb->pos)));
            Vec3 dd = v3_sub(v3_sub(p, b->prev[i]), v3_scale(vb, h));
            Vec3 dt = v3_sub(dd, v3_scale(n, v3_dot(dd, n)));
            f32 lt = v3_len(dt);
            Vec3 fr = v3(0, 0, 0);
            if (lt > 0.0f) fr = lt < mu * depth ? v3_scale(dt, -1.0f) : v3_scale(dt, -mu * depth / lt);
            p = v3_add(p, fr);
            b->x[i] = p;
            sw->rigid_contacts++;
            if (dynamic) {
                /* parçacığın momentum değişimi kadar ters itki */
                Vec3 P = v3_scale(v3_add(corr, fr), inv_h / b->w[i]);
                rigid_impulse(sw, pw, &cand[c], P, v3_sub(p, v3_scale(n, r)));
            }
        }
    }
}

/* ---- kumaş öz-çarpışması: uzamsal karma (hücre = 2r), sayma sıralaması (artan indeks, belirleyici) ---- */

static inline u32 cell_hash(i32 x, i32 y, i32 z, u32 mask) {
    return ((u32)x * 92837111u ^ (u32)y * 689287499u ^ (u32)z * 283923481u) & mask;
}

static void self_collide(SoftWorld* sw, SoftBody* b) {
    f32 d = 2.0f * b->radius, d2 = d * d, inv = 1.0f / d;
    u32 n = b->count, mask = b->hash_cap - 1;
    u32* start = b->hash_start;
    mem_zero(start, sizeof(u32) * (b->hash_cap + 1));
    for (u32 i = 0; i < n; i++) {
        Vec3 p = b->x[i];
        start[cell_hash((i32)m_floorf(p.x * inv), (i32)m_floorf(p.y * inv), (i32)m_floorf(p.z * inv), mask) + 1]++;
    }
    for (u32 k = 0; k < b->hash_cap; k++) start[k + 1] += start[k];
    /* doldurma: start[h] ilerletilir, sonra bir kaydırılarak geri alınır */
    for (u32 i = 0; i < n; i++) {
        Vec3 p = b->x[i];
        u32 h = cell_hash((i32)m_floorf(p.x * inv), (i32)m_floorf(p.y * inv), (i32)m_floorf(p.z * inv), mask);
        b->hash_items[start[h]++] = i;
    }
    for (u32 k = b->hash_cap; k > 0; k--) start[k] = start[k - 1];
    start[0] = 0;
    for (u32 i = 0; i < n; i++) {
        Vec3 p = b->x[i];
        i32 cx = (i32)m_floorf(p.x * inv), cy = (i32)m_floorf(p.y * inv), cz = (i32)m_floorf(p.z * inv);
        for (i32 dz = -1; dz <= 1; dz++) {
            for (i32 dy = -1; dy <= 1; dy++) {
                for (i32 dx = -1; dx <= 1; dx++) {
                    /* (komşu hücreler aynı karma kovasına düşerse çift iki kez işlenir: seyrek ve zararsız) */
                    u32 h = cell_hash(cx + dx, cy + dy, cz + dz, mask);
                    for (u32 k = start[h]; k < start[h + 1]; k++) {
                        u32 j = b->hash_items[k];
                        if (j <= i) continue;
                        f32 wi = b->w[i], wj = b->w[j];
                        if (wi + wj == 0.0f) continue;
                        Vec3 dv = v3_sub(b->x[j], b->x[i]);
                        f32 l2 = v3_dot(dv, dv);
                        if (l2 >= d2 || l2 < 1e-18f) continue;
                        Vec3 r0 = v3_sub(b->x0[j], b->x0[i]);
                        if (v3_dot(r0, r0) < d2) continue;   /* dinlenmede zaten yakın: kumaşın kendi örgüsü */
                        f32 l = m_sqrtf(l2);
                        Vec3 nn = v3_scale(dv, 1.0f / l);
                        f32 depth = d - l;
                        Vec3 corr = v3_scale(nn, depth / (wi + wj));
                        b->x[i] = v3_sub(b->x[i], v3_scale(corr, wi));
                        b->x[j] = v3_add(b->x[j], v3_scale(corr, wj));
                        /* sürtünme: göreli teğetsel yer değiştirme */
                        Vec3 rel = v3_sub(v3_sub(b->x[j], b->prev[j]), v3_sub(b->x[i], b->prev[i]));
                        Vec3 tg = v3_sub(rel, v3_scale(nn, v3_dot(rel, nn)));
                        f32 lt = v3_len(tg);
                        if (lt > 0.0f) {
                            Vec3 fr = lt < b->friction * depth ? tg : v3_scale(tg, b->friction * depth / lt);
                            fr = v3_scale(fr, 1.0f / (wi + wj));
                            b->x[i] = v3_add(b->x[i], v3_scale(fr, wi));
                            b->x[j] = v3_sub(b->x[j], v3_scale(fr, wj));
                        }
                        sw->self_contacts++;
                    }
                }
            }
        }
    }
}

/* ---- rijit gövde ↔ kumaş üçgeni (yavaş gövdeler, ≤ SOFT_SWEPT_MIN) ----
 * Dar faz phys_collide_cores (GJK / EPA, mesh üçgenleriyle aynı yol): parçacıkların arasındaki yüzeye değen
 * küçük gövdeler ve keskin kenarlar için. Hızlı gövdeler iz silindiri + yakalamayla (collide_rigid).
 * Düzeltme D temas noktasında; köşelere barisentrik ağırlıkla: Δx_i = D·w_i·m_i⁻¹ / Σ w_j²·m_j⁻¹;
 * parçacık momentumu P = D / (Σ w_j²·m_j⁻¹ · h) → gövdeye −P. */

static void bary(Vec3 q, Vec3 a, Vec3 b, Vec3 c, f32* w0, f32* w1, f32* w2) {
    Vec3 e0 = v3_sub(b, a), e1 = v3_sub(c, a), r = v3_sub(q, a);
    f32 d00 = v3_dot(e0, e0), d01 = v3_dot(e0, e1), d11 = v3_dot(e1, e1);
    f32 d20 = v3_dot(r, e0), d21 = v3_dot(r, e1);
    f32 den = d00 * d11 - d01 * d01;
    if (!(den > 0.0f)) { *w0 = 1.0f; *w1 = *w2 = 0.0f; return; }
    *w1 = (d11 * d20 - d01 * d21) / den;
    *w2 = (d00 * d21 - d01 * d20) / den;
    *w0 = 1.0f - *w1 - *w2;
}

static void push_tri(SoftWorld* sw, PhysWorld* pw, SoftBody* b, const Cand* cd, const u32* ids, Vec3 q, Vec3 D, f32 h) {
    Vec3 a = b->x[ids[0]], bb = b->x[ids[1]], c = b->x[ids[2]];
    f32 w[3];
    Vec3 qc = closest_on_tri(q, a, bb, c);
    bary(qc, a, bb, c, &w[0], &w[1], &w[2]);
    f32 S = 0.0f, wmax = 0.0f;
    for (u32 k = 0; k < 3; k++) {
        S += w[k] * w[k] * b->w[ids[k]];
        wmax = m_maxf(wmax, b->w[ids[k]]);
    }
    /* nokta ağırlıkça iğneli köşelerde (Σw²m⁻¹ küçük): serbest köşeler ~1/S kat savrulurdu → bölge sabit sayılır */
    if (!(S > 0.1f * wmax)) return;
    for (u32 k = 0; k < 3; k++) b->x[ids[k]] = v3_add(b->x[ids[k]], v3_scale(D, w[k] * b->w[ids[k]] / S));
    rigid_impulse(sw, pw, cd, v3_scale(D, 1.0f / (S * h)), qc);
    sw->tri_contacts++;
}

static void collide_tris(SoftWorld* sw, PhysWorld* pw, SoftBody* b, Cand* cand, u32 nc, f32 h) {
    f32 thick = b->radius;
    for (u32 ci = 0; ci < nc; ci++) {
        Cand* cd = &cand[ci];
        const Body* rb = &cd->pose;
        if (rb->shape == SHAPE_PLANE || rb->shape == SHAPE_MESH) continue;   /* düzlem / mesh: parçacık teması yeter */
        /* hızlı gövde: iz silindiri + yakalama (collide_rigid / solve_attach) yeterli ve kararlı */
        if (cd->moving && v3_len(g_paths[cd->path].v) > SOFT_SWEPT_MIN) continue;
        Core rc;
        phys_body_core(rb, &rc);
        Vec3 bmn = v3_sub(rb->aabb_min, v3(thick, thick, thick)), bmx = v3_add(rb->aabb_max, v3(thick, thick, thick));
        for (u32 t = 0; t < b->tri_count; t++) {
            const u32* ids = &b->tris[t * 3];
            if (b->w[ids[0]] + b->w[ids[1]] + b->w[ids[2]] == 0.0f) continue;
            Vec3 a = b->x[ids[0]], bb = b->x[ids[1]], c = b->x[ids[2]];
            Vec3 tmn = v3(m_minf(m_minf(a.x, bb.x), c.x), m_minf(m_minf(a.y, bb.y), c.y), m_minf(m_minf(a.z, bb.z), c.z));
            Vec3 tmx = v3(m_maxf(m_maxf(a.x, bb.x), c.x), m_maxf(m_maxf(a.y, bb.y), c.y), m_maxf(m_maxf(a.z, bb.z), c.z));
            if (tmn.x > bmx.x || tmx.x < bmn.x || tmn.y > bmx.y || tmx.y < bmn.y || tmn.z > bmx.z || tmx.z < bmn.z) continue;
            Vec3 nn = v3_cross(v3_sub(bb, a), v3_sub(c, a));
            if (!(v3_dot(nn, nn) > 1e-4f * thick * thick * thick * thick)) continue;   /* ezilmiş (neredeyse dejenere) üçgen */
            /* dar faz: rijit çekirdek ↔ üçgen (GJK / EPA + kırpma; mesh üçgenleriyle aynı yol), normal gövdeden
             * üçgene; en derin nokta kalınlık kadar dışarı */
            Core tc;
            core_triangle(&tc, a, bb, c);
            Manifold m;
            if (!phys_collide_cores(&rc, &tc, &m)) continue;
            u32 deepest = 0;
            for (u32 q = 1; q < m.count; q++) if (m.p[q].depth > m.p[deepest].depth) deepest = q;
            f32 d = m.p[deepest].depth + thick;
            /* sonlu ve gövde boyunu aşmayan düzeltme (üçgen gövdenin içine gömülmüşse en yakın yüzeye) */
            if (!(d > 0.0f) || !(d < 2.0f * (pw->bodies[cd->id].max_extent + thick)) || !(v3_dot(m.normal, m.normal) > 0.5f)) continue;
            push_tri(sw, pw, b, cd, ids, m.p[deepest].pos, v3_scale(m.normal, d), h);
        }
    }
}

static Quat quat_nlerp(Quat a, Quat b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    Quat q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    return quat_normalize(q);
}

/* gövde kopyaları büyük; yığında değil (tek iş parçacığı) */
static Cand g_cand_store[SOFT_MAX_BODIES][SOFT_MAX_CAND];
#define SOFT_QUERY_MAX 1024
static u32 g_query[SOFT_QUERY_MAX];

void soft_world_step(SoftWorld* sw, PhysWorld* pw, f32 dt) {
    f64 t0 = sys_now_ms();
    u32 n = sw->substeps ? sw->substeps : 1;
    f32 h = dt / (f32)n, inv_h2 = 1.0f / (h * h);
    sw->rigid_contacts = sw->soft_contacts = sw->tri_contacts = sw->self_contacts = 0;
    g_path_count = 0;
    Cand* cands[SOFT_MAX_BODIES];
    u32 ncand[SOFT_MAX_BODIES];
    u32 nb = sw->count < SOFT_MAX_BODIES ? sw->count : SOFT_MAX_BODIES;
    for (u32 k = 0; k < nb; k++) {
        SoftBody* b = sw->bodies[k];
        f32 vmax = 0.0f;
        for (u32 i = 0; i < b->count; i++) vmax = m_maxf(vmax, v3_len(b->v[i]));
        soft_aabb(b, vmax * dt + 2.0f * b->radius + 0.05f);
        cands[k] = g_cand_store[k];
        ncand[k] = 0;
        if (!pw) continue;
        /* adaylar artan indeks sırasıyla: ağaç / doğrusal sorgu (kutu, rijit gövdelerin bu adımdaki en büyük
         * yer değiştirmesi kadar büyütülür → aşağıdaki birleşim-AABB testinin bulacağı her gövdeyi kapsar)
         * + düzlemler, birleştirilerek */
        Vec3 e = v3(pw->max_disp, pw->max_disp, pw->max_disp);
        u32 nq = phys_query_aabb(pw, v3_sub(b->aabb_min, e), v3_add(b->aabb_max, e), g_query, SOFT_QUERY_MAX);
        u32 qi = 0, pi = 0;
        while ((qi < nq || pi < pw->plane_count) && ncand[k] < SOFT_MAX_CAND) {
            u32 j;
            if (pi < pw->plane_count && (qi >= nq || pw->plane_ids[pi] < g_query[qi])) j = pw->plane_ids[pi++];
            else j = g_query[qi++];
            const Body* rb = &pw->bodies[j];
            /* adım başı + sonu AABB birleşimi (dönüş yok sayılır) */
            Vec3 sh = v3_sub(rb->pos0, rb->pos);
            Vec3 mn = v3(m_minf(rb->aabb_min.x, rb->aabb_min.x + sh.x), m_minf(rb->aabb_min.y, rb->aabb_min.y + sh.y), m_minf(rb->aabb_min.z, rb->aabb_min.z + sh.z));
            Vec3 mx = v3(m_maxf(rb->aabb_max.x, rb->aabb_max.x + sh.x), m_maxf(rb->aabb_max.y, rb->aabb_max.y + sh.y), m_maxf(rb->aabb_max.z, rb->aabb_max.z + sh.z));
            if (rb->shape != SHAPE_PLANE &&
                (mn.x > b->aabb_max.x || mx.x < b->aabb_min.x || mn.y > b->aabb_max.y || mx.y < b->aabb_min.y ||
                 mn.z > b->aabb_max.z || mx.z < b->aabb_min.z)) continue;
            Cand* c = &cands[k][ncand[k]++];
            c->id = j;
            c->pose = *rb;
            c->pose.aabb_min = mn;
            c->pose.aabb_max = mx;
            c->p0 = rb->pos0;
            c->p1 = rb->pos;
            c->q0 = rb->rot0;
            c->q1 = rb->rot;
            c->moving = rb->inv_mass > 0.0f && (rb->pos0.x != rb->pos.x || rb->pos0.y != rb->pos.y || rb->pos0.z != rb->pos.z ||
                                                rb->rot0.x != rb->rot.x || rb->rot0.y != rb->rot.y || rb->rot0.z != rb->rot.z);
            c->path = path_of(rb, j, dt, c->moving);
        }
    }
    for (u32 s = 0; s < n; s++) {
        f32 t = (f32)(s + 1) / (f32)n;
        /* rijit yollar: temas yoksa ara değer, varsa kendi hızıyla */
        for (u32 i = 0; i < g_path_count; i++) {
            RigidPath* rp = &g_paths[i];
            if (!rp->moving) continue;
            rp->p_prev = rp->p;
            rp->p = rp->touched ? v3_add(rp->p, v3_scale(rp->v, h)) : v3_lerp(rp->p0, rp->p1, t);
        }
        for (u32 k = 0; k < nb; k++) {
            SoftBody* b = sw->bodies[k];
            if (b->drag > 0.0f) wind_forces(b, sw->wind, h);
            /* öz-çarpışmada bir alt adımda en çok yarıçap kadar ilerle (başka katmanı atlayamasın) */
            f32 vlim = b->self_collide ? b->radius / h : 3.4e38f;
            for (u32 i = 0; i < b->count; i++) {
                b->prev[i] = b->x[i];
                if (b->w[i] == 0.0f) continue;
                b->v[i] = v3_add(b->v[i], v3_scale(sw->gravity, h));
                f32 sp = v3_len(b->v[i]);
                if (sp > vlim) b->v[i] = v3_scale(b->v[i], vlim / sp);
                b->x[i] = v3_add(b->x[i], v3_scale(b->v[i], h));
            }
            solve_constraints(b, inv_h2);
            if (b->self_collide && b->x0) self_collide(sw, b);
        }
        for (u32 k = 0; k < nb; k++) {
            for (u32 j = k + 1; j < nb; j++) soft_pair(sw, sw->bodies[k], sw->bodies[j]);
        }
        for (u32 k = 0; k < nb; k++) {
            SoftBody* b = sw->bodies[k];
            for (u32 c = 0; c < ncand[k]; c++) {
                Cand* cd = &cands[k][c];
                if (!cd->moving) continue;
                Body* rb = &cd->pose;
                const RigidPath* rp = &g_paths[cd->path];
                rb->pos = rp->p;
                rb->rot = quat_nlerp(cd->q0, cd->q1, t);
                rb->rot_m = mat3_from_quat(rb->rot);
                if (rp->touched) {   /* yol ara değerden ayrıldı: kutu = önceki ve şimdiki konumda sınır küre */
                    const Body* real = &pw->bodies[cd->id];
                    f32 e = real->max_extent + PHYS_MARGIN;
                    rb->aabb_min = v3_sub(v3(m_minf(rp->p.x, rp->p_prev.x), m_minf(rp->p.y, rp->p_prev.y), m_minf(rp->p.z, rp->p_prev.z)), v3(e, e, e));
                    rb->aabb_max = v3_add(v3(m_maxf(rp->p.x, rp->p_prev.x), m_maxf(rp->p.y, rp->p_prev.y), m_maxf(rp->p.z, rp->p_prev.z)), v3(e, e, e));
                }
            }
            if (sw->attach_count) solve_attach(sw, pw, k, b, cands[k], ncand[k], h);
            collide_rigid(sw, pw, k, b, cands[k], ncand[k], h);
            if (b->tri_collide) collide_tris(sw, pw, b, cands[k], ncand[k], h);
            f32 damp = m_maxf(0.0f, 1.0f - b->damping * h);
            f32 inv_h = 1.0f / h;
            for (u32 i = 0; i < b->count; i++) {
                if (b->w[i] == 0.0f) { b->v[i] = v3(0, 0, 0); continue; }
                b->v[i] = v3_scale(v3_sub(b->x[i], b->prev[i]), inv_h * damp);
            }
        }
    }
    for (u32 k = 0; k < nb; k++) soft_aabb(sw->bodies[k], sw->bodies[k]->radius);
    /* yakalamaları bırak: gövde yavaşladı, geri döndü ya da artık aday değil */
    u32 kept = 0;
    for (u32 a = 0; a < sw->attach_count; a++) {
        SoftAttach t = sw->attach[a];
        b32 keep = FALSE;
        for (u32 i = 0; i < g_path_count && !keep; i++) {
            const RigidPath* rp = &g_paths[i];
            if (rp->id != t.body) continue;
            keep = v3_len(rp->v) > SOFT_RELEASE && v3_dot(rp->v, t.dir) > 0.0f;
        }
        if (keep) sw->attach[kept++] = t;
    }
    sw->attach_count = kept;
    /* yumuşak gövdeye çarpıp adım içinde yavaşlayan rijit gövdeler: konum yolun sonuna */
    b32 moved = FALSE;
    for (u32 i = 0; i < g_path_count; i++) {
        const RigidPath* rp = &g_paths[i];
        if (!rp->touched || !rp->moving) continue;
        Body* rb = &pw->bodies[rp->id];
        rb->pos = rp->p;
        phys_body_update_derived(rb);
        moved = TRUE;
    }
    if (moved && pw->broadphase == PHYS_BP_BVH) phys_update_tree(pw);
    sw->step_ms = sys_now_ms() - t0;
}
