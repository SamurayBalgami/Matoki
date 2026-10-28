/* jit/jit.h — çalışma anında üretilen wasm modülünü yükleyip fonksiyon işaretçileri döndürür.
 * Platform (JS) modülü derler, env.memory ile örnekler ve export'larını sırayla (shade, prelude,
 * shade_fast — olanları) motorun __indirect_function_table'ına ekler; dönen ilk indeks C'de işaretçidir. */
#ifndef JIT_JIT_H
#define JIT_JIT_H
#include "raster/shade_abi.h"

typedef struct JitShader {
    ShadeFn   shade;      /* tek başına doğru */
    PreludeFn prelude;    /* NULL: modülde yok */
    ShadeFn   fast;       /* prelude'dan sonra; NULL: modülde yok */
} JitShader;

/* FALSE = başarısız (geçersiz modül veya platform JIT desteklemiyor). has_fast: modül prelude + shade_fast içeriyor. */
b32 jit_load(const u8* bytes, u32 len, b32 has_fast, JitShader* out);

/* yalnızca "shade" (eski arayüz) */
ShadeFn jit_load_shade(const u8* bytes, u32 len);

#endif
