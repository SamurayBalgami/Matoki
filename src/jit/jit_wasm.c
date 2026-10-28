/* jit/jit_wasm.c — jit.h'nin wasm uygulaması (plat_jit_instantiate import'u).
 * wasm32'de fonksiyon işaretçisi = tablo indeksi; indeks 0 LLVM'de null için ayrılmıştır.
 * Platform shade, prelude, shade_fast export'larını ardışık indekslere koyar.
 * Import'u sağlayan taraf: web/3d.js, tests/run.mjs, tools/bench*.mjs. */
#include "jit/jit.h"

IMPORT(plat_jit_instantiate) i32 plat_jit_instantiate(const u8* bytes, u32 len);

b32 jit_load(const u8* bytes, u32 len, b32 has_fast, JitShader* out) {
    out->shade = NULL;
    out->prelude = NULL;
    out->fast = NULL;
    i32 idx = plat_jit_instantiate(bytes, len);
    if (idx <= 0) return FALSE;
    out->shade = (ShadeFn)(uptr)(u32)idx;
    if (has_fast) {
        out->prelude = (PreludeFn)(uptr)(u32)(idx + 1);
        out->fast = (ShadeFn)(uptr)(u32)(idx + 2);
    }
    return TRUE;
}

ShadeFn jit_load_shade(const u8* bytes, u32 len) {
    JitShader s;
    return jit_load(bytes, len, FALSE, &s) ? s.shade : NULL;
}
