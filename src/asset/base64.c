/* asset/base64.c — 4 karakter → 3 bayt çözme; dolgu yalnız sonda ve en fazla 2 karakter olabilir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/base64.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "asset/base64.h"

static i32 b64_val(u8 c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;   /* '-' URL-güvenli alfabe */
    if (c == '/' || c == '_') return 63;
    return -1;
}

u32 base64_max_decoded(u32 text_len) {
    return (text_len / 4u + 1u) * 3u;
}

b32 base64_decode(const char* text, u32 len, u8* out, u32 out_cap, u32* out_len) {
    u32 acc = 0, bits = 0, n = 0, pad = 0;
    for (u32 i = 0; i < len; i++) {
        u8 c = (u8)text[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; continue; }
        if (pad) return FALSE;             /* dolgudan sonra veri */
        i32 v = b64_val(c);
        if (v < 0) return FALSE;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return FALSE;
            out[n++] = (u8)(acc >> bits);
        }
    }
    if (pad > 2 || bits >= 6) return FALSE;
    if ((acc & ((1u << bits) - 1u)) != 0) return FALSE;   /* kullanılmayan bitler sıfır olmalı */
    *out_len = n;
    return TRUE;
}
