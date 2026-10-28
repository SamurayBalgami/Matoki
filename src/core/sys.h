/* core/sys.h — platformdan alınan asgari hizmetler.
 * sys_reserve: wasm belleğini büyüterek sıfırlanmış sayfa ayırır (geri verilmez).
 * sys_log: ham bayt dizisini platform günlüğüne yazar; sys_now_ms: monoton zaman. */
#ifndef CORE_SYS_H
#define CORE_SYS_H
#include "core/types.h"

void* sys_reserve(usize bytes);
void  sys_log(const char* s, usize n);
f64   sys_now_ms(void);
void  sys_panic(const char* msg);

#endif
