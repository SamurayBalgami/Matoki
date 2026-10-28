/* core/rng.h — deterministik sözde rastgele sayı üreteci (xorshift64*).
 * Testler ve prosedürel üretim için; aynı seed → aynı dizi (platformdan bağımsız).
 * Kriptografik değildir. */
#ifndef CORE_RNG_H
#define CORE_RNG_H
#include "core/types.h"

typedef struct Rng {
    u64 s;
} Rng;

static inline Rng rng_make(u64 seed) {
    Rng r;
    r.s = seed ? seed : 0x9E3779B97F4A7C15ull;
    return r;
}

static inline u32 rng_u32(Rng* r) {
    u64 x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return (u32)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

/* [0, 1) aralığında, 24 bit çözünürlük */
static inline f32 rng_f32(Rng* r) { return (f32)(rng_u32(r) >> 8) * (1.0f / 16777216.0f); }

static inline f32 rng_range(Rng* r, f32 lo, f32 hi) { return lo + (hi - lo) * rng_f32(r); }

static inline u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

#endif
