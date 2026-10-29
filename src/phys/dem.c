/* phys/dem.c — Hertz temaslı küreler (dem.h). Kuvvet: zemin + tüm küre çiftleri (O(n²), küçük n için).
 * Hız Verlet: x += v·h + ½a·h², sönüm için v tahmini v + a·h ile yeni ivme, v += ½(a + a')·h. Korunumlu
 * kısımda simplektik (e = 1'de enerji hatası O(h²), birikmez). Kesirli üsler sqrt ile: δ^{3/2} = δ·√δ. */
#include "phys/dem.h"

static f64 dsqrt(f64 x) { return __builtin_sqrt(x); }

/* E* = E / (2(1 − ν²)) (iki gövde aynı malzeme) */
static f64 e_star(const DemWorld* w) { return w->young / (2.0 * (1.0 - w->poisson * w->poisson)); }

/* normal temas kuvveti büyüklüğü (≥ 0) ve esneklik enerjisi */
static f64 hertz(f64 k, f64 ms, f64 gamma, f64 delta, f64 ddot) {
    f64 sd = dsqrt(delta);
    f64 fn = k * delta * sd + gamma * dsqrt(ms * k) * dsqrt(sd) * ddot;
    return fn > 0.0 ? fn : 0.0;
}

static f64 gravity_acc(const DemWorld* w, f64 y) {
    if (w->planet_gm <= 0.0) return -w->g;
    f64 r = w->planet_radius + y;
    return -w->planet_gm / (r * r);
}

static void forces(DemWorld* w, const f64* vx, f64* f) {
    f64 es = e_star(w);
    w->contacts = 0;
    for (u32 i = 0; i < w->n; i++) {
        const DemBall* b = &w->b[i];
        f[i * 3] = 0.0;
        f[i * 3 + 1] = b->m * gravity_acc(w, b->x[1]);
        f[i * 3 + 2] = 0.0;
        f64 delta = b->r - b->x[1];   /* zemin y = 0 */
        if (delta > 0.0) {
            f64 k = (4.0 / 3.0) * es * dsqrt(b->r);
            f[i * 3 + 1] += hertz(k, b->m, w->gamma, delta, -vx[i * 3 + 1]);
            w->contacts++;
        }
    }
    for (u32 i = 0; i < w->n; i++) {
        for (u32 j = i + 1; j < w->n; j++) {
            const DemBall* a = &w->b[i];
            const DemBall* c = &w->b[j];
            f64 d[3] = { c->x[0] - a->x[0], c->x[1] - a->x[1], c->x[2] - a->x[2] };
            f64 dist = dsqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            f64 delta = a->r + c->r - dist;
            if (delta <= 0.0 || dist < 1e-12) continue;
            f64 nrm[3] = { d[0] / dist, d[1] / dist, d[2] / dist };
            f64 rel = (vx[j * 3] - vx[i * 3]) * nrm[0] + (vx[j * 3 + 1] - vx[i * 3 + 1]) * nrm[1] + (vx[j * 3 + 2] - vx[i * 3 + 2]) * nrm[2];
            f64 rs = a->r * c->r / (a->r + c->r), ms = a->m * c->m / (a->m + c->m);
            f64 fn = hertz((4.0 / 3.0) * es * dsqrt(rs), ms, w->gamma, delta, -rel);
            for (u32 k = 0; k < 3; k++) {
                f[j * 3 + k] += fn * nrm[k];
                f[i * 3 + k] -= fn * nrm[k];
            }
            w->contacts++;
        }
    }
}

f64 dem_gamma_for_restitution(f64 e) {
    if (e >= 1.0) return 0.0;
    /* boyutsuz çarpışma: m* = k = 1, δ(0) = 0, δ'(0) = 1; ayrılma hızı = e(γ), γ'da azalan */
    f64 lo = 0.0, hi = 4.0;
    for (u32 it = 0; it < 48; it++) {
        f64 g = 0.5 * (lo + hi);
        f64 x = 0.0, v = 1.0, h = 2e-4;
        f64 a = -hertz(1.0, 1.0, g, 0.0, v);
        for (u32 s = 0; s < 200000; s++) {
            x += v * h + 0.5 * a * h * h;
            if (x <= 0.0) break;
            f64 an = -hertz(1.0, 1.0, g, x, v + a * h);
            v += 0.5 * (a + an) * h;
            a = an;
        }
        if (-v > e) lo = g;   /* fazla sekiyor → daha çok sönüm */
        else hi = g;
    }
    return 0.5 * (lo + hi);
}

void dem_init(DemWorld* w, Arena* a, u32 cap, f64 young, f64 poisson, f64 restitution) {
    w->b = ARENA_ARRAY(a, DemBall, cap);
    w->f = ARENA_ARRAY(a, f64, cap * 3);
    w->f_new = ARENA_ARRAY(a, f64, cap * 3);
    w->vp = ARENA_ARRAY(a, f64, cap * 3);
    w->n = 0;
    w->cap = cap;
    w->g = 9.81;
    w->planet_gm = 0.0;
    w->planet_radius = 0.0;
    w->young = young;
    w->poisson = poisson;
    w->restitution = restitution;
    w->gamma = dem_gamma_for_restitution(restitution);
    w->h = 0.0;
    w->steps_per_contact = 40.0;
    w->contacts = 0;
}

u32 dem_add(DemWorld* w, f64 x, f64 y, f64 z, f64 r, f64 m) {
    if (w->n >= w->cap) return w->n;
    DemBall* b = &w->b[w->n];
    b->x[0] = x; b->x[1] = y; b->x[2] = z;
    b->v[0] = b->v[1] = b->v[2] = 0.0;
    b->r = r;
    b->m = m;
    return w->n++;
}

/* Hertz temas süresi t_c ≈ 2,87·(m*² / (R*·E*²·v))^{1/5}; en kısa olası temas en hafif / en küçük çift, en hızlı
 * yaklaşmada. Alt adım t_c / steps_per_contact. */
static f64 substep(const DemWorld* w) {
    f64 mmin = 1e30, rmin = 1e30, vmax = 1.0;
    for (u32 i = 0; i < w->n; i++) {
        const DemBall* b = &w->b[i];
        if (b->m < mmin) mmin = b->m;
        if (b->r < rmin) rmin = b->r;
        f64 s = dsqrt(b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]);
        if (2.0 * s > vmax) vmax = 2.0 * s;
    }
    f64 es = e_star(w), ms = 0.5 * mmin, rs = 0.5 * rmin;
    f32 q = (f32)(ms * ms / (rs * es * es * vmax));
    f64 tc = 2.87 * (f64)m_powf(q, 0.2f);
    return tc / w->steps_per_contact;
}

void dem_step(DemWorld* w, f64 dt) {
    if (!w->n || dt <= 0.0) return;
    f64 hmax = substep(w);
    u32 nsub = (u32)(dt / hmax) + 1;
    f64 h = dt / (f64)nsub;
    w->h = h;
    u32 n3 = w->n * 3;
    f64* vp = w->vp;   /* hız tahmini (sönüm terimi için) */

    for (u32 i = 0; i < w->n; i++) for (u32 k = 0; k < 3; k++) vp[i * 3 + k] = w->b[i].v[k];
    forces(w, vp, w->f);
    for (u32 s = 0; s < nsub; s++) {
        for (u32 i = 0; i < w->n; i++) {
            DemBall* b = &w->b[i];
            for (u32 k = 0; k < 3; k++) {
                f64 acc = w->f[i * 3 + k] / b->m;
                b->x[k] += b->v[k] * h + 0.5 * acc * h * h;
                vp[i * 3 + k] = b->v[k] + acc * h;
            }
        }
        forces(w, vp, w->f_new);
        for (u32 i = 0; i < w->n; i++) {
            DemBall* b = &w->b[i];
            for (u32 k = 0; k < 3; k++) b->v[k] += 0.5 * (w->f[i * 3 + k] + w->f_new[i * 3 + k]) / b->m * h;
        }
        for (u32 k = 0; k < n3; k++) w->f[k] = w->f_new[k];
    }
}

f64 dem_energy(const DemWorld* w) {
    f64 es = e_star(w), e = 0.0;
    for (u32 i = 0; i < w->n; i++) {
        const DemBall* b = &w->b[i];
        e += 0.5 * b->m * (b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]);
        e += w->planet_gm > 0.0 ? -b->m * w->planet_gm / (w->planet_radius + b->x[1]) : b->m * w->g * b->x[1];
        f64 d = b->r - b->x[1];
        if (d > 0.0) e += 0.4 * (4.0 / 3.0) * es * dsqrt(b->r) * d * d * dsqrt(d);
        for (u32 j = i + 1; j < w->n; j++) {
            const DemBall* c = &w->b[j];
            f64 dx = c->x[0] - b->x[0], dy = c->x[1] - b->x[1], dz = c->x[2] - b->x[2];
            f64 dd = b->r + c->r - dsqrt(dx * dx + dy * dy + dz * dz);
            if (dd > 0.0) e += 0.4 * (4.0 / 3.0) * es * dsqrt(b->r * c->r / (b->r + c->r)) * dd * dd * dsqrt(dd);
        }
    }
    return e;
}
