/* matoki/matoki.h — Matoki derleyicisinin üst düzey API'si.
 * matoki_compile: kaynak → (lexer → parser → sema → lower) → IR (sabit katlama + CSE dahil).
 * matoki_jit: IR → wasm (codegen) → JIT yükleme → ShadeFn. Uniform yerleşimi yansıtma ile
 * sorgulanır (matoki_find_uniform); C tarafı uniform bloğunu bu ofsetlere göre yazar. */
#ifndef MATOKI_MATOKI_H
#define MATOKI_MATOKI_H
#include "matoki/ast.h"
#include "matoki/codegen.h"
#include "matoki/ir.h"
#include "matoki/sema.h"
#include "raster/shade_abi.h"

typedef struct MatokiProgram {
    Module     module;
    SemaResult sema;
    IrFunc     ir;
    WasmModule wasm;
    ShadeFn    fn;          /* matoki_jit sonrası */
    u32        ir_total;    /* oluşturulan komut sayısı */
    u32        ir_live;     /* DCE sonrası canlı komut */
} MatokiProgram;

typedef struct MatokiOptions {
    u32 ir_cap;     /* 0 → 65536 */
    b32 no_fold;    /* sabit katlama + CSE kapalı (test) */
} MatokiOptions;

b32 matoki_compile(Arena* a, const char* src, u32 len, const MatokiOptions* opt, MatokiProgram* p, MatokiError* err);
b32 matoki_codegen(Arena* a, MatokiProgram* p, MatokiError* err);
b32 matoki_jit(Arena* a, MatokiProgram* p, MatokiError* err);   /* codegen + yükleme */

const GlobalDecl* matoki_find_uniform(const MatokiProgram* p, const char* name);
const GlobalDecl* matoki_find_varying(const MatokiProgram* p, const char* name);

/* "satır:sütun: mesaj" biçiminde */
void matoki_format_error(const MatokiError* e, StrBuf* out);

#endif
