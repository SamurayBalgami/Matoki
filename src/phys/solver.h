/* phys/solver.h — soft step temas çözücüsünün iç arayüzü (world.c ↔ solver_simd_wasm.c).
 * Skaler yol world.c'de (manifold başına); SIMD yol aynı formülü aynı işlem sırasıyla 4 manifoldda
 * birlikte uygular. Renk içi manifoldlar ortak dinamik gövde paylaşmadığından iki yol bit-bit aynıdır. */
#ifndef PHYS_SOLVER_H
#define PHYS_SOLVER_H
#include "phys/phys.h"

#define SOFT_MAX_BIAS_VEL 3.0f   /* girişim düzeltmesinin üst hızı (m/s) */

typedef struct Softness {
    f32 bias_rate, mass_scale, impulse_scale;
} Softness;

#if defined(__wasm_simd128__)
#define PHYS_HAVE_SIMD 1
/* w->wide için gereken bayt (grup kapasitesi cap) */
usize solver_simd_bytes(u32 cap);
/* soft_prepare sonrası: renk 0..PHYS_COLORS-1 manifoldlarını 4'lü gruplara kopyala */
void solver_simd_prepare(PhysWorld* w);
void solver_simd_warm_start(PhysWorld* w);
void solver_simd_solve(PhysWorld* w, f32 inv_h, Softness soft, b32 use_bias);
/* biriken itkileri manifoldlara geri yaz (restitüsyon ve sonraki adımın warm start'ı için) */
void solver_simd_store(PhysWorld* w);
#else
#define PHYS_HAVE_SIMD 0
#endif

#endif
