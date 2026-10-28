/* jit/jit.h — çalışma anında üretilen wasm modülünü yükleyip ShadeFn olarak döndürür.
 * Platform (JS) modülü derler, env.memory ile örnekler ve "shade" export'unu motorun
 * __indirect_function_table'ına ekler; dönen tablo indeksi C'de fonksiyon işaretçisidir. */
#ifndef JIT_JIT_H
#define JIT_JIT_H
#include "raster/shade_abi.h"

/* NULL = başarısız (geçersiz modül veya platform JIT desteklemiyor) */
ShadeFn jit_load_shade(const u8* bytes, u32 len);

#endif
