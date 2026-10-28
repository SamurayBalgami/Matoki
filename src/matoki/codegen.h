/* matoki/codegen.h — IR → wasm SIMD modülü (ikili, doğrudan bayt üretimi).
 * Modül: env.memory'yi içe aktarır, tek fonksiyon "shade" (i32 q, i32 uniforms, i32 out) -> ()
 * dışa aktarır; imzası ShadeFn ile aynı, böylece motorun fonksiyon tablosuna eklenip
 * C'den işaretçiyle çağrılabilir. Her canlı IR değeri bir v128 yereli. */
#ifndef MATOKI_CODEGEN_H
#define MATOKI_CODEGEN_H
#include "matoki/ir.h"

typedef struct WasmModule {
    u8* bytes;
    u32 len;
    u32 live_insts;
    u32 body_bytes;
} WasmModule;

b32 codegen_wasm(IrFunc* f, Arena* a, WasmModule* out);

#endif
