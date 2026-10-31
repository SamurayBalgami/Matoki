/* phys/interp.h — fizik ↔ görüntü ara değerlemesi (D39). Fizik sabit adımla (demo: 60 Hz) ilerler, görüntü kare
 * hızında; son adımın pozunu doğrudan çizmek 144 Hz ekranda takılma (kimi kare kımıldamaz, kimi atlar) verir.
 * Her adımdan önce poz saklanır (save); çizimden önce önceki ↔ şimdiki poz arasında α = birikmiş / adım ile ara değer
 * gövdelere yazılır (apply), çizimden sonra şimdiki poz bit bit geri konur (restore). Fizik durumu değişmez →
 * belirlenimcilik ve geri sarma etkilenmez. Geçmişi olmayan (yeni eklenen, ışınlanan) gövde olduğu gibi çizilir. */
#ifndef PHYS_INTERP_H
#define PHYS_INTERP_H
#include "phys/phys.h"

/* katı gövdeler: konum + dönüş (rot_m de türetilir ve geri konur) */
typedef struct PhysInterp {
    u32   cap;
    u32   count;     /* geçmişi olan gövde sayısı (0 = geçmiş yok: sahne kuruldu / ışınlandı) */
    u32   applied;   /* son apply'ın değiştirdiği gövde aralığı [0, applied) — restore bunu geri koyar */
    Vec3* prev_pos;
    Quat* prev_rot;
    Vec3* cur_pos;
    Quat* cur_rot;
    Mat3* cur_rot_m;
} PhysInterp;

void phys_interp_init(PhysInterp* p, Arena* a, u32 cap);
void phys_interp_reset(PhysInterp* p);
void phys_interp_save(PhysInterp* p, const PhysWorld* w);
void phys_interp_apply(PhysInterp* p, PhysWorld* w, f32 alpha);
void phys_interp_restore(PhysInterp* p, PhysWorld* w);

/* nokta kümesi (yumuşak cisim parçacıkları, karakter ayağı): birden çok dizi `at` ofsetleriyle tek kümede.
 * save sırayla çağrılır (count = en büyük at + n); apply / restore aynı (at, x, n) üçlüleriyle. */
typedef struct PhysInterpPts {
    u32   cap;
    u32   count;
    Vec3* prev;
    Vec3* cur;
} PhysInterpPts;

void phys_interp_pts_init(PhysInterpPts* p, Arena* a, u32 cap);
void phys_interp_pts_reset(PhysInterpPts* p);
void phys_interp_pts_save(PhysInterpPts* p, u32 at, const Vec3* x, u32 n);
void phys_interp_pts_apply(PhysInterpPts* p, u32 at, Vec3* x, u32 n, f32 alpha);
void phys_interp_pts_restore(const PhysInterpPts* p, u32 at, Vec3* x, u32 n);

#endif
