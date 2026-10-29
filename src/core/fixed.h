/* core/fixed.h — 24.8 (Fx8) ve 16.16 (Fx16) sabit nokta aritmetiği.
 * Üçgen kurulumu ve alt-piksel rasterizasyon için kullanılır.
 * Taşmayı önlemek için çarpım ve bölümlerde i64 ara değer kullanılır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/core/fixed.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef CORE_FIXED_H
#define CORE_FIXED_H

#include "core/types.h"

typedef i32 Fx8;   /* 24.8 sabit nokta */
typedef i32 Fx16;  /* 16.16 sabit nokta */

#define FX8_SHIFT  8
#define FX8_ONE    (1 << FX8_SHIFT)
#define FX8_HALF   (1 << (FX8_SHIFT - 1))
#define FX8_MASK   (FX8_ONE - 1)

#define FX16_SHIFT 16
#define FX16_ONE   (1 << FX16_SHIFT)
#define FX16_HALF  (1 << (FX16_SHIFT - 1))
#define FX16_MASK  (FX16_ONE - 1)

/* --- Fx8 (24.8) --- */

static inline Fx8 fx8_from_i32(i32 v) {
    return (Fx8)((u32)v << FX8_SHIFT);
}

static inline Fx8 fx8_from_f32(f32 v) {
    f32 s = v * (f32)FX8_ONE;
    return (Fx8)(s >= 0.0f ? (s + 0.5f) : (s - 0.5f));
}

static inline i32 fx8_to_i32(Fx8 v) {
    return v >> FX8_SHIFT;
}

static inline f32 fx8_to_f32(Fx8 v) {
    return (f32)v * (1.0f / (f32)FX8_ONE);
}

static inline Fx8 fx8_floor(Fx8 v) {
    return v & ~FX8_MASK;
}

static inline Fx8 fx8_ceil(Fx8 v) {
    return (v + FX8_MASK) & ~FX8_MASK;
}

static inline Fx8 fx8_round(Fx8 v) {
    return (v + FX8_HALF) & ~FX8_MASK;
}

static inline Fx8 fx8_frac(Fx8 v) {
    return v & FX8_MASK;
}

static inline i32 fx8_floor_i32(Fx8 v) {
    return v >> FX8_SHIFT;
}

static inline i32 fx8_ceil_i32(Fx8 v) {
    return (v + FX8_MASK) >> FX8_SHIFT;
}

static inline i32 fx8_round_i32(Fx8 v) {
    return (v + FX8_HALF) >> FX8_SHIFT;
}

static inline Fx8 fx8_mul(Fx8 a, Fx8 b) {
    return (Fx8)(((i64)a * (i64)b) >> FX8_SHIFT);
}

static inline Fx8 fx8_div(Fx8 a, Fx8 b) {
    return (Fx8)((i64)((u64)(i64)a << FX8_SHIFT) / (i64)b);
}

/* --- Fx16 (16.16) --- */

static inline Fx16 fx16_from_i32(i32 v) {
    return (Fx16)((u32)v << FX16_SHIFT);
}

static inline Fx16 fx16_from_f32(f32 v) {
    f32 s = v * (f32)FX16_ONE;
    return (Fx16)(s >= 0.0f ? (s + 0.5f) : (s - 0.5f));
}

static inline i32 fx16_to_i32(Fx16 v) {
    return v >> FX16_SHIFT;
}

static inline f32 fx16_to_f32(Fx16 v) {
    return (f32)v * (1.0f / (f32)FX16_ONE);
}

static inline Fx16 fx16_floor(Fx16 v) {
    return v & ~FX16_MASK;
}

static inline Fx16 fx16_ceil(Fx16 v) {
    return (v + FX16_MASK) & ~FX16_MASK;
}

static inline Fx16 fx16_round(Fx16 v) {
    return (v + FX16_HALF) & ~FX16_MASK;
}

static inline Fx16 fx16_frac(Fx16 v) {
    return v & FX16_MASK;
}

static inline i32 fx16_floor_i32(Fx16 v) {
    return v >> FX16_SHIFT;
}

static inline i32 fx16_ceil_i32(Fx16 v) {
    return (v + FX16_MASK) >> FX16_SHIFT;
}

static inline i32 fx16_round_i32(Fx16 v) {
    return (v + FX16_HALF) >> FX16_SHIFT;
}

static inline Fx16 fx16_mul(Fx16 a, Fx16 b) {
    return (Fx16)(((i64)a * (i64)b) >> FX16_SHIFT);
}

static inline Fx16 fx16_div(Fx16 a, Fx16 b) {
    return (Fx16)((i64)((u64)(i64)a << FX16_SHIFT) / (i64)b);
}

static inline Fx16 fx8_to_fx16(Fx8 v) {
    return (Fx16)((u32)v << (FX16_SHIFT - FX8_SHIFT));
}

static inline Fx8 fx16_to_fx8(Fx16 v) {
    return (Fx8)(v >> (FX16_SHIFT - FX8_SHIFT));
}

#endif
