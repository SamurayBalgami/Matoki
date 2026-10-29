/* core/fmt.h — libc'siz küçük metin oluşturucu.
 * StrBuf sabit kapasiteli bir tampona yazar; taşan kısım sessizce kesilir.
 * log_* yardımcıları satır sonu ekleyip sys_log'a gönderir.
 * fmt_format: printf alt kümesi (%d %i %u %x %X %p %f %c %s %%, genişlik, '-', '0', .kesinlik, l/ll/z);
 * LOG / PANIC / ASSERT bunun üstünde (MatokiEngine'den, D37). */
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

typedef __builtin_va_list VaList;
#define VA_START(ap, last) __builtin_va_start(ap, last)
#define VA_END(ap)         __builtin_va_end(ap)
#define VA_ARG(ap, T)      __builtin_va_arg(ap, T)

/* buf her zaman NUL ile biter (cap > 0); dönüş: yazılan karakter (NUL hariç, kesilmiş) */
u32  fmt_vformat(char* buf, u32 cap, const char* fmt, VaList ap);
u32  fmt_format(char* buf, u32 cap, const char* fmt, ...);
/* satır sonu ekler; satır en çok 1023 karakter */
void log_printf(const char* fmt, ...);
_Noreturn void log_panic(const char* file, i32 line, const char* fmt, ...);

#define LOG(...)   log_printf(__VA_ARGS__)
#define PANIC(...) log_panic(__FILE__, __LINE__, __VA_ARGS__)
#define ASSERT(c)  do { if (!(c)) log_panic(__FILE__, __LINE__, "ASSERT: %s", #c); } while (0)

#endif
