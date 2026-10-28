/* phys/solver_simd_wasm.c — soft step temas çözücüsünün wasm SIMD yolu: lane = manifold (aynı renkten 4).
 * Gövde durumu Body'deki 128 baytlık bloktan 8 yükleme + 4×4 devrikle toplanır, işlenip yalnızca dinamik
 * gövdelere geri yazılır. Kısıt verisi adım başında SoA "geniş" gruplara kopyalanır (WideContact).
 * İşlem sırası world.c'deki warm_manifold / solve_manifold ile BİREBİR aynı (FMA yok, max = pmax, seçimler
 * bitselect) → skaler renk sırasıyla bit-bit aynı sonuç (D9/D11/D20). */
#include <wasm_simd128.h>
#include "phys/solver.h"
#include "core/mem.h"
#include "core/sys.h"

_Static_assert(__builtin_offsetof(Body, vel) % 16 == 0, "çözücü bloğu hizası");
_Static_assert(__builtin_offsetof(Body, inv_mass) == __builtin_offsetof(Body, vel) + 12, "blok: inv_mass");
_Static_assert(__builtin_offsetof(Body, ang_vel) == __builtin_offsetof(Body, vel) + 16, "blok: ang_vel");
_Static_assert(__builtin_offsetof(Body, pos) == __builtin_offsetof(Body, vel) + 32, "blok: pos");
_Static_assert(__builtin_offsetof(Body, inv_inertia_world) == __builtin_offsetof(Body, vel) + 48, "blok: I");
_Static_assert(__builtin_offsetof(Body, rot_m) == __builtin_offsetof(Body, vel) + 84, "blok: R");
_Static_assert(__builtin_offsetof(Body, pad2) + 8 == __builtin_offsetof(Body, vel) + 128, "blok: 128 bayt");

typedef union F4 {
    v128_t v;
    f32    f[4];
    u32    u[4];
} F4;

typedef struct WidePoint {
    F4 la[3], lb[3], ra[3], rb[3];
    F4 s0, mass_n, mass_t1, mass_t2;
    F4 jn, jt1, jt2, max_jn;
    F4 mask;   /* lane'de bu nokta var: tüm bitler 1 */
} WidePoint;

typedef struct WideContact {
    u32 a[4], b[4], m[4];
    u32 max_count, pad[3];
    F4  n[3], t1[3], t2[3], friction;
    WidePoint p[PHYS_MAX_POINTS];
} WideContact;

usize solver_simd_bytes(u32 cap) { return sizeof(WideContact) * (usize)cap; }

/* boş lane'ler için sıfır gövde bloğu */
static _Alignas(16) f32 g_zero_block[32];

/* ---- kurulum ve geri yazma ---- */

static void set3(F4* d, u32 l, Vec3 v) { d[0].f[l] = v.x; d[1].f[l] = v.y; d[2].f[l] = v.z; }

void solver_simd_prepare(PhysWorld* w) {
    WideContact* wc = (WideContact*)w->wide;
    u32 g = 0;
    for (u32 c = 0; c < PHYS_COLORS; c++) {
        w->wide_color_start[c] = g;
        for (u32 k = w->color_start[c]; k < w->color_start[c + 1]; k += 4) {
            if (g >= w->wide_cap) sys_panic("phys: SIMD grup kapasitesi");
            WideContact* x = &wc[g++];
            mem_zero(x, sizeof(*x));
            for (u32 l = 0; l < 4; l++) {
                if (k + l >= w->color_start[c + 1]) {
                    x->a[l] = x->b[l] = x->m[l] = PHYS_NO_BODY;
                    continue;
                }
                u32 mi = w->color_order[k + l];
                const Manifold* m = &w->cur[mi];
                x->a[l] = m->a;
                x->b[l] = m->b;
                x->m[l] = mi;
                if (m->count > x->max_count) x->max_count = m->count;
                set3(x->n, l, m->normal);
                set3(x->t1, l, m->t1);
                set3(x->t2, l, m->t2);
                x->friction.f[l] = m->friction;
                for (u32 i = 0; i < m->count; i++) {
                    const ContactPoint* cp = &m->p[i];
                    WidePoint* p = &x->p[i];
                    set3(p->la, l, cp->la);
                    set3(p->lb, l, cp->lb);
                    set3(p->ra, l, cp->ra);
                    set3(p->rb, l, cp->rb);
                    p->s0.f[l] = cp->s0;
                    p->mass_n.f[l] = cp->mass_n;
                    p->mass_t1.f[l] = cp->mass_t1;
                    p->mass_t2.f[l] = cp->mass_t2;
                    p->jn.f[l] = cp->jn;
                    p->jt1.f[l] = cp->jt1;
                    p->jt2.f[l] = cp->jt2;
                    p->max_jn.f[l] = cp->max_jn;
                    p->mask.u[l] = 0xFFFFFFFFu;
                }
            }
        }
    }
    w->wide_color_start[PHYS_COLORS] = g;
    w->wide_count = g;
}

void solver_simd_store(PhysWorld* w) {
    WideContact* wc = (WideContact*)w->wide;
    for (u32 g = 0; g < w->wide_count; g++) {
        const WideContact* x = &wc[g];
        for (u32 l = 0; l < 4; l++) {
            if (x->m[l] == PHYS_NO_BODY) continue;
            Manifold* m = &w->cur[x->m[l]];
            for (u32 i = 0; i < m->count; i++) {
                ContactPoint* cp = &m->p[i];
                cp->jn = x->p[i].jn.f[l];
                cp->jt1 = x->p[i].jt1.f[l];
                cp->jt2 = x->p[i].jt2.f[l];
                cp->max_jn = x->p[i].max_jn.f[l];
            }
        }
    }
}

/* ---- gövde toplama / dağıtma ---- */

typedef struct WBody {
    v128_t v[3], im, w[3], wpad, p[3], I[9], R[9];
} WBody;

static inline void transpose4(v128_t* a, v128_t* b, v128_t* c, v128_t* d) {
    v128_t t0 = wasm_i32x4_shuffle(*a, *b, 0, 4, 1, 5);
    v128_t t1 = wasm_i32x4_shuffle(*c, *d, 0, 4, 1, 5);
    v128_t t2 = wasm_i32x4_shuffle(*a, *b, 2, 6, 3, 7);
    v128_t t3 = wasm_i32x4_shuffle(*c, *d, 2, 6, 3, 7);
    *a = wasm_i32x4_shuffle(t0, t1, 0, 1, 4, 5);
    *b = wasm_i32x4_shuffle(t0, t1, 2, 3, 6, 7);
    *c = wasm_i32x4_shuffle(t2, t3, 0, 1, 4, 5);
    *d = wasm_i32x4_shuffle(t2, t3, 2, 3, 6, 7);
}

static inline const f32* block_of(const PhysWorld* w, u32 i) {
    return i == PHYS_NO_BODY ? g_zero_block : (const f32*)&w->bodies[i].vel;
}

static void gather(const PhysWorld* w, const u32 idx[4], WBody* o) {
    const f32* p[4] = { block_of(w, idx[0]), block_of(w, idx[1]), block_of(w, idx[2]), block_of(w, idx[3]) };
    v128_t r[8][4];
    for (u32 k = 0; k < 8; k++) {
        for (u32 l = 0; l < 4; l++) r[k][l] = wasm_v128_load(p[l] + 4 * k);
        transpose4(&r[k][0], &r[k][1], &r[k][2], &r[k][3]);
    }
    o->v[0] = r[0][0]; o->v[1] = r[0][1]; o->v[2] = r[0][2]; o->im = r[0][3];
    o->w[0] = r[1][0]; o->w[1] = r[1][1]; o->w[2] = r[1][2]; o->wpad = r[1][3];
    o->p[0] = r[2][0]; o->p[1] = r[2][1]; o->p[2] = r[2][2];
    o->I[0] = r[3][0]; o->I[1] = r[3][1]; o->I[2] = r[3][2]; o->I[3] = r[3][3];
    o->I[4] = r[4][0]; o->I[5] = r[4][1]; o->I[6] = r[4][2]; o->I[7] = r[4][3];
    o->I[8] = r[5][0]; o->R[0] = r[5][1]; o->R[1] = r[5][2]; o->R[2] = r[5][3];
    o->R[3] = r[6][0]; o->R[4] = r[6][1]; o->R[5] = r[6][2]; o->R[6] = r[6][3];
    o->R[7] = r[7][0]; o->R[8] = r[7][1];
}

/* yalnızca dinamik gövdeler (renk içinde tekil); statikler değişmedi sayılır (skalerde ×0 itki) */
static void scatter(PhysWorld* w, const u32 idx[4], const WBody* o) {
    v128_t a0 = o->v[0], a1 = o->v[1], a2 = o->v[2], a3 = o->im;
    v128_t b0 = o->w[0], b1 = o->w[1], b2 = o->w[2], b3 = o->wpad;
    transpose4(&a0, &a1, &a2, &a3);
    transpose4(&b0, &b1, &b2, &b3);
    const v128_t va[4] = { a0, a1, a2, a3 }, vb[4] = { b0, b1, b2, b3 };
    for (u32 l = 0; l < 4; l++) {
        if (idx[l] == PHYS_NO_BODY) continue;
        Body* b = &w->bodies[idx[l]];
        if (!(b->inv_mass > 0.0f)) continue;
        f32* p = (f32*)&b->vel;
        wasm_v128_store(p, va[l]);
        wasm_v128_store(p + 4, vb[l]);
    }
}

/* ---- skalerle aynı sırada vektör yardımcıları ---- */

#define ADD wasm_f32x4_add
#define SUB wasm_f32x4_sub
#define MUL wasm_f32x4_mul
#define SEL(m, a, b) wasm_v128_bitselect((a), (b), (m))   /* m ? a : b */

/* mat3_mul_v3 (sütun-öncelikli): x = (a0·x + a3·y) + a6·z */
static inline void mat_mul(const v128_t* a, const v128_t* x, v128_t* o) {
    o[0] = ADD(ADD(MUL(a[0], x[0]), MUL(a[3], x[1])), MUL(a[6], x[2]));
    o[1] = ADD(ADD(MUL(a[1], x[0]), MUL(a[4], x[1])), MUL(a[7], x[2]));
    o[2] = ADD(ADD(MUL(a[2], x[0]), MUL(a[5], x[1])), MUL(a[8], x[2]));
}

static inline void cross(const v128_t* a, const v128_t* b, v128_t* o) {
    o[0] = SUB(MUL(a[1], b[2]), MUL(a[2], b[1]));
    o[1] = SUB(MUL(a[2], b[0]), MUL(a[0], b[2]));
    o[2] = SUB(MUL(a[0], b[1]), MUL(a[1], b[0]));
}

static inline v128_t dot(const v128_t* a, const v128_t* b) {
    return ADD(ADD(MUL(a[0], b[0]), MUL(a[1], b[1])), MUL(a[2], b[2]));
}

static inline void ld3(const F4* s, v128_t* d) { d[0] = s[0].v; d[1] = s[1].v; d[2] = s[2].v; }

/* point_vel(B, rb) − point_vel(A, ra) */
static inline void rel_vel(const WBody* A, const WBody* B, const v128_t* ra, const v128_t* rb, v128_t* dv) {
    v128_t ca[3], cb[3];
    cross(B->w, rb, cb);
    cross(A->w, ra, ca);
    for (u32 k = 0; k < 3; k++) dv[k] = SUB(ADD(B->v[k], cb[k]), ADD(A->v[k], ca[k]));
}

/* apply_impulse: A −= P, B += P */
static inline void apply(WBody* A, WBody* B, const v128_t* ra, const v128_t* rb, const v128_t* P) {
    v128_t c[3], t[3];
    for (u32 k = 0; k < 3; k++) A->v[k] = SUB(A->v[k], MUL(P[k], A->im));
    cross(ra, P, c);
    mat_mul(A->I, c, t);
    for (u32 k = 0; k < 3; k++) A->w[k] = SUB(A->w[k], t[k]);
    for (u32 k = 0; k < 3; k++) B->v[k] = ADD(B->v[k], MUL(P[k], B->im));
    cross(rb, P, c);
    mat_mul(B->I, c, t);
    for (u32 k = 0; k < 3; k++) B->w[k] = ADD(B->w[k], t[k]);
}

/* maskesiz lane'lerde gövdeyi noktadan önceki haline döndür */
static inline void keep(v128_t mask, WBody* A, WBody* B, const WBody* A0, const WBody* B0) {
    for (u32 k = 0; k < 3; k++) {
        A->v[k] = SEL(mask, A->v[k], A0->v[k]);
        A->w[k] = SEL(mask, A->w[k], A0->w[k]);
        B->v[k] = SEL(mask, B->v[k], B0->v[k]);
        B->w[k] = SEL(mask, B->w[k], B0->w[k]);
    }
}

void solver_simd_warm_start(PhysWorld* w) {
    WideContact* wc = (WideContact*)w->wide;
    for (u32 g = 0; g < w->wide_count; g++) {
        WideContact* x = &wc[g];
        WBody A, B;
        gather(w, x->a, &A);
        gather(w, x->b, &B);
        v128_t n[3], t1[3], t2[3];
        ld3(x->n, n);
        ld3(x->t1, t1);
        ld3(x->t2, t2);
        for (u32 i = 0; i < x->max_count; i++) {
            WidePoint* p = &x->p[i];
            WBody A0 = A, B0 = B;
            v128_t ra[3], rb[3], P[3];
            ld3(p->ra, ra);
            ld3(p->rb, rb);
            /* P = n·jn + (t1·jt1 + t2·jt2) */
            for (u32 k = 0; k < 3; k++) P[k] = ADD(MUL(n[k], p->jn.v), ADD(MUL(t1[k], p->jt1.v), MUL(t2[k], p->jt2.v)));
            apply(&A, &B, ra, rb, P);
            keep(p->mask.v, &A, &B, &A0, &B0);
        }
        scatter(w, x->a, &A);
        scatter(w, x->b, &B);
    }
}

void solver_simd_solve(PhysWorld* w, f32 inv_h, Softness soft, b32 use_bias) {
    WideContact* wc = (WideContact*)w->wide;
    const v128_t zero = wasm_f32x4_splat(0.0f), one = wasm_f32x4_splat(1.0f);
    const v128_t vinv_h = wasm_f32x4_splat(inv_h);
    const v128_t rate = wasm_f32x4_splat(soft.bias_rate), max_bias = wasm_f32x4_splat(-SOFT_MAX_BIAS_VEL);
    const v128_t mscale = wasm_f32x4_splat(soft.mass_scale), iscale = wasm_f32x4_splat(soft.impulse_scale);
    for (u32 g = 0; g < w->wide_count; g++) {
        WideContact* x = &wc[g];
        WBody A, B;
        gather(w, x->a, &A);
        gather(w, x->b, &B);
        v128_t n[3], t1[3], t2[3];
        ld3(x->n, n);
        ld3(x->t1, t1);
        ld3(x->t2, t2);
        const v128_t fric = x->friction.v;
        for (u32 i = 0; i < x->max_count; i++) {
            WidePoint* p = &x->p[i];
            WBody A0 = A, B0 = B;
            v128_t la[3], lb[3], ra[3], rb[3], t[3], d[3], dv[3], P[3];
            ld3(p->la, la);
            ld3(p->lb, lb);
            ld3(p->ra, ra);
            ld3(p->rb, rb);
            /* güncel ayrılma: s = dot((B.pos + R_B·lb) − (A.pos + R_A·la), n) + s0 */
            v128_t pa[3], pb[3];
            mat_mul(A.R, la, t);
            for (u32 k = 0; k < 3; k++) pa[k] = ADD(A.p[k], t[k]);
            mat_mul(B.R, lb, t);
            for (u32 k = 0; k < 3; k++) pb[k] = ADD(B.p[k], t[k]);
            for (u32 k = 0; k < 3; k++) d[k] = SUB(pb[k], pa[k]);
            v128_t s = ADD(dot(d, n), p->s0.v);
            v128_t pos = wasm_f32x4_gt(s, zero);
            v128_t spec = MUL(s, vinv_h);
            v128_t bias, ms, is;
            if (use_bias) {
                bias = SEL(pos, spec, wasm_f32x4_pmax(max_bias, MUL(rate, s)));   /* m_maxf(rate·s, −3) */
                ms = SEL(pos, one, mscale);
                is = SEL(pos, zero, iscale);
            } else {
                bias = SEL(pos, spec, zero);
                ms = one;
                is = zero;
            }
            rel_vel(&A, &B, ra, rb, dv);
            v128_t vn = dot(dv, n);
            /* imp = −m_n·ms·(vn + bias) − is·jn */
            v128_t imp = SUB(MUL(MUL(wasm_f32x4_neg(p->mass_n.v), ms), ADD(vn, bias)), MUL(is, p->jn.v));
            v128_t nn = wasm_f32x4_pmax(zero, ADD(p->jn.v, imp));   /* m_maxf(jn + imp, 0) */
            imp = SUB(nn, p->jn.v);
            v128_t jn = nn;
            v128_t max_jn = wasm_f32x4_pmax(nn, p->max_jn.v);       /* m_maxf(max_jn, nn) */
            for (u32 k = 0; k < 3; k++) P[k] = MUL(n[k], imp);
            apply(&A, &B, ra, rb, P);

            v128_t limit = MUL(fric, jn);
            rel_vel(&A, &B, ra, rb, dv);
            v128_t n1 = SUB(p->jt1.v, MUL(p->mass_t1.v, dot(dv, t1)));
            v128_t n2 = SUB(p->jt2.v, MUL(p->mass_t2.v, dot(dv, t2)));
            v128_t len2 = ADD(MUL(n1, n1), MUL(n2, n2));
            v128_t clamp = wasm_f32x4_gt(len2, MUL(limit, limit));
            v128_t kf = wasm_f32x4_div(limit, wasm_f32x4_sqrt(len2));
            n1 = SEL(clamp, MUL(n1, kf), n1);
            n2 = SEL(clamp, MUL(n2, kf), n2);
            v128_t d1 = SUB(n1, p->jt1.v), d2 = SUB(n2, p->jt2.v);
            for (u32 k = 0; k < 3; k++) P[k] = ADD(MUL(t1[k], d1), MUL(t2[k], d2));
            apply(&A, &B, ra, rb, P);

            v128_t m = p->mask.v;
            p->jn.v = SEL(m, jn, p->jn.v);
            p->max_jn.v = SEL(m, max_jn, p->max_jn.v);
            p->jt1.v = SEL(m, n1, p->jt1.v);
            p->jt2.v = SEL(m, n2, p->jt2.v);
            keep(m, &A, &B, &A0, &B0);
        }
        scatter(w, x->a, &A);
        scatter(w, x->b, &B);
    }
}
