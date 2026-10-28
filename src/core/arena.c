/* core/arena.c — arena ayırıcının uygulaması.
 * Hizalama 2'nin kuvveti olmalı. Taşma = programlama hatası → sys_panic.
 * peak, en yüksek kullanım (istatistik/ayar için). */
#include "core/arena.h"
#include "core/mem.h"
#include "core/sys.h"

void arena_init(Arena* a, void* mem, usize cap) {
    a->base = (u8*)mem;
    a->cap = cap;
    a->used = 0;
    a->peak = 0;
}

b32 arena_init_sys(Arena* a, usize cap) {
    void* mem = sys_reserve(cap);
    if (!mem) return FALSE;
    arena_init(a, mem, cap);
    return TRUE;
}

void* arena_alloc_nozero(Arena* a, usize size, usize align) {
    if (align < 1) align = 1;
    uptr p = (uptr)(a->base + a->used);
    uptr aligned = (p + (align - 1)) & ~(uptr)(align - 1);
    usize start = (usize)(aligned - (uptr)a->base);
    if (start > a->cap || size > a->cap - start) {
        sys_panic("arena dolu");
        return NULL;
    }
    a->used = start + size;
    if (a->used > a->peak) a->peak = a->used;
    return (void*)aligned;
}

void* arena_alloc(Arena* a, usize size, usize align) {
    void* p = arena_alloc_nozero(a, size, align);
    mem_zero(p, size);
    return p;
}

usize arena_mark(const Arena* a) { return a->used; }

void arena_reset(Arena* a, usize mark) {
    if (mark <= a->used) a->used = mark;
}
