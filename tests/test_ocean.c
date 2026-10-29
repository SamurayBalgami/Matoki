/* tests/test_ocean.c — istatistiksel okyanus (phys/ocean.c), M11 Faz 2.
 * FFT doğrudan DFT ile; genlikler Hermitsel (yükseklik gerçek); reverse = 0'da rüzgâra karşı genlik yok;
 * alan rüzgâr yönünde ilerler (zaman kaydırmalı çapraz ilinti); rms hedefi (Parseval); belirleyicilik. */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "phys/ocean.h"

static OceanDesc ocean_desc(u32 n, f32 reverse) {
    OceanDesc d;
    d.n = n;
    d.size = 40.0f;
    d.wind_x = 1.0f;
    d.wind_z = 0.0f;
    d.wind_speed = 5.0f;
    d.rms_height = 0.2f;
    d.reverse = reverse;
    d.choppy = 0.8f;
    d.seed = 1234;
    return d;
}

TEST(ocean_fft_matches_dft) {
    Ocean* o = ARENA_NEW(test_arena(), Ocean);
    OceanDesc d = ocean_desc(16, 0.0f);
    ocean_init(o, test_arena(), &d);
    f32 re[16], im[16];
    f64 rr[16], ri[16];
    Rng r = rng_make(7);
    for (u32 i = 0; i < 16; i++) { re[i] = rng_range(&r, -1, 1); im[i] = rng_range(&r, -1, 1); }
    for (u32 p = 0; p < 16; p++) {
        rr[p] = ri[p] = 0.0;
        for (u32 k = 0; k < 16; k++) {
            f64 a = 6.283185307179586 * (f64)(k * p) / 16.0;
            f64 c = m_cosf((f32)a), s = m_sinf((f32)a);
            rr[p] += re[k] * c - im[k] * s;
            ri[p] += re[k] * s + im[k] * c;
        }
    }
    ocean_fft(o, re, im, 1);
    for (u32 p = 0; p < 16; p++) {
        CHECK_NEAR(re[p], rr[p], 1e-4);
        CHECK_NEAR(im[p], ri[p], 1e-4);
    }
}

TEST(ocean_amplitudes_hermitian_height_real) {
    Ocean* o = ARENA_NEW(test_arena(), Ocean);
    OceanDesc d = ocean_desc(32, 0.3f);
    ocean_init(o, test_arena(), &d);
    u32 n = 32, nn = n * n;
    f32* re = ARENA_ARRAY(test_arena(), f32, nn);
    f32* im = ARENA_ARRAY(test_arena(), f32, nn);
    f64 t = 3.7;
    f64 worst = 0.0;
    for (u32 idx = 0; idx < nn; idx++) {
        u32 i = idx % n, j = idx / n;
        u32 neg = ((n - j) % n) * n + (n - i) % n;
        f32 ar, ai, br, bi;
        ocean_amplitude(o, idx, t, &ar, &ai);
        ocean_amplitude(o, neg, t, &br, &bi);
        f64 e = m_absf(ar - br) + m_absf(ai + bi);
        if (e > worst) worst = e;
        re[idx] = ar;
        im[idx] = ai;
    }
    CHECK(worst < 1e-6);
    for (u32 j = 0; j < n; j++) ocean_fft(o, re + j * n, im + j * n, 1);
    for (u32 i = 0; i < n; i++) ocean_fft(o, re + i, im + i, n);
    f64 max_im = 0.0, max_re = 0.0;
    for (u32 idx = 0; idx < nn; idx++) {
        if (m_absf(im[idx]) > max_im) max_im = m_absf(im[idx]);
        if (m_absf(re[idx]) > max_re) max_re = m_absf(re[idx]);
    }
    test_note("yukseklik max |gercek|", max_re, 4);
    test_note("yukseklik max |sanal|", max_im, 7);
    CHECK(max_re > 0.1);
    CHECK(max_im < 1e-4 * max_re);
}

TEST(ocean_no_reverse_waves) {
    Ocean* o = ARENA_NEW(test_arena(), Ocean);
    OceanDesc d = ocean_desc(32, 0.0f);
    ocean_init(o, test_arena(), &d);
    f64 down = 0.0, up = 0.0;
    for (u32 idx = 0; idx < 32 * 32; idx++) {
        f64 e = (f64)o->h0r[idx] * o->h0r[idx] + (f64)o->h0i[idx] * o->h0i[idx];
        if (o->kx[idx] < 0.0f) up += e;
        else down += e;
    }
    test_note("enerji ruzgar yonunde", down, 6);
    test_note("enerji ruzgara karsi", up, 6);
    CHECK(down > 0.0);
    CHECK(up == 0.0);
    /* reverse = 1: iki yön de dolu */
    Ocean* o2 = ARENA_NEW(test_arena(), Ocean);
    OceanDesc d2 = ocean_desc(32, 1.0f);
    ocean_init(o2, test_arena(), &d2);
    f64 up2 = 0.0;
    for (u32 idx = 0; idx < 32 * 32; idx++) {
        if (o2->kx[idx] < 0.0f) up2 += (f64)o2->h0r[idx] * o2->h0r[idx] + (f64)o2->h0i[idx] * o2->h0i[idx];
    }
    CHECK(up2 > 0.0);
}

/* C(s) = Σ h(x, t)·h(x + s, t + Δ): alan +x yönünde ilerliyorsa s > 0'da büyük */
static f64 shifted_corr(const f32* a, const f32* b, u32 n, i32 s) {
    f64 c = 0.0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) c += (f64)a[j * n + i] * b[j * n + (u32)((i32)i + s + (i32)n) % n];
    }
    return c;
}

TEST(ocean_waves_travel_downwind) {
    const u32 n = 64;
    f32 asym[2];
    for (u32 v = 0; v < 2; v++) {
        Ocean* o = ARENA_NEW(test_arena(), Ocean);
        OceanDesc d = ocean_desc(n, v == 0 ? 0.0f : 1.0f);
        ocean_init(o, test_arena(), &d);
        f32* h0 = ARENA_ARRAY(test_arena(), f32, n * n);
        ocean_update(o, 10.0);
        for (u32 k = 0; k < n * n; k++) h0[k] = o->height[k];
        ocean_update(o, 10.3);
        f64 fwd = shifted_corr(h0, o->height, n, 3), back = shifted_corr(h0, o->height, n, -3);
        asym[v] = (f32)((fwd - back) / (m_absf((f32)fwd) + m_absf((f32)back) + 1e-12f));
    }
    test_note("ilinti asimetrisi (reverse 0)", asym[0], 3);
    test_note("ilinti asimetrisi (reverse 1)", asym[1], 3);
    CHECK(asym[0] > 0.5f);
    CHECK(asym[0] > asym[1]);
}

TEST(ocean_rms_height_and_determinism) {
    const u32 n = 64;
    Ocean* a = ARENA_NEW(test_arena(), Ocean);
    Ocean* b = ARENA_NEW(test_arena(), Ocean);
    OceanDesc d = ocean_desc(n, 0.0f);
    ocean_init(a, test_arena(), &d);
    ocean_init(b, test_arena(), &d);
    f64 sum2 = 0.0;
    u32 cnt = 0;
    b32 same = TRUE;
    for (u32 s = 0; s < 8; s++) {
        f64 t = 2.0 + s * 3.1;
        ocean_update(a, t);
        ocean_update(b, t);
        for (u32 k = 0; k < n * n; k++) {
            sum2 += (f64)a->height[k] * a->height[k];
            cnt++;
            if (a->height[k] != b->height[k] || a->disp_x[k] != b->disp_x[k]) same = FALSE;
        }
    }
    f64 rms = __builtin_sqrt(sum2 / cnt);
    test_note("rms yukseklik (hedef 0.2)", rms, 4);
    CHECK_NEAR(rms, 0.2, 0.04);
    CHECK(same);
}
