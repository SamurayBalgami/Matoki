/* core/fmt.c — StrBuf ve günlük yardımcıları.
 * sb_f64 yaklaşıktır (sabit ondalık, |v| < 1e18); hata ayıklama/istatistik içindir,
 * gidiş-dönüş doğruluğu gereken yerlerde (JSON) kullanılmaz. */
#include "core/fmt.h"
#include "core/sys.h"

void sb_init(StrBuf* s, char* mem, u32 cap) {
    s->p = mem;
    s->len = 0;
    s->cap = cap;
}

void sb_char(StrBuf* s, char c) {
    if (s->len < s->cap) s->p[s->len++] = c;
}

void sb_cstr(StrBuf* s, const char* str) {
    while (*str) sb_char(s, *str++);
}

void sb_u64(StrBuf* s, u64 v) {
    char tmp[24];
    u32 n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n) sb_char(s, tmp[--n]);
}

void sb_i64(StrBuf* s, i64 v) {
    if (v < 0) {
        sb_char(s, '-');
        sb_u64(s, (u64)0 - (u64)v);
    } else {
        sb_u64(s, (u64)v);
    }
}

void sb_f64(StrBuf* s, f64 v, u32 decimals) {
    if (v != v) { sb_cstr(s, "nan"); return; }
    if (v < 0) { sb_char(s, '-'); v = -v; }
    if (v >= 1e18) { sb_cstr(s, "inf"); return; }
    if (decimals > 9) decimals = 9;
    u64 scale = 1;
    for (u32 i = 0; i < decimals; i++) scale *= 10;
    u64 whole = (u64)v;
    f64 frac = v - (f64)whole;
    u64 f = (u64)(frac * (f64)scale + 0.5);
    if (f >= scale) { whole++; f -= scale; }
    sb_u64(s, whole);
    if (decimals) {
        sb_char(s, '.');
        char tmp[10];
        for (u32 i = 0; i < decimals; i++) { tmp[decimals - 1 - i] = (char)('0' + f % 10); f /= 10; }
        for (u32 i = 0; i < decimals; i++) sb_char(s, tmp[i]);
    }
}

void sb_hex32(StrBuf* s, u32 v) {
    static const char digits[] = "0123456789abcdef";
    sb_cstr(s, "0x");
    for (i32 i = 7; i >= 0; i--) sb_char(s, digits[(v >> (i * 4)) & 15]);
}

usize cstr_len(const char* s) {
    usize n = 0;
    while (s[n]) n++;
    return n;
}

void log_cstr(const char* s) {
    sys_log(s, cstr_len(s));
    sys_log("\n", 1);
}

void log_sb(const StrBuf* s) {
    sys_log(s->p, s->len);
    sys_log("\n", 1);
}
