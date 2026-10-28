/* core/mem.c — bellek yardımcıları ve derleyicinin beklediği memcpy/memset/memmove.
 * __builtin_* çağrıları bulk-memory ile tek komuta iner; özyineleme olmaz.
 * memcmp karşılığı mem_equal basit bayt döngüsüdür (sıcak yolda kullanılmaz). */
#include "core/mem.h"

void* memcpy(void* dst, const void* src, usize n);
void* memmove(void* dst, const void* src, usize n);
void* memset(void* dst, int c, usize n);

void* memcpy(void* dst, const void* src, usize n) { return __builtin_memcpy(dst, src, n); }
void* memmove(void* dst, const void* src, usize n) { return __builtin_memmove(dst, src, n); }
void* memset(void* dst, int c, usize n) { return __builtin_memset(dst, c, n); }

void mem_copy(void* dst, const void* src, usize n) { __builtin_memmove(dst, src, n); }
void mem_set(void* dst, u8 value, usize n) { __builtin_memset(dst, value, n); }
void mem_zero(void* dst, usize n) { __builtin_memset(dst, 0, n); }

b32 mem_equal(const void* a, const void* b, usize n) {
    const u8* pa = (const u8*)a;
    const u8* pb = (const u8*)b;
    for (usize i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return FALSE;
    }
    return TRUE;
}
