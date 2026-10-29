/* asset/base64.h — RFC 4648 base64 çözücü (glTF "data:" URI'leri için).
 * Boşlukları atlar, '=' dolgusunu doğrular, geçersiz karakterde FALSE döner.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/base64.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ASSET_BASE64_H
#define ASSET_BASE64_H

#include "core/types.h"

/* Çıktı için gereken en büyük bayt sayısı */
u32 base64_max_decoded(u32 text_len);
/* out_cap yeterli olmalı; başarıda *out_len yazılan bayt sayısı */
b32 base64_decode(const char* text, u32 len, u8* out, u32 out_cap, u32* out_len);

#endif
