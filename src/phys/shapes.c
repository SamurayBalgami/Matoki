/* phys/shapes.c — dışbükey gövde oluşturma ve üçgen mesh BVH'si.
 * Hull: O(n⁴) kaba kuvvet (n ≤ 64, yalnızca oluşturmada): her üçlü bir aday düzlem; tüm noktalar
 * bir yanındaysa yüz. Eş düzlemli adaylar birleşir, yüz köşeleri açıya göre CCW sıralanır.
 * Hacim merkezi tetrahedron ayrıştırmasıyla bulunur ve köşeler oraya kaydırılır. */
#include "phys/shapes.h"
#include "core/mem.h"

/* ---- birim küp ---- */

static Vec3 g_cube_verts[8];
static HullFace g_cube_faces[6];
static u32 g_cube_idx[24];
static ConvexHull g_cube;
static b32 g_cube_ready;

const ConvexHull* hull_unit_cube(void) {
    if (g_cube_ready) return &g_cube;
    for (u32 i = 0; i < 8; i++) {
        g_cube_verts[i] = v3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    }
    static const u32 F[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 }, { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const f32 N[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (u32 f = 0; f < 6; f++) {
        g_cube_faces[f].n = v3(N[f][0], N[f][1], N[f][2]);
        g_cube_faces[f].d = 1.0f;
        g_cube_faces[f].first = f * 4;
        g_cube_faces[f].count = 4;
        for (u32 k = 0; k < 4; k++) g_cube_idx[f * 4 + k] = F[f][k];
    }
    g_cube.verts = g_cube_verts;
    g_cube.vcount = 8;
    g_cube.faces = g_cube_faces;
    g_cube.fcount = 6;
    g_cube.idx = g_cube_idx;
    g_cube.icount = 24;
    g_cube.offset = v3(0, 0, 0);
    g_cube.volume = 8.0f;
    g_cube.aabb_min = v3(-1, -1, -1);
    g_cube.aabb_max = v3(1, 1, 1);
    g_cube.inertia = v3(2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f);
    mem_zero(&g_cube.inertia_axes, sizeof(Mat3));
    g_cube.inertia_axes.m[0] = g_cube.inertia_axes.m[4] = g_cube.inertia_axes.m[8] = 1.0f;
    g_cube.inner_radius = 1.0f;
    g_cube.outer_radius = m_sqrtf(3.0f);
    g_cube_ready = TRUE;
    return &g_cube;
}

/* ---- hull ---- */

typedef struct Plane4 {
    Vec3 n;
    f32  d;
    u64  mask;   /* düzlemde (eps içinde) kalan köşeler */
} Plane4;

ConvexHull* hull_create(Arena* a, const Vec3* pts, u32 n) {
    Vec3 p[HULL_MAX_VERTS];
    u32 np = 0;
    Vec3 mn = pts[0], mx = pts[0];
    for (u32 i = 0; i < n; i++) {
        mn = v3(m_minf(mn.x, pts[i].x), m_minf(mn.y, pts[i].y), m_minf(mn.z, pts[i].z));
        mx = v3(m_maxf(mx.x, pts[i].x), m_maxf(mx.y, pts[i].y), m_maxf(mx.z, pts[i].z));
    }
    f32 scale = m_maxf(m_maxf(mx.x - mn.x, mx.y - mn.y), mx.z - mn.z);
    if (!(scale > 0.0f)) return NULL;
    const f32 eps = 1e-4f * scale;
    for (u32 i = 0; i < n; i++) {
        b32 dup = FALSE;
        for (u32 k = 0; k < np && !dup; k++) {
            Vec3 d = v3_sub(pts[i], p[k]);
            dup = v3_dot(d, d) < eps * eps;
        }
        if (dup) continue;
        if (np == HULL_MAX_VERTS) return NULL;
        p[np++] = pts[i];
    }
    if (np < 4) return NULL;

    Plane4 planes[HULL_MAX_FACES];
    u32 nplanes = 0;
    for (u32 i = 0; i < np; i++) {
        for (u32 j = i + 1; j < np; j++) {
            for (u32 k = j + 1; k < np; k++) {
                Vec3 nn = v3_cross(v3_sub(p[j], p[i]), v3_sub(p[k], p[i]));
                f32 len = v3_len(nn);
                if (len < eps * eps) continue;
                nn = v3_scale(nn, 1.0f / len);
                f32 d = v3_dot(nn, p[i]);
                u32 above = 0, below = 0;
                for (u32 q = 0; q < np; q++) {
                    f32 s = v3_dot(nn, p[q]) - d;
                    if (s > eps) above++;
                    else if (s < -eps) below++;
                }
                if (above && below) continue;
                if (above) { nn = v3_scale(nn, -1.0f); d = -d; }
                /* tekilleştirme köşe kümesiyle: neredeyse eş düzlemli 4 nokta iki üçgenden iki
                 * farklı düzlem verse de aynı kümeyi toplar → tek yüz (normal toleransı yetmez) */
                u64 mask = 0;
                for (u32 q = 0; q < np; q++) {
                    if (m_absf(v3_dot(nn, p[q]) - d) <= eps) mask |= (u64)1 << q;
                }
                b32 seen = FALSE;
                for (u32 q = 0; q < nplanes && !seen; q++) seen = planes[q].mask == mask;
                if (seen) continue;
                if (nplanes == HULL_MAX_FACES) return NULL;
                planes[nplanes].n = nn;
                planes[nplanes].d = d;
                planes[nplanes].mask = mask;
                nplanes++;
            }
        }
    }
    if (nplanes < 4) return NULL;

    /* yüz köşeleri (CCW) */
    u32 idx_tmp[HULL_MAX_FACES * HULL_MAX_VERTS / 4];
    u32 icount = 0;
    HullFace faces[HULL_MAX_FACES];
    b32 used[HULL_MAX_VERTS] = { 0 };
    for (u32 f = 0; f < nplanes; f++) {
        u32 list[HULL_MAX_VERTS];
        u32 cnt = 0;
        Vec3 c = v3(0, 0, 0);
        for (u32 q = 0; q < np; q++) {
            if (planes[f].mask & ((u64)1 << q)) {
                list[cnt++] = q;
                c = v3_add(c, p[q]);
            }
        }
        c = v3_scale(c, 1.0f / (f32)cnt);
        /* yüz çokgeni = yüz noktalarının 2B dışbükey zarfı (Andrew monoton zinciri); düzlemin eps'i
         * içinde kalan iç / eş doğrusal noktalar atılır → Euler korunur. (u, w = n × u) tabanında
         * saat yönü tersi = n etrafında CCW. */
        Vec3 u = v3_norm(v3_sub(p[list[0]], c));
        Vec3 w = v3_cross(planes[f].n, u);
        f32 px[HULL_MAX_VERTS], py[HULL_MAX_VERTS];
        for (u32 q = 0; q < cnt; q++) {
            Vec3 r = v3_sub(p[list[q]], c);
            px[q] = v3_dot(r, u);
            py[q] = v3_dot(r, w);
        }
        for (u32 q = 1; q < cnt; q++) {   /* (x, y) sözlüksel ekleme sıralaması */
            u32 li = list[q];
            f32 xv = px[q], yv = py[q];
            u32 r = q;
            while (r > 0 && (px[r - 1] > xv || (px[r - 1] == xv && py[r - 1] > yv))) {
                px[r] = px[r - 1];
                py[r] = py[r - 1];
                list[r] = list[r - 1];
                r--;
            }
            px[r] = xv;
            py[r] = yv;
            list[r] = li;
        }
        u32 hullv[HULL_MAX_VERTS * 2];
        u32 k = 0;
        const f32 ceps = eps * eps;
        for (u32 pass = 0; pass < 2; pass++) {
            u32 base = k;
            for (u32 s = 0; s < cnt; s++) {
                u32 q = pass ? cnt - 1 - s : s;
                while (k >= base + 2) {
                    u32 a0 = hullv[k - 2], a1 = hullv[k - 1];
                    f32 cr = (px[a1] - px[a0]) * (py[q] - py[a0]) - (py[a1] - py[a0]) * (px[q] - px[a0]);
                    if (cr > ceps) break;
                    k--;
                }
                hullv[k++] = q;
            }
            k--;   /* zincirin son noktası diğerinin ilki */
        }
        u32 kept[HULL_MAX_VERTS];
        for (u32 q = 0; q < k; q++) kept[q] = list[hullv[q]];
        cnt = k;
        for (u32 q = 0; q < cnt; q++) list[q] = kept[q];
        if (cnt < 3) return NULL;
        if (icount + cnt > ARRAY_COUNT(idx_tmp)) return NULL;
        faces[f].n = planes[f].n;
        faces[f].first = icount;
        faces[f].count = cnt;
        for (u32 q = 0; q < cnt; q++) {
            idx_tmp[icount++] = list[q];
            used[list[q]] = TRUE;
        }
    }

    /* yalnızca yüzlerde geçen köşeleri tut (iç noktalar atılır) */
    u32 remap[HULL_MAX_VERTS];
    u32 nv = 0;
    for (u32 q = 0; q < np; q++) remap[q] = used[q] ? nv++ : 0xFFFFFFFFu;

    ConvexHull* h = ARENA_NEW(a, ConvexHull);
    h->verts = ARENA_ARRAY(a, Vec3, nv);
    for (u32 q = 0; q < np; q++) {
        if (used[q]) h->verts[remap[q]] = p[q];
    }
    h->vcount = nv;
    h->idx = ARENA_ARRAY(a, u32, icount);
    for (u32 q = 0; q < icount; q++) h->idx[q] = remap[idx_tmp[q]];
    h->icount = icount;
    h->faces = ARENA_ARRAY(a, HullFace, nplanes);
    for (u32 f = 0; f < nplanes; f++) h->faces[f] = faces[f];
    h->fcount = nplanes;

    /* hacim ve hacim merkezi: iç nokta etrafında tetrahedronlar */
    Vec3 inner = v3(0, 0, 0);
    for (u32 q = 0; q < nv; q++) inner = v3_add(inner, h->verts[q]);
    inner = v3_scale(inner, 1.0f / (f32)nv);
    f32 vol = 0.0f;
    Vec3 cen = v3(0, 0, 0);
    for (u32 f = 0; f < h->fcount; f++) {
        const HullFace* hf = &h->faces[f];
        Vec3 a0 = h->verts[h->idx[hf->first]];
        for (u32 q = 1; q + 1 < hf->count; q++) {
            Vec3 b0 = h->verts[h->idx[hf->first + q]], c0 = h->verts[h->idx[hf->first + q + 1]];
            f32 tv = v3_dot(v3_sub(a0, inner), v3_cross(v3_sub(b0, inner), v3_sub(c0, inner))) / 6.0f;
            vol += tv;
            cen = v3_add(cen, v3_scale(v3_add(v3_add(inner, a0), v3_add(b0, c0)), tv * 0.25f));
        }
    }
    if (!(vol > 0.0f)) return NULL;
    cen = v3_scale(cen, 1.0f / vol);
    h->volume = vol;
    h->offset = cen;
    h->aabb_min = v3(3.4e38f, 3.4e38f, 3.4e38f);
    h->aabb_max = v3(-3.4e38f, -3.4e38f, -3.4e38f);
    for (u32 q = 0; q < nv; q++) {
        Vec3 v = v3_sub(h->verts[q], cen);
        h->verts[q] = v;
        h->aabb_min = v3(m_minf(h->aabb_min.x, v.x), m_minf(h->aabb_min.y, v.y), m_minf(h->aabb_min.z, v.z));
        h->aabb_max = v3(m_maxf(h->aabb_max.x, v.x), m_maxf(h->aabb_max.y, v.y), m_maxf(h->aabb_max.z, v.z));
    }
    for (u32 f = 0; f < h->fcount; f++) h->faces[f].d = v3_dot(h->faces[f].n, h->verts[h->idx[h->faces[f].first]]);

    /* eylemsizlik: merkez (artık orijin) etrafındaki tetrahedronların kovaryansı
     * C_tet = det/120 · (aaᵀ + bbᵀ + ccᵀ + (a+b+c)(a+b+c)ᵀ);  I = tr(C)·1 − C; birim kütleye bölünür */
    f32 C[9] = { 0 };
    for (u32 f = 0; f < h->fcount; f++) {
        const HullFace* hf = &h->faces[f];
        Vec3 a0 = h->verts[h->idx[hf->first]];
        for (u32 q = 1; q + 1 < hf->count; q++) {
            Vec3 b0 = h->verts[h->idx[hf->first + q]], c0 = h->verts[h->idx[hf->first + q + 1]];
            f32 det = v3_dot(a0, v3_cross(b0, c0)) / 120.0f;
            Vec3 s = v3_add(v3_add(a0, b0), c0);
            const f32 va[4][3] = { { a0.x, a0.y, a0.z }, { b0.x, b0.y, b0.z }, { c0.x, c0.y, c0.z }, { s.x, s.y, s.z } };
            for (u32 k = 0; k < 4; k++) {
                for (u32 c = 0; c < 3; c++) {
                    for (u32 r = 0; r < 3; r++) C[c * 3 + r] += det * va[k][r] * va[k][c];
                }
            }
        }
    }
    f32 tr = C[0] + C[4] + C[8];
    Mat3 I;
    for (u32 k = 0; k < 9; k++) I.m[k] = -C[k] / vol;
    I.m[0] += tr / vol;
    I.m[4] += tr / vol;
    I.m[8] += tr / vol;
    mat3_sym_eigen(&I, &h->inertia_axes, &h->inertia);
    h->inner_radius = 3.4e38f;
    h->outer_radius = 0.0f;
    for (u32 f = 0; f < h->fcount; f++) h->inner_radius = m_minf(h->inner_radius, h->faces[f].d);
    for (u32 q = 0; q < nv; q++) h->outer_radius = m_maxf(h->outer_radius, v3_len(h->verts[q]));
    return h;
}

void mat3_sym_eigen(const Mat3* s, Mat3* q, Vec3* eig) {
    f32 a[3][3], v[3][3];
    for (u32 r = 0; r < 3; r++) {
        for (u32 c = 0; c < 3; c++) {
            a[r][c] = s->m[c * 3 + r];
            v[r][c] = r == c ? 1.0f : 0.0f;
        }
    }
    static const u32 P[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
    for (u32 sweep = 0; sweep < 16; sweep++) {
        f32 off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        f32 dia = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
        if (off <= 1e-14f * dia) break;
        for (u32 k = 0; k < 3; k++) {
            u32 p = P[k][0], r = P[k][1];
            if (a[p][r] == 0.0f) continue;
            /* Numerical Recipes: θ = (a_rr − a_pp) / 2a_pr, t = sgn θ / (|θ| + √(θ² + 1)) */
            f32 th = (a[r][r] - a[p][p]) / (2.0f * a[p][r]);
            f32 t = m_absf(th) > 1e15f ? 0.5f / th : (th >= 0.0f ? 1.0f : -1.0f) / (m_absf(th) + m_sqrtf(th * th + 1.0f));
            f32 c = 1.0f / m_sqrtf(t * t + 1.0f), sn = t * c;
            for (u32 i = 0; i < 3; i++) {
                f32 ip = a[i][p], ir = a[i][r];
                a[i][p] = c * ip - sn * ir;
                a[i][r] = sn * ip + c * ir;
            }
            for (u32 i = 0; i < 3; i++) {
                f32 pi = a[p][i], ri = a[r][i];
                a[p][i] = c * pi - sn * ri;
                a[r][i] = sn * pi + c * ri;
            }
            a[p][r] = a[r][p] = 0.0f;
            for (u32 i = 0; i < 3; i++) {
                f32 ip = v[i][p], ir = v[i][r];
                v[i][p] = c * ip - sn * ir;
                v[i][r] = sn * ip + c * ir;
            }
        }
    }
    f32 det = v[0][0] * (v[1][1] * v[2][2] - v[2][1] * v[1][2]) - v[0][1] * (v[1][0] * v[2][2] - v[2][0] * v[1][2]) +
              v[0][2] * (v[1][0] * v[2][1] - v[2][0] * v[1][1]);
    if (det < 0.0f) for (u32 i = 0; i < 3; i++) v[i][2] = -v[i][2];
    for (u32 r = 0; r < 3; r++) {
        for (u32 c = 0; c < 3; c++) q->m[c * 3 + r] = v[r][c];
    }
    *eig = v3(a[0][0], a[1][1], a[2][2]);
}

/* ---- üçgen mesh + BVH ---- */

typedef struct BvhBuild {
    TriMesh* m;
    u32*     order;
    Vec3*    cent;
    Vec3*    tmn;
    Vec3*    tmx;
    u32      next;   /* sıradaki boş düğüm */
} BvhBuild;

static void bvh_build(BvhBuild* b, u32 node, u32 first, u32 count) {
    TriMesh* m = b->m;
    BvhNode* nd = &m->nodes[node];
    Vec3 mn = v3(3.4e38f, 3.4e38f, 3.4e38f), mx = v3(-3.4e38f, -3.4e38f, -3.4e38f);
    Vec3 cmn = mn, cmx = mx;
    for (u32 i = first; i < first + count; i++) {
        u32 t = b->order[i];
        mn = v3(m_minf(mn.x, b->tmn[t].x), m_minf(mn.y, b->tmn[t].y), m_minf(mn.z, b->tmn[t].z));
        mx = v3(m_maxf(mx.x, b->tmx[t].x), m_maxf(mx.y, b->tmx[t].y), m_maxf(mx.z, b->tmx[t].z));
        Vec3 c = b->cent[t];
        cmn = v3(m_minf(cmn.x, c.x), m_minf(cmn.y, c.y), m_minf(cmn.z, c.z));
        cmx = v3(m_maxf(cmx.x, c.x), m_maxf(cmx.y, c.y), m_maxf(cmx.z, c.z));
    }
    nd->mn = mn;
    nd->mx = mx;
    if (count <= 4) {
        nd->first = first;
        nd->count = count;
        nd->left = 0;
        return;
    }
    Vec3 ext = v3_sub(cmx, cmn);
    u32 axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    f32 split = axis == 0 ? (cmn.x + cmx.x) * 0.5f : axis == 1 ? (cmn.y + cmx.y) * 0.5f : (cmn.z + cmx.z) * 0.5f;
    u32 i = first, j = first + count;
    while (i < j) {
        Vec3 c = b->cent[b->order[i]];
        f32 v = axis == 0 ? c.x : axis == 1 ? c.y : c.z;
        if (v < split) i++;
        else {
            j--;
            u32 t = b->order[i]; b->order[i] = b->order[j]; b->order[j] = t;
        }
    }
    u32 left_count = i - first;
    if (left_count == 0 || left_count == count) left_count = count / 2;   /* hepsi aynı merkezde */
    u32 l = b->next;
    b->next += 2;
    nd->count = 0;
    nd->first = 0;
    nd->left = l;
    bvh_build(b, l, first, left_count);
    bvh_build(b, l + 1, first + left_count, count - left_count);
}

/* Kenar komşuluğu (açık adresli tablo, anahtar = sıralı köşe çifti) ve aktif kenar bayrakları.
 * Aktif: komşusuz, ikiden çok üçgenli, ters sarımlı komşulu ya da belirgin dışbükey kenar.
 * Pasif: içbükey ya da neredeyse düz kenar → o kenardaki temas komşu yüzlerce zaten taşınır. */
static void trimesh_edges(TriMesh* m, Arena* a) {
    u32 ne = m->tri_count * 3;
    u32 cap = 1;
    while (cap < ne * 2) cap <<= 1;
    u32* table = ARENA_ARRAY(a, u32, cap);   /* 0 = boş, aksi halde kenar + 1 */
    u32* other = ARENA_ARRAY(a, u32, ne);    /* komşu kenar + 1; 0 yok; 0xFFFFFFFF çoklu */
    for (u32 e = 0; e < ne; e++) {
        u32 i0 = m->idx[e], i1 = m->idx[(e / 3) * 3 + (e % 3 + 1) % 3];
        u32 lo = i0 < i1 ? i0 : i1, hi = i0 < i1 ? i1 : i0;
        u32 h = ((lo * 73856093u) ^ (hi * 19349663u)) & (cap - 1);
        for (;;) {
            if (!table[h]) { table[h] = e + 1; break; }
            u32 f = table[h] - 1;
            u32 j0 = m->idx[f], j1 = m->idx[(f / 3) * 3 + (f % 3 + 1) % 3];
            if ((j0 == lo && j1 == hi) || (j0 == hi && j1 == lo)) {
                /* aynı kenar: ilk kayıt f; ikinci gelişte eşleştir, üçüncüde çoklu */
                if (other[f] == 0) { other[f] = e + 1; other[e] = f + 1; }
                else { if (other[f] != 0xFFFFFFFFu) other[other[f] - 1] = 0xFFFFFFFFu; other[f] = 0xFFFFFFFFu; other[e] = 0xFFFFFFFFu; }
                break;
            }
            h = (h + 1) & (cap - 1);
        }
    }
    for (u32 t = 0; t < m->tri_count; t++) {
        Vec3 p0, p1, p2;
        trimesh_tri(m, t, &p0, &p1, &p2);
        Vec3 n = v3_cross(v3_sub(p1, p0), v3_sub(p2, p0));
        f32 nl = v3_len(n);
        u8 flags = 0;
        for (u32 k = 0; k < 3; k++) {
            u32 e = t * 3 + k;
            if (other[e] == 0 || other[e] == 0xFFFFFFFFu || !(nl > 0.0f)) { flags |= (u8)(1u << k); continue; }
            u32 f = other[e] - 1, ft = f / 3;
            /* tutarlı sarım: komşuda kenar ters yönde geçer */
            if (m->idx[f] != m->idx[(e / 3) * 3 + (k + 1) % 3]) { flags |= (u8)(1u << k); continue; }
            Vec3 q0, q1, q2;
            trimesh_tri(m, ft, &q0, &q1, &q2);
            Vec3 n2 = v3_cross(v3_sub(q1, q0), v3_sub(q2, q0));
            f32 n2l = v3_len(n2);
            if (!(n2l > 0.0f)) { flags |= (u8)(1u << k); continue; }
            Vec3 opp = (f % 3) == 0 ? q2 : ((f % 3) == 1 ? q0 : q1);   /* komşunun karşı köşesi */
            b32 convex = v3_dot(n, v3_sub(opp, p0)) < 0.0f;
            if (convex && v3_dot(n, n2) / (nl * n2l) < TRIMESH_ACTIVE_EDGE_COS) flags |= (u8)(1u << k);
        }
        m->edge_flags[t] = flags;
    }
}

TriMesh* trimesh_create(Arena* a, const Vec3* verts, u32 vcount, const u32* idx, u32 tri_count) {
    TriMesh* m = ARENA_NEW(a, TriMesh);
    m->verts = ARENA_ARRAY(a, Vec3, vcount);
    mem_copy(m->verts, verts, sizeof(Vec3) * vcount);
    m->vcount = vcount;
    m->tri_count = tri_count;
    m->idx = ARENA_ARRAY(a, u32, tri_count * 3);
    m->nodes = ARENA_ARRAY(a, BvhNode, tri_count * 2 + 1);
    m->edge_flags = ARENA_ARRAY(a, u8, tri_count);
    usize mark = arena_mark(a);   /* bundan sonrası geçici */
    BvhBuild b;
    b.m = m;
    b.order = ARENA_ARRAY(a, u32, tri_count);
    b.cent = ARENA_ARRAY(a, Vec3, tri_count);
    b.tmn = ARENA_ARRAY(a, Vec3, tri_count);
    b.tmx = ARENA_ARRAY(a, Vec3, tri_count);
    for (u32 t = 0; t < tri_count; t++) {
        Vec3 p0 = verts[idx[t * 3]], p1 = verts[idx[t * 3 + 1]], p2 = verts[idx[t * 3 + 2]];
        b.order[t] = t;
        b.cent[t] = v3_scale(v3_add(v3_add(p0, p1), p2), 1.0f / 3.0f);
        b.tmn[t] = v3(m_minf(p0.x, m_minf(p1.x, p2.x)), m_minf(p0.y, m_minf(p1.y, p2.y)), m_minf(p0.z, m_minf(p1.z, p2.z)));
        b.tmx[t] = v3(m_maxf(p0.x, m_maxf(p1.x, p2.x)), m_maxf(p0.y, m_maxf(p1.y, p2.y)), m_maxf(p0.z, m_maxf(p1.z, p2.z)));
    }
    b.next = 1;
    if (tri_count) bvh_build(&b, 0, 0, tri_count);
    m->node_count = tri_count ? b.next : 0;
    for (u32 t = 0; t < tri_count; t++) {
        u32 s = b.order[t];
        m->idx[t * 3] = idx[s * 3];
        m->idx[t * 3 + 1] = idx[s * 3 + 1];
        m->idx[t * 3 + 2] = idx[s * 3 + 2];
    }
    m->aabb_min = tri_count ? m->nodes[0].mn : v3(0, 0, 0);
    m->aabb_max = tri_count ? m->nodes[0].mx : v3(0, 0, 0);
    trimesh_edges(m, a);
    arena_reset(a, mark);
    return m;
}

static inline b32 box_overlap(Vec3 amn, Vec3 amx, Vec3 bmn, Vec3 bmx) {
    return amn.x <= bmx.x && amx.x >= bmn.x && amn.y <= bmx.y && amx.y >= bmn.y && amn.z <= bmx.z && amx.z >= bmn.z;
}

u32 trimesh_query_aabb(const TriMesh* m, Vec3 mn, Vec3 mx, u32* out, u32 max) {
    if (!m->node_count) return 0;
    u32 stack[64];
    u32 sp = 0, n = 0;
    stack[sp++] = 0;
    while (sp) {
        const BvhNode* nd = &m->nodes[stack[--sp]];
        if (!box_overlap(nd->mn, nd->mx, mn, mx)) continue;
        if (nd->count) {
            for (u32 i = 0; i < nd->count; i++) {
                u32 t = nd->first + i;
                Vec3 a, b, c;
                trimesh_tri(m, t, &a, &b, &c);
                Vec3 tmn = v3(m_minf(a.x, m_minf(b.x, c.x)), m_minf(a.y, m_minf(b.y, c.y)), m_minf(a.z, m_minf(b.z, c.z)));
                Vec3 tmx = v3(m_maxf(a.x, m_maxf(b.x, c.x)), m_maxf(a.y, m_maxf(b.y, c.y)), m_maxf(a.z, m_maxf(b.z, c.z)));
                if (box_overlap(tmn, tmx, mn, mx) && n < max) out[n++] = t;
            }
        } else if (sp + 2 <= ARRAY_COUNT(stack)) {
            stack[sp++] = nd->left;
            stack[sp++] = nd->left + 1;
        }
    }
    return n;
}

static b32 ray_box_t(Vec3 o, Vec3 inv, Vec3 mn, Vec3 mx, f32 tmax) {
    f32 t0 = 0.0f, t1 = tmax;
    const f32 oo[3] = { o.x, o.y, o.z }, id[3] = { inv.x, inv.y, inv.z };
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

/* Möller–Trumbore, çift yüzlü */
static b32 ray_tri(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, f32 tmax, f32* t) {
    Vec3 e1 = v3_sub(b, a), e2 = v3_sub(c, a);
    Vec3 p = v3_cross(d, e2);
    f32 det = v3_dot(e1, p);
    if (m_absf(det) < 1e-12f) return FALSE;
    f32 inv = 1.0f / det;
    Vec3 s = v3_sub(o, a);
    f32 u = v3_dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return FALSE;
    Vec3 q = v3_cross(s, e1);
    f32 v = v3_dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return FALSE;
    f32 tt = v3_dot(e2, q) * inv;
    if (tt < 0.0f || tt > tmax) return FALSE;
    *t = tt;
    return TRUE;
}

b32 trimesh_raycast(const TriMesh* m, Vec3 o, Vec3 d, f32 tmax, f32* t_out, Vec3* n_out, u32* tri_out) {
    if (!m->node_count) return FALSE;
    Vec3 inv = v3(d.x != 0.0f ? 1.0f / d.x : 3.4e38f, d.y != 0.0f ? 1.0f / d.y : 3.4e38f, d.z != 0.0f ? 1.0f / d.z : 3.4e38f);
    u32 stack[64];
    u32 sp = 0;
    stack[sp++] = 0;
    f32 best = tmax;
    b32 hit = FALSE;
    while (sp) {
        const BvhNode* nd = &m->nodes[stack[--sp]];
        if (!ray_box_t(o, inv, nd->mn, nd->mx, best)) continue;
        if (nd->count) {
            for (u32 i = 0; i < nd->count; i++) {
                u32 tri = nd->first + i;
                Vec3 a, b, c;
                trimesh_tri(m, tri, &a, &b, &c);
                f32 t;
                if (ray_tri(o, d, a, b, c, best, &t)) {
                    best = t;
                    hit = TRUE;
                    Vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
                    *n_out = v3_dot(n, d) > 0.0f ? v3_scale(n, -1.0f) : n;
                    *tri_out = tri;
                }
            }
        } else if (sp + 2 <= ARRAY_COUNT(stack)) {
            stack[sp++] = nd->left;
            stack[sp++] = nd->left + 1;
        }
    }
    if (hit) *t_out = best;
    return hit;
}
