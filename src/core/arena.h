/* core/arena.h — doğrusal (arena) bellek ayırıcı. malloc yok; tüm bellek buradan.
 * arena_alloc sıfırlanmış bellek döndürür; dolarsa sys_panic ile durur (sessiz NULL yok).
 * Kare başına geçici bellek için arena_mark / arena_reset kullanılır. */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H
#include "core/types.h"

typedef struct Arena {
    u8*   base;
    usize cap;
    usize used;
    usize peak;
} Arena;

void  arena_init(Arena* a, void* mem, usize cap);
b32   arena_init_sys(Arena* a, usize cap);
void* arena_alloc(Arena* a, usize size, usize align);
void* arena_alloc_nozero(Arena* a, usize size, usize align);
usize arena_mark(const Arena* a);
void  arena_reset(Arena* a, usize mark);

#define ARENA_ARRAY(a, T, n) ((T*)arena_alloc((a), sizeof(T) * (usize)(n), _Alignof(T)))
#define ARENA_NEW(a, T) ARENA_ARRAY(a, T, 1)

#endif
