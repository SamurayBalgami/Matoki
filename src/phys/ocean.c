/* phys/ocean.c — istatistiksel okyanus: Phillips spektrumu, Gauss karmaşık genlikler, ters 2B FFT (ocean.h).
 * FFT sırası: indeks j ↔ dalga sayısı j (j < n/2) ya da j − n; x_p = p·size/n'de h(x) = Σ_k h̃(k)·e^{ik·x}
 * doğrudan normalize edilmemiş ters DFT. İki gerçek alan tek karmaşık FFT'de paketlenir: alanlar Hermitsel
 * (h̃(−k) = conj h̃(k)) → IFFT(A + iB) = a + i·b. Faz f64'te 2π'ye indirgenir (uzun sürede hassasiyet). */
#include "phys/ocean.h"
#include "core/rng.h"

#define OCEAN_G 9.81f

static u32 neg_index(const Ocean* o, u32 idx) {
    u32 n = o->n, m = n - 1;
    u32 i = idx & m, j = idx >> o->log2n;
    return ((n - j) & m) * n + ((n - i) & m);
}

/* Box–Muller: iki bağımsız standart normal */
static void gauss2(Rng* r, f32* a, f32* b) {
    f32 u1 = m_maxf(rng_f32(r), 1e-7f), u2 = rng_f32(r);
    f32 rad = m_sqrtf(-2.0f * m_logf(u1));
    *a = rad * m_cosf(M_TAU_F * u2);
    *b = rad * m_sinf(M_TAU_F * u2);
}

/* Phillips: A·e^{-1/(kL)²}/k⁴ · (k̂·ŵ)² · e^{-k²l²}; rüzgâra karşı yarı 'reverse' ile çarpılır */
static f32 phillips(const OceanDesc* d, f32 kx, f32 kz, f32 wx, f32 wz) {
    f32 k2 = kx * kx + kz * kz;
    if (k2 < 1e-12f) return 0.0f;
    f32 L = d->wind_speed * d->wind_speed / OCEAN_G;
    f32 kw = (kx * wx + kz * wz) / m_sqrtf(k2);
    f32 dir = kw * kw;
    if (kw < 0.0f) dir *= d->reverse;
    f32 l = L * 0.001f;
    return m_expf(-1.0f / (k2 * L * L)) / (k2 * k2) * dir * m_expf(-k2 * l * l);
}

void ocean_init(Ocean* o, Arena* a, const OceanDesc* d) {
    u32 n = d->n, nn = n * n;
    o->d = *d;
    o->n = n;
    o->log2n = 0;
    while ((1u << o->log2n) < n) o->log2n++;
    o->h0r = ARENA_ARRAY(a, f32, nn);  o->h0i = ARENA_ARRAY(a, f32, nn);
    o->h0mr = ARENA_ARRAY(a, f32, nn); o->h0mi = ARENA_ARRAY(a, f32, nn);
    o->omega = ARENA_ARRAY(a, f32, nn);
    o->kx = ARENA_ARRAY(a, f32, nn);   o->kz = ARENA_ARRAY(a, f32, nn);
    for (u32 f = 0; f < 3; f++) { o->wr[f] = ARENA_ARRAY(a, f32, nn); o->wi[f] = ARENA_ARRAY(a, f32, nn); }
    o->height = ARENA_ARRAY(a, f32, nn);  o->disp_x = ARENA_ARRAY(a, f32, nn); o->disp_z = ARENA_ARRAY(a, f32, nn);
    o->slope_x = ARENA_ARRAY(a, f32, nn); o->slope_z = ARENA_ARRAY(a, f32, nn);
    o->bitrev = ARENA_ARRAY(a, u32, n);
    o->tw_c = ARENA_ARRAY(a, f32, n / 2 + 1); o->tw_s = ARENA_ARRAY(a, f32, n / 2 + 1);
    for (u32 i = 0; i < n; i++) {
        u32 r = 0;
        for (u32 b = 0; b < o->log2n; b++) r |= ((i >> b) & 1u) << (o->log2n - 1 - b);
        o->bitrev[i] = r;
    }
    for (u32 j = 0; j < n / 2; j++) {
        f32 ang = M_TAU_F * (f32)j / (f32)n;
        o->tw_c[j] = m_cosf(ang);
        o->tw_s[j] = m_sinf(ang);
    }
    f32 wl = m_sqrtf(d->wind_x * d->wind_x + d->wind_z * d->wind_z);
    f32 wx = wl > 0.0f ? d->wind_x / wl : 1.0f, wz = wl > 0.0f ? d->wind_z / wl : 0.0f;
    Rng rng = rng_make(d->seed);
    f64 energy = 0.0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 idx = j * n + i;
            i32 si = i < n / 2 ? (i32)i : (i32)i - (i32)n;
            i32 sj = j < n / 2 ? (i32)j : (i32)j - (i32)n;
            f32 kx = M_TAU_F * (f32)si / d->size, kz = M_TAU_F * (f32)sj / d->size;
            o->kx[idx] = kx;
            o->kz[idx] = kz;
            o->omega[idx] = m_sqrtf(OCEAN_G * m_sqrtf(kx * kx + kz * kz));
            f32 g1, g2;
            gauss2(&rng, &g1, &g2);   /* her k için sabit sırada (belirleyici) */
            /* Nyquist satır/sütunu kendi eşleniği: eğim/yer değiştirme orada Hermitsel olamaz → sıfır */
            b32 nyq = i == n / 2 || j == n / 2;
            f32 amp = nyq ? 0.0f : m_sqrtf(phillips(d, kx, kz, wx, wz) * 0.5f);
            o->h0r[idx] = g1 * amp;
            o->h0i[idx] = g2 * amp;
            energy += (f64)o->h0r[idx] * o->h0r[idx] + (f64)o->h0i[idx] * o->h0i[idx];
        }
    }
    /* Parseval: ortalama h² = Σ|h̃(k,t)|² ≈ 2·Σ|h̃0|² (çapraz terimler zamanla ortalanır) → hedef rms */
    f32 s = energy > 0.0 ? d->rms_height / (f32)__builtin_sqrt(2.0 * energy) : 0.0f;
    for (u32 idx = 0; idx < n * n; idx++) { o->h0r[idx] *= s; o->h0i[idx] *= s; }
    for (u32 idx = 0; idx < n * n; idx++) {
        u32 m = neg_index(o, idx);
        o->h0mr[idx] = o->h0r[m];
        o->h0mi[idx] = -o->h0i[m];
    }
}

void ocean_amplitude(const Ocean* o, u32 idx, f64 t, f32* re, f32* im) {
    f64 ph = (f64)o->omega[idx] * t;
    ph -= __builtin_floor(ph * (1.0 / 6.283185307179586)) * 6.283185307179586;
    f32 c = m_cosf((f32)ph), s = m_sinf((f32)ph);
    /* h0·e^{-iφ} + h0m·e^{iφ} */
    *re = o->h0r[idx] * c + o->h0i[idx] * s + o->h0mr[idx] * c - o->h0mi[idx] * s;
    *im = o->h0i[idx] * c - o->h0r[idx] * s + o->h0mi[idx] * c + o->h0mr[idx] * s;
}

void ocean_fft(const Ocean* o, f32* re, f32* im, u32 stride) {
    u32 n = o->n;
    for (u32 i = 0; i < n; i++) {
        u32 r = o->bitrev[i];
        if (r > i) {
            f32 t = re[i * stride]; re[i * stride] = re[r * stride]; re[r * stride] = t;
            t = im[i * stride]; im[i * stride] = im[r * stride]; im[r * stride] = t;
        }
    }
    for (u32 len = 2; len <= n; len <<= 1) {
        u32 half = len >> 1, step = n / len;
        for (u32 b = 0; b < n; b += len) {
            for (u32 k = 0; k < half; k++) {
                f32 wc = o->tw_c[k * step], ws = o->tw_s[k * step];   /* e^{+2πik/len} */
                u32 p = (b + k) * stride, q = (b + k + half) * stride;
                f32 xr = re[q] * wc - im[q] * ws;
                f32 xi = re[q] * ws + im[q] * wc;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr;        im[p] += xi;
            }
        }
    }
}

static void fft2d(const Ocean* o, f32* re, f32* im) {
    u32 n = o->n;
    for (u32 j = 0; j < n; j++) ocean_fft(o, re + j * n, im + j * n, 1);
    for (u32 i = 0; i < n; i++) ocean_fft(o, re + i, im + i, n);
}

void ocean_update(Ocean* o, f64 t) {
    u32 nn = o->n * o->n;
    f32 lam = o->d.choppy;
    for (u32 idx = 0; idx < nn; idx++) {
        f32 hr, hi;
        ocean_amplitude(o, idx, t, &hr, &hi);
        f32 kx = o->kx[idx], kz = o->kz[idx];
        f32 kl = m_sqrtf(kx * kx + kz * kz);
        f32 ux = kl > 0.0f ? kx / kl : 0.0f, uz = kl > 0.0f ? kz / kl : 0.0f;
        /* eğim S = i·k·h̃; yer değiştirme D = −i·k̂·h̃ (λ ile); paket: (h + i·Sx), (Dx + i·Dz), Sz */
        f32 sxr = -kx * hi, sxi = kx * hr;
        f32 szr = -kz * hi, szi = kz * hr;
        f32 dxr = lam * ux * hi, dxi = -lam * ux * hr;
        f32 dzr = lam * uz * hi, dzi = -lam * uz * hr;
        o->wr[0][idx] = hr - sxi;  o->wi[0][idx] = hi + sxr;
        o->wr[1][idx] = dxr - dzi; o->wi[1][idx] = dxi + dzr;
        o->wr[2][idx] = szr;       o->wi[2][idx] = szi;
    }
    for (u32 f = 0; f < 3; f++) fft2d(o, o->wr[f], o->wi[f]);
    for (u32 idx = 0; idx < nn; idx++) {
        o->height[idx] = o->wr[0][idx];
        o->slope_x[idx] = o->wi[0][idx];
        o->disp_x[idx] = o->wr[1][idx];
        o->disp_z[idx] = o->wi[1][idx];
        o->slope_z[idx] = o->wr[2][idx];
    }
}

void ocean_sample(const Ocean* o, f32 x, f32 z, Vec3* offset, Vec3* normal) {
    u32 n = o->n, m = n - 1;
    f32 inv = (f32)n / o->d.size;
    f32 gx = x * inv, gz = z * inv;
    f32 fx = m_floorf(gx), fz = m_floorf(gz);
    f32 tx = gx - fx, tz = gz - fz;
    u32 x0 = (u32)(i32)fx & m, z0 = (u32)(i32)fz & m;
    u32 x1 = (x0 + 1) & m, z1 = (z0 + 1) & m;
    u32 i00 = z0 * n + x0, i10 = z0 * n + x1, i01 = z1 * n + x0, i11 = z1 * n + x1;
    f32 w00 = (1.0f - tx) * (1.0f - tz), w10 = tx * (1.0f - tz), w01 = (1.0f - tx) * tz, w11 = tx * tz;
#define BL(a) ((a)[i00] * w00 + (a)[i10] * w10 + (a)[i01] * w01 + (a)[i11] * w11)
    *offset = v3(BL(o->disp_x), BL(o->height), BL(o->disp_z));
    *normal = v3_norm(v3(-BL(o->slope_x), 1.0f, -BL(o->slope_z)));
#undef BL
}
