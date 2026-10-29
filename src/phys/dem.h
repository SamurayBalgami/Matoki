/* phys/dem.h — ayrık eleman yöntemi (DEM): Hertz temaslı küreler + zemin düzlemi (y = 0), f64.
 * Temas kuvveti F = k·δ^{3/2} + γ·√(m*·k)·δ^{1/4}·δ̇ (Tsuji): k = (4/3)·E*·√R*, restitüsyon e hızdan bağımsız,
 * γ(e) başlangıçta tek çarpışma simülasyonuyla bisection ile kalibre edilir. Üst üste değen topların çarpışması
 * katı cisim çözücüsünün sıralı darbe modelinin aksine aynı anda, temas esnekliğiyle yayılır (D34).
 * Çekim sabit g ya da g(h) = GM/(R+h)². Hız Verlet; tek temas süresine ≥ 40 alt adım düşecek şekilde. */
#ifndef PHYS_DEM_H
#define PHYS_DEM_H
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"

typedef struct DemBall {
    f64 x[3], v[3];
    f64 r, m;
} DemBall;

typedef struct DemWorld {
    DemBall* b;
    u32  n, cap;
    f64  g;             /* sabit çekim (planet_gm = 0 iken), m/s² */
    f64  planet_gm;     /* > 0: g(h) = GM / (R + y)² */
    f64  planet_radius;
    f64  young;         /* E (Pa) */
    f64  poisson;       /* ν */
    f64  restitution;   /* hedef e (0..1] */
    f64  gamma;         /* kalibre edilen Tsuji sönüm katsayısı */
    f64  h;             /* son alt adım (s) */
    f64  steps_per_contact; /* en kısa temas süresine düşen alt adım (varsayılan 40) */
    f64* f;             /* kuvvet tamponu (3n) */
    f64* f_new;
    f64* vp;            /* hız tahmini */
    u32  contacts;      /* son adımdaki temas sayısı */
} DemWorld;

void dem_init(DemWorld* w, Arena* a, u32 cap, f64 young, f64 poisson, f64 restitution);
u32  dem_add(DemWorld* w, f64 x, f64 y, f64 z, f64 r, f64 m);
/* dt'yi alt adımlarla ilerletir */
void dem_step(DemWorld* w, f64 dt);
/* toplam enerji: kinetik + çekim potansiyeli + Hertz esneklik enerjisi (2/5)·k·δ^{5/2} */
f64  dem_energy(const DemWorld* w);
/* e için γ (scale-free: sonuç kütle/yarıçap/E/hızdan bağımsız) */
f64  dem_gamma_for_restitution(f64 e);

#endif
