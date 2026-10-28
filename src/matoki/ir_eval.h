/* matoki/ir_eval.h — IR'ın skaler yorumlayıcısı: DOĞRULUK KÂHİNİ.
 * Codegen çıktısıyla bit-bit aynı olmak zorunda (NaN yükü hariç; wasm NaN bitlerini
 * belirlemez). Ayrıca host'ta/testte ShadeFn olarak kullanılabilir (ir_eval_shade). */
#ifndef MATOKI_IR_EVAL_H
#define MATOKI_IR_EVAL_H
#include "matoki/ir.h"
#include "raster/shade_abi.h"

/* scratch: f->count * 16 bayt geçici bellek (her çağrıda mark/reset) */
void ir_eval(const IrFunc* f, const ShadeQuad* q, const void* uniforms, u32 out[4], Arena* scratch);

typedef struct IrEvalShader {
    const IrFunc* f;
    const void*   uniforms;
    Arena*        scratch;
} IrEvalShader;

/* uniforms parametresi bir IrEvalShader* olmalı */
void ir_eval_shade(const ShadeQuad* q, const void* ctx, u32 out[4]);

#endif
