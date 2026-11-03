/* phys/wave.h — yüzey dalga denklemi (D49): h_tt = c²∇²h − k·h_t, kare ızgarada açık sonlu fark (leapfrog). Sığ suda c = √(g·derinlik).
 * Fizik çekirdeğine bağımlı değil, üstüne eklenen bir alan: cisimler kaynak ekler (wave_add), engeller yansıtır (wave_block),
 * sonuç eğim dokusu olarak shader'a gider (wave_bake). Adım sabit, CFL sınırının altında: belirlenimci ve kararlı. */
#ifndef PHYS_WAVE_H
#define PHYS_WAVE_H
#include "core/arena.h"

typedef struct WaveField {
    u32  n;              /* kenar başına hücre */
    f32  size, dx;       /* alan kenarı, hücre boyu (m) */
    f32  c, damping;     /* dalga hızı (m/s), sönüm (1/s) */
    f32  dt, r2;         /* sabit adım (s), (c·dt/dx)² */
    f32  accum;
    f32* h;              /* yükseklik (m), n² */
    f32* hp;             /* bir önceki adım */
    u8*  solid;          /* engel hücresi: h = 0, dalga yansır */
} WaveField;

/* alan x, z ∈ [-size/2, size/2] (yerel); kenarlar sabit uçlu: dalga kıyıdan yansır */
void wave_init(WaveField* w, Arena* a, u32 n, f32 size, f32 c, f32 damping);
void wave_step(WaveField* w, f32 dt);
/* Gauss tepesi: merkez yüksekliği amount (m), standart sapma radius (m) */
void wave_add(WaveField* w, f32 x, f32 z, f32 radius, f32 amount);
void wave_block(WaveField* w, f32 x, f32 z, f32 half_x, f32 half_z);
f32  wave_height(const WaveField* w, f32 x, f32 z);
/* ayrık (kinetik + potansiyel) enerji, birim ρ = 1; sönümsüzken adımlar boyunca korunur: test ve tanı */
f32  wave_energy(const WaveField* w);
/* doku w × w piksele: R, G = 128 + 510·(∂h/∂x, ∂h/∂z) (eğim ±0,25'e kadar), B = 128 + 255·h, A = 255 */
void wave_bake(const WaveField* w, u32* px);

#endif
