/* gfx3d/cloth_mesh.c — kumaş ızgarasının Catmull-Rom yüzeyi (D58).
 * Önce her parçacık satırı u yönünde sıklaştırılır (konum + ∂u), sonra sütunlar v yönünde (konum, ∂v ve ∂u'nun
 * v-ağırlıklı toplamı). Ağırlıklar alt aralık başına bir kez hesaplanır. Normal = normalize(∂u × ∂v): fiziğin üçgen sarımıyla
 * (p00, p10, p11) aynı yön. Kenar parçacığının dışı yansıtılır: uçta yüzey düz devam eder, sarkmaz. */
#include "gfx3d/cloth_mesh.h"
#include "core/mem.h"
#include "core/sys.h"

/* Catmull-Rom: P(t) = Σ w_k(t) · P[i − 1 + k], t ∈ [0, 1] */
static void cr_weights(f32 t, f32 w[4], f32 dw[4]) {
    f32 t2 = t * t, t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
    w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
    dw[0] = 0.5f * (-3.0f * t2 + 4.0f * t - 1.0f);
    dw[1] = 0.5f * (9.0f * t2 - 10.0f * t);
    dw[2] = 0.5f * (-9.0f * t2 + 8.0f * t + 1.0f);
    dw[3] = 0.5f * (3.0f * t2 - 2.0f * t);
}

ClothMesh* cloth_mesh_alloc(Arena* a, u32 nu, u32 nv, u32 r, f32 u_len, f32 v_len) {
    if (nu < 2 || nv < 2) sys_panic("cloth_mesh_alloc: en az 2×2");
    if (r < 1) r = 1;
    if (r > CLOTH_MESH_MAX_R) r = CLOTH_MESH_MAX_R;
    ClothMesh* c = ARENA_NEW(a, ClothMesh);
    c->nu = nu;
    c->nv = nv;
    c->r = r;
    c->mu = (nu - 1) * r + 1;
    c->mv = (nv - 1) * r + 1;
    u32 n = c->mu * c->mv;
    c->mesh = mesh_alloc(a, n * 2, (c->mu - 1) * (c->mv - 1) * 12);
    c->row_p = ARENA_ARRAY(a, Vec3, nv * c->mu);
    c->row_du = ARENA_ARRAY(a, Vec3, nv * c->mu);
    for (u32 k = 0; k <= r; k++) cr_weights((f32)k / (f32)r, c->w[k], c->dw[k]);
    Mesh* m = c->mesh;
    u32* ix = m->indices;
    u32 q = (c->mu - 1) * (c->mv - 1) * 6;
    for (u32 j = 0; j + 1 < c->mv; j++) {
        for (u32 i = 0; i + 1 < c->mu; i++) {
            u32 p00 = j * c->mu + i, p10 = p00 + 1, p01 = p00 + c->mu, p11 = p01 + 1;
            ix[0] = p00; ix[1] = p10; ix[2] = p11;
            ix[3] = p00; ix[4] = p11; ix[5] = p01;
            /* arka yüz: ters sarım, ikinci kopya */
            ix[q + 0] = p00 + n; ix[q + 1] = p11 + n; ix[q + 2] = p10 + n;
            ix[q + 3] = p00 + n; ix[q + 4] = p01 + n; ix[q + 5] = p11 + n;
            ix += 6;
        }
    }
    for (u32 j = 0; j < c->mv; j++) {
        for (u32 i = 0; i < c->mu; i++) {
            u32 k = j * c->mu + i;
            m->u[k] = m->u[k + n] = (f32)i / (f32)(c->mu - 1) * u_len;
            m->v[k] = m->v[k + n] = (f32)j / (f32)(c->mv - 1) * v_len;
        }
    }
    return c;
}

/* p[i − 1 .. i + 2] kenarda yansıtılarak */
static void gather4(const Vec3* p, u32 stride, u32 count, u32 i, Vec3 out[4]) {
    out[1] = p[i * stride];
    out[2] = p[(i + 1) * stride];
    out[0] = i > 0 ? p[(i - 1) * stride] : v3_sub(v3_scale(out[1], 2.0f), out[2]);
    out[3] = i + 2 < count ? p[(i + 2) * stride] : v3_sub(v3_scale(out[2], 2.0f), out[1]);
}

static Vec3 comb4(const Vec3 p[4], const f32 w[4]) {
    return v3_add(v3_add(v3_scale(p[0], w[0]), v3_scale(p[1], w[1])), v3_add(v3_scale(p[2], w[2]), v3_scale(p[3], w[3])));
}

/* çizim ızgarasının a. noktası → (aralık, alt adım); son nokta son aralığın t = 1'i */
static void split(u32 a, u32 r, u32 count, u32* seg, u32* k) {
    u32 s = a / r;
    if (s >= count - 1) s = count - 2;
    *seg = s;
    *k = a - s * r;
}

void cloth_mesh_update(ClothMesh* c, const Vec3* x) {
    Mesh* m = c->mesh;
    u32 n = c->mu * c->mv;
    /* satırlar: u yönünde konum ve ∂u */
    for (u32 j = 0; j < c->nv; j++) {
        const Vec3* row = x + j * c->nu;
        for (u32 a = 0; a < c->mu; a++) {
            u32 s, k;
            split(a, c->r, c->nu, &s, &k);
            Vec3 p[4];
            gather4(row, 1, c->nu, s, p);
            c->row_p[j * c->mu + a] = comb4(p, c->w[k]);
            c->row_du[j * c->mu + a] = comb4(p, c->dw[k]);
        }
    }
    /* sütunlar: v yönünde konum, ∂v; ∂u satır türevlerinin v-ağırlıklı toplamı */
    for (u32 b = 0; b < c->mv; b++) {
        u32 s, k;
        split(b, c->r, c->nv, &s, &k);
        for (u32 a = 0; a < c->mu; a++) {
            Vec3 p[4], d[4];
            gather4(c->row_p + a, c->mu, c->nv, s, p);
            gather4(c->row_du + a, c->mu, c->nv, s, d);
            Vec3 pos = comb4(p, c->w[k]);
            Vec3 dv = comb4(p, c->dw[k]);
            Vec3 du = comb4(d, c->w[k]);
            Vec3 nn = v3_cross(du, dv);
            f32 l = v3_len(nn);
            nn = l > 1e-12f ? v3_scale(nn, 1.0f / l) : v3(0, 1, 0);
            u32 i = b * c->mu + a;
            m->px[i] = m->px[i + n] = pos.x;
            m->py[i] = m->py[i + n] = pos.y;
            m->pz[i] = m->pz[i + n] = pos.z;
            m->nx[i] = nn.x; m->ny[i] = nn.y; m->nz[i] = nn.z;
            m->nx[i + n] = -nn.x; m->ny[i + n] = -nn.y; m->nz[i + n] = -nn.z;
        }
    }
    mesh_compute_bounds(m);
}
