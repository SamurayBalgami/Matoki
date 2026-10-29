/* core/sys_wasm.c — sys.h'nin wasm uygulaması.
 * Bellek memory.grow ile büyütülür; JS tarafı plat_log / plat_now_ms import'larını sağlar
 * (web/3d.js ve tests/run.mjs). */
#include "core/sys.h"

IMPORT(plat_log) void plat_log(const char* p, u32 n);
IMPORT(plat_now_ms) f64 plat_now_ms(void);

void* sys_reserve(usize bytes) {
    usize pages = (bytes + 65535u) / 65536u;
    usize old = __builtin_wasm_memory_grow(0, pages);
    if (old == (usize)-1) return NULL;
    return (void*)(old * 65536u);
}

void sys_log(const char* s, usize n) { plat_log(s, (u32)n); }

f64 sys_now_ms(void) { return plat_now_ms(); }

_Noreturn void sys_panic(const char* msg) {
    usize n = 0;
    while (msg[n]) n++;
    sys_log("PANIC: ", 7);
    sys_log(msg, n);
    sys_log("\n", 1);
    __builtin_trap();
}
