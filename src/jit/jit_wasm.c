/* jit/jit_wasm.c — jit.h'nin wasm uygulaması (plat_jit_instantiate import'u) ve worker kayıt defteri.
 * wasm32'de fonksiyon işaretçisi = tablo indeksi; indeks 0 LLVM'de null için ayrılmıştır.
 * Platform shade, prelude, shade_fast export'larını ardışık indekslere koyar.
 * Defter: modül baytlarının kopyası ayrı bir arenada (çağıranın arenası sıfırlanabilir); worker'lar
 * jit_count / jit_entry ile okur (web/worker.mjs). Defter doluysa modül yalnızca ana thread'de çalışır. */
#include "jit/jit.h"
#include "core/arena.h"
#include "core/mem.h"
#include "core/fmt.h"

IMPORT(plat_jit_instantiate) i32 plat_jit_instantiate(const u8* bytes, u32 len, u32 cache, u32 cache_stride);

#define JIT_MAX_MODULES 4096
#define JIT_BYTES_CAP   ((usize)32 << 20)

typedef struct JitEntry {
    u32 bytes, len;       /* defter arenasındaki kopya */
    u32 table_index;
    u32 cache, cache_stride;
} JitEntry;

static JitEntry g_entries[JIT_MAX_MODULES];
static u32 g_count;
static Arena g_bytes;
static b32 g_bytes_ok, g_full_logged;

static void registry_add(const u8* bytes, u32 len, i32 idx, void* cache, u32 stride) {
    if (!g_bytes_ok) g_bytes_ok = arena_init_sys(&g_bytes, JIT_BYTES_CAP);
    if (!g_bytes_ok || g_count >= JIT_MAX_MODULES || len > g_bytes.cap - g_bytes.used) {
        if (!g_full_logged) log_cstr("jit: kayıt defteri dolu; yeni modüller yalnızca ana thread'de");
        g_full_logged = TRUE;
        return;
    }
    u8* copy = (u8*)arena_alloc_nozero(&g_bytes, len, 1);
    mem_copy(copy, bytes, len);
    JitEntry* e = &g_entries[g_count];
    e->bytes = (u32)(uptr)copy;
    e->len = len;
    e->table_index = (u32)idx;
    e->cache = (u32)(uptr)cache;
    e->cache_stride = stride;
    __atomic_store_n(&g_count, g_count + 1, __ATOMIC_SEQ_CST);   /* worker'lar girdiyi tam görür */
}

b32 jit_load(const u8* bytes, u32 len, void* cache, u32 cache_stride, b32 has_fast, JitShader* out) {
    out->shade = NULL;
    out->prelude = NULL;
    out->fast = NULL;
    i32 idx = plat_jit_instantiate(bytes, len, (u32)(uptr)cache, cache_stride);
    if (idx <= 0) return FALSE;
    registry_add(bytes, len, idx, cache, cache_stride);
    out->shade = (ShadeFn)(uptr)(u32)idx;
    if (has_fast) {
        out->prelude = (PreludeFn)(uptr)(u32)(idx + 1);
        out->fast = (ShadeFn)(uptr)(u32)(idx + 2);
    }
    return TRUE;
}

EXPORT(jit_count) u32 jit_count(void) { return __atomic_load_n(&g_count, __ATOMIC_SEQ_CST); }
EXPORT(jit_entry) const JitEntry* jit_entry(u32 i) { return i < g_count ? &g_entries[i] : NULL; }
