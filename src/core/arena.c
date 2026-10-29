/* core/arena.c — arena ayırıcının uygulaması.
 * Hizalama 2'nin kuvveti olmalı. arena_alloc taşması = programlama hatası → sys_panic; arena_try NULL döner.
 * peak, en yüksek kullanım; fails, başarısız deneme sayısı (istatistik/ayar için). */
#include "core/arena.h"
#include "core/mem.h"
#include "core/sys.h"

void arena_init(Arena* a, void* mem, usize cap) {
    a->base = (u8*)mem;
    a->cap = cap;
    a->used = 0;
    a->peak = 0;
    a->fails = 0;
}

b32 arena_init_sys(Arena* a, usize cap) {
    void* mem = sys_reserve(cap);
    if (!mem) return FALSE;
    arena_init(a, mem, cap);
    return TRUE;
}

void* arena_try_nozero(Arena* a, usize size, usize align) {
    if (align < 1) align = 1;
    uptr p = (uptr)(a->base + a->used);
    uptr aligned = (p + (align - 1)) & ~(uptr)(align - 1);
    usize start = (usize)(aligned - (uptr)a->base);
    if (start > a->cap || size > a->cap - start) {
        a->fails++;
        return NULL;
    }
    a->used = start + size;
    if (a->used > a->peak) a->peak = a->used;
    return (void*)aligned;
}

void* arena_try(Arena* a, usize size, usize align) {
    void* p = arena_try_nozero(a, size, align);
    if (p) mem_zero(p, size);
    return p;
}

void* arena_alloc_nozero(Arena* a, usize size, usize align) {
    void* p = arena_try_nozero(a, size, align);
    if (!p) sys_panic("arena dolu");
    return p;
}

void* arena_alloc(Arena* a, usize size, usize align) {
    void* p = arena_alloc_nozero(a, size, align);
    mem_zero(p, size);
    return p;
}

static b32 array_bytes(usize elem, usize count, usize* out) {
    if (elem && count > (usize)-1 / elem) return FALSE;
    *out = elem * count;
    return TRUE;
}

void* arena_alloc_array(Arena* a, usize elem, usize count, usize align) {
    usize n;
    if (!array_bytes(elem, count, &n)) sys_panic("arena: dizi boyutu taşması");
    return arena_alloc(a, n, align);
}

void* arena_try_array(Arena* a, usize elem, usize count, usize align) {
    usize n;
    if (!array_bytes(elem, count, &n)) {
        a->fails++;
        return NULL;
    }
    return arena_try(a, n, align);
}

usize arena_mark(const Arena* a) { return a->used; }

void arena_reset(Arena* a, usize mark) {
    if (mark <= a->used) a->used = mark;
}

void* arena_alloc_atomic(Arena* a, usize size, usize align) {
    if (align < 1) align = 1;
    usize used = __atomic_load_n(&a->used, __ATOMIC_RELAXED);
    for (;;) {
        uptr p = (uptr)(a->base + used);
        uptr aligned = (p + (align - 1)) & ~(uptr)(align - 1);
        usize start = (usize)(aligned - (uptr)a->base);
        if (start > a->cap || size > a->cap - start) {
            __atomic_fetch_add(&a->fails, 1u, __ATOMIC_RELAXED);
            return NULL;
        }
        if (__atomic_compare_exchange_n(&a->used, &used, start + size, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            return (void*)aligned;
        }
    }
}