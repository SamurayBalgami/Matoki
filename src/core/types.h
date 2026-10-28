/* core/types.h — MATOKI temel tipleri ve makroları.
 * src/ altında libc başlığı yasak; <stdint.h> yerine bu dosya kullanılır.
 * Tipler derleyicinin önceden tanımlı makrolarından türetilir (clang).
 * EXPORT/IMPORT yalnızca wasm hedefinde anlam taşır. */
#ifndef CORE_TYPES_H
#define CORE_TYPES_H

typedef __UINT8_TYPE__   u8;
typedef __UINT16_TYPE__  u16;
typedef __UINT32_TYPE__  u32;
typedef __UINT64_TYPE__  u64;
typedef __INT8_TYPE__    i8;
typedef __INT16_TYPE__   i16;
typedef __INT32_TYPE__   i32;
typedef __INT64_TYPE__   i64;
typedef float            f32;
typedef double           f64;
typedef i32              b32;
typedef __SIZE_TYPE__    usize;
typedef __UINTPTR_TYPE__ uptr;

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL ((void*)0)
#endif

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX((x), (lo)), (hi))
#define UNUSED(x) ((void)(x))

#if defined(__wasm__)
#define EXPORT(name) __attribute__((export_name(#name)))
#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#else
#define EXPORT(name)
#define IMPORT(name)
#endif

#endif
