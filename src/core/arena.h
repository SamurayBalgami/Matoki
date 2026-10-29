/* core/arena.h — doğrusal (arena) bellek ayırıcı. malloc yok; tüm bellek buradan.
 * arena_alloc sıfırlanmış bellek döndürür; dolarsa sys_panic ile durur (başlangıç / kalıcı bellek: programlama hatası).
 * arena_try kare başına bellek içindir: dolarsa NULL + fails sayacı (çağıran işi atlar, motor durmaz).
 * Dizi boyutu (eleman × sayı) taşması her iki yolda da yakalanır. arena_mark / arena_reset kare sonu temizliği. */
#ifndef CORE_ARENA_H
#define CORE_ARENA_H
#include "core/types.h"

typedef struct Arena {
    u8*   base;
    usize cap;
    usize used;
    usize peak;
    u32   fails;   /* arena_try / arena_alloc_atomic başarısızlıkları (doluluk göstergesi) */
} Arena;

void  arena_init(Arena* a, void* mem, usize cap);
b32   arena_init_sys(Arena* a, usize cap);
void* arena_alloc(Arena* a, usize size, usize align);
void* arena_alloc_nozero(Arena* a, usize size, usize align);
/* thread güvenli (CAS), sıfırlamaz; dolarsa NULL (panik yok: worker'da tuzak = kilitlenme). peak güncellenmez. */
void* arena_alloc_atomic(Arena* a, usize size, usize align);
/* dolarsa NULL döner ve a->fails'i artırır (panik yok) */
void* arena_try(Arena* a, usize size, usize align);
void* arena_try_nozero(Arena* a, usize size, usize align);
/* eleman × sayı taşmasız; taşma = dolu */
void* arena_alloc_array(Arena* a, usize elem, usize count, usize align);
void* arena_try_array(Arena* a, usize elem, usize count, usize align);
void* arena_alloc_array_nozero(Arena* a, usize elem, usize count, usize align);
usize arena_mark(const Arena* a);
void  arena_reset(Arena* a, usize mark);
/* parent'tan cap baytlık alt arena (16 hizalı); alt arena ayrı sıfırlanır, parent'a geri verilmez */
Arena arena_sub(Arena* parent, usize cap);

#define ARENA_ARRAY(a, T, n) ((T*)arena_alloc_array((a), sizeof(T), (usize)(n), _Alignof(T)))
#define ARENA_ARRAY_NOZERO(a, T, n) ((T*)arena_alloc_array_nozero((a), sizeof(T), (usize)(n), _Alignof(T)))
#define ARENA_TRY_ARRAY(a, T, n) ((T*)arena_try_array((a), sizeof(T), (usize)(n), _Alignof(T)))
#define ARENA_NEW(a, T) ARENA_ARRAY(a, T, 1)

#endif
