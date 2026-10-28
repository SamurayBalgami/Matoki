/* matoki/codegen.h — IR → wasm SIMD modülü (ikili, doğrudan bayt üretimi).
 * Modül env.memory'yi içe aktarır ve üç fonksiyon dışa aktarır (bu sırayla tabloya eklenir):
 *   shade(q, uniforms, out)      — her şeyi hesaplar (ShadeFn; tek başına doğru)
 *   prelude(uniforms)            — uniform'lardan türeyen değerleri önbelleğe yazar (PreludeFn)
 *   shade_fast(q, uniforms, out) — önbelleği okur; aynı uniforms ile prelude'dan SONRA çağrılmalı
 * Önbellek codegen arenasında; arena program yaşadıkça yaşamalı. Her canlı IR değeri bir v128 yereli. */
#ifndef MATOKI_CODEGEN_H
#define MATOKI_CODEGEN_H
#include "matoki/ir.h"

typedef struct WasmModule {
    u8*   bytes;
    u32   len;
    u32   live_insts;
    u32   body_bytes;   /* shade gövdesi */
    u32   hoisted;      /* önbelleğe alınan değer sayısı */
    void* cache;
} WasmModule;

b32 codegen_wasm(IrFunc* f, Arena* a, WasmModule* out);
/* ölçüm: quad-sabit değerleri skaler (varsayılan) ya da v128 hesapla; sonraki codegen'lere etki eder */
void codegen_set_scalar(b32 on);

#endif
