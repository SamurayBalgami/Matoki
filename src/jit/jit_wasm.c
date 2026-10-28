/* jit/jit_wasm.c — jit.h'nin wasm uygulaması (plat_jit_instantiate import'u).
 * wasm32'de fonksiyon işaretçisi = tablo indeksi; indeks 0 LLVM'de null için ayrılmıştır.
 * Import'u sağlayan taraf: web/3d.js ve tests/run.mjs. */
#include "jit/jit.h"

IMPORT(plat_jit_instantiate) i32 plat_jit_instantiate(const u8* bytes, u32 len);

ShadeFn jit_load_shade(const u8* bytes, u32 len) {
    i32 idx = plat_jit_instantiate(bytes, len);
    if (idx <= 0) return NULL;
    return (ShadeFn)(uptr)(u32)idx;
}
