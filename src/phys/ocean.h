/* phys/ocean.h — istatistiksel okyanus (Tessendorf 2001): karmaşık genlikli dalga spektrumu + 2B FFT.
 * Her dalga vektörü k'nın genliği karmaşık bir Gauss rastgele sayısı: h̃0(k) = (ξr + i·ξi)·√(P(k)/2), P = Phillips
 * spektrumu (rüzgâra hizalı enerji dağılımı = genliğin olasılık yoğunluğu). Zamanla h̃(k,t) = h̃0(k)·e^{-iωt}
 * + conj(h̃0(-k))·e^{iωt}, ω = √(g|k|): sanal üstel faz dalgayı k yönünde yürütür. Rüzgâra karşı (k̂·ŵ < 0)
 * genlikler 'reverse' ile çarpılır (0 → ters dalga yok). Alanlar ters FFT ile: yükseklik, yatay yer değiştirme, eğim. */
#ifndef PHYS_OCEAN_H
#define PHYS_OCEAN_H
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"

typedef struct OceanDesc {
    u32 n;            /* ızgara (2'nin kuvveti) */
    f32 size;         /* yama kenarı (m); alan bu periyotla döşenir */
    f32 wind_x, wind_z;
    f32 wind_speed;   /* m/s; tepe dalga boyu ~ 2π·V²/g */
    f32 rms_height;   /* hedef karekök ortalama yükseklik (m), genlikler buna ölçeklenir */
    f32 reverse;      /* rüzgâra karşı genlik çarpanı (0..1) */
    f32 choppy;       /* yatay yer değiştirme çarpanı λ (sivri tepe) */
    u64 seed;
} OceanDesc;

typedef struct Ocean {
    OceanDesc d;
    u32  n, log2n;
    f32* h0r;  f32* h0i;     /* h̃0(k) */
    f32* h0mr; f32* h0mi;    /* conj(h̃0(-k)) */
    f32* omega;
    f32* kx;   f32* kz;      /* dalga vektörü (rad/m), FFT sırası */
    u32* bitrev;
    f32* tw_c; f32* tw_s;    /* ters FFT dönüş çarpanları cos, sin(2πj/n) */
    f32* wr[3]; f32* wi[3];  /* çalışma: 3 karmaşık alan (ikişer gerçek alan paketli) */
    /* çıktılar (n×n, satır = z): */
    f32* height; f32* disp_x; f32* disp_z; f32* slope_x; f32* slope_z;
} Ocean;

void ocean_init(Ocean* o, Arena* a, const OceanDesc* d);
/* t anındaki alanları hesaplar (3 ters 2B FFT) */
void ocean_update(Ocean* o, f64 t);
/* yerel (x, z) noktasında periyodik çift doğrusal örnek: konum yer değiştirmesi + normal */
void ocean_sample(const Ocean* o, f32 x, f32 z, Vec3* offset, Vec3* normal);
/* idx dalga vektörünün t anındaki karmaşık genliği h̃(k,t) (test) */
void ocean_amplitude(const Ocean* o, u32 idx, f64 t, f32* re, f32* im);
/* n noktalı yerinde ters (pozitif üslü, normalize edilmemiş) karmaşık FFT, adım 'stride' (test) */
void ocean_fft(const Ocean* o, f32* re, f32* im, u32 stride);

#endif
