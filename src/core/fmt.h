/* core/fmt.h — libc'siz küçük metin oluşturucu (printf yok).
 * StrBuf sabit kapasiteli bir tampona yazar; taşan kısım sessizce kesilir.
 * log_* yardımcıları satır sonu ekleyip sys_log'a gönderir. */
#ifndef CORE_FMT_H
#define CORE_FMT_H
#include "core/types.h"

typedef struct StrBuf {
    char* p;
    u32   len;
    u32   cap;
} StrBuf;

void sb_init(StrBuf* s, char* mem, u32 cap);
void sb_char(StrBuf* s, char c);
void sb_cstr(StrBuf* s, const char* str);
void sb_u64(StrBuf* s, u64 v);
void sb_i64(StrBuf* s, i64 v);
void sb_f64(StrBuf* s, f64 v, u32 decimals);
void sb_hex32(StrBuf* s, u32 v);

usize cstr_len(const char* s);
void  log_cstr(const char* s);
void  log_sb(const StrBuf* s);

#endif
