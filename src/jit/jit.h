/* jit/jit.h — çalışma anında üretilen wasm modülünü yükleyip fonksiyon işaretçileri döndürür.
 * Platform (JS) modülü derler, env.memory ve env.cache (thread'in önsöz önbelleği) ile örnekler ve
 * export'larını sırayla (shade, prelude, shade_fast — olanları) __indirect_function_table'a ekler.
 * Her yükleme kayıt defterine yazılır (bayt kopyası dahil): worker'lar iş almadan önce defteri okuyup
 * aynı modülleri aynı tablo indekslerine, kendi önbellek dilimleriyle (cache + tid * stride) kurar. */
#ifndef JIT_JIT_H
#define JIT_JIT_H
#include "raster/shade_abi.h"

typedef struct JitShader {
    ShadeFn   shade;      /* tek başına doğru */
    PreludeFn prelude;    /* NULL: modülde yok */
    ShadeFn   fast;       /* prelude'dan sonra; NULL: modülde yok */
} JitShader;

/* cache: JOBS_MAX_THREADS × cache_stride baytlık önsöz önbelleği (codegen ayırır).
 * FALSE = başarısız (geçersiz modül veya platform JIT desteklemiyor). */
b32 jit_load(const u8* bytes, u32 len, void* cache, u32 cache_stride, b32 has_fast, JitShader* out);

#endif
