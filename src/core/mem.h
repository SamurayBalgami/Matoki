/* core/mem.h — bellek kopyalama/doldurma.
 * libc yok; derleyicinin örtük memcpy/memset çağrıları da mem.c'de tanımlı.
 * wasm'da -mbulk-memory ile memory.copy / memory.fill'e iner. */
#ifndef CORE_MEM_H
#define CORE_MEM_H
#include "core/types.h"

void mem_copy(void* dst, const void* src, usize n);
void mem_set(void* dst, u8 value, usize n);
void mem_zero(void* dst, usize n);
b32  mem_equal(const void* a, const void* b, usize n);

#endif
