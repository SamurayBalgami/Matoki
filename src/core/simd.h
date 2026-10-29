/* core/simd.h — WebAssembly SIMD128 ve Relaxed SIMD ortak soyutlama makroları.
 * -mrelaxed-simd etkin olduğunda (__wasm_relaxed_simd__) f32x4.relaxed_madd (FMA),
 * relaxed min/max, laneselect, trunc ve swizzle komutlarını üretir; aksi halde
 * standart SIMD128 (mul+add, bitselect, trunc_sat vb.) karşılıklarına düşer.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/core/simd.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef CORE_SIMD_H
#define CORE_SIMD_H

#include "core/types.h"

#if defined(__wasm_relaxed_simd__)
#define SIMD_RELAXED 1
#else
#define SIMD_RELAXED 0
#endif

/* Platform yetenek bit maskesi (web/caps.js -> engine_set_caps ile eşleşir) */
#define CAP_SAB             (1u << 0)
#define CAP_SIMD128         (1u << 1)
#define CAP_RELAXED_SIMD    (1u << 2)
#define CAP_THREADS         (1u << 3)
#define CAP_BULK_MEMORY     (1u << 4)
#define CAP_MEMORY64        (1u << 5)
#define CAP_WEBGPU          (1u << 6)
#define CAP_GPU_TIMESTAMP   (1u << 7)
#define CAP_GPU_SUBGROUPS   (1u << 8)
#define CAP_GPU_F16         (1u << 9)
#define CAP_OFFSCREEN       (1u << 10)
#define CAP_WAIT_ASYNC      (1u << 11)
#define CAP_WEBCODECS       (1u << 12)
#define CAP_WEBTRANSPORT    (1u << 13)
#define CAP_WORKER_RENDER   (1u << 14)
#define CAP_RELAXED_BUILD   (1u << 15)

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>

#if SIMD_RELAXED

/* a * b + c (tek komut: f32x4.relaxed_madd) */
#define simd_madd(a, b, c)         wasm_f32x4_relaxed_madd((a), (b), (c))

/* -(a * b) + c (tek komut: f32x4.relaxed_nmadd) */
#define simd_nmadd(a, b, c)        wasm_f32x4_relaxed_nmadd((a), (b), (c))

/* NaN içermediği bilinen şeritlerde hızlı f32x4 min / max */
#define simd_min_f32(a, b)         wasm_f32x4_relaxed_min((a), (b))
#define simd_max_f32(a, b)         wasm_f32x4_relaxed_max((a), (b))

/* Maske şeritleri tam 0 veya 0xFFFFFFFF olduğunda şerit seçimi */
#define simd_laneselect_i32(a, b, m) wasm_i32x4_relaxed_laneselect((a), (b), (m))
#define simd_laneselect_i8(a, b, m)  wasm_i8x16_relaxed_laneselect((a), (b), (m))

/* Aralık içinde olduğu bilinen f32x4 -> i32x4 / u32x4 dönüşümü */
#define simd_trunc_s_f32(a)        wasm_i32x4_relaxed_trunc_f32x4((a))
#define simd_trunc_u_f32(a)        wasm_u32x4_relaxed_trunc_f32x4((a))

/* İndekslerin [0..15] aralığında olduğu bilinen bayt karıştırma */
#define simd_swizzle_i8(a, s)      wasm_i8x16_relaxed_swizzle((a), (s))

#else /* !SIMD_RELAXED */

#define simd_madd(a, b, c)         wasm_f32x4_add(wasm_f32x4_mul((a), (b)), (c))
#define simd_nmadd(a, b, c)        wasm_f32x4_sub((c), wasm_f32x4_mul((a), (b)))
#define simd_min_f32(a, b)         wasm_f32x4_min((a), (b))
#define simd_max_f32(a, b)         wasm_f32x4_max((a), (b))
#define simd_laneselect_i32(a, b, m) wasm_v128_bitselect((a), (b), (m))
#define simd_laneselect_i8(a, b, m)  wasm_v128_bitselect((a), (b), (m))
#define simd_trunc_s_f32(a)        wasm_i32x4_trunc_sat_f32x4((a))
#define simd_trunc_u_f32(a)        wasm_u32x4_trunc_sat_f32x4((a))
#define simd_swizzle_i8(a, s)      wasm_i8x16_swizzle((a), (s))

#endif /* SIMD_RELAXED */

#endif /* defined(__wasm_simd128__) */

#endif /* CORE_SIMD_H */
