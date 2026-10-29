/* font/matoki_sans_data.h — Gömülü "Matoki Sans" TrueType (.ttf) yazı tipi ikili verisi.
 * tools/make_matoki_sans.py tarafından otomatik üretilmiştir; dış yazı tipi bağımlılığı yoktur.
 * Doğrudan ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &font) ile kullanılır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/matoki_sans_data.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef FONT_MATOKI_SANS_DATA_H
#define FONT_MATOKI_SANS_DATA_H

#include "core/types.h"

#define MATOKI_SANS_TTF_LEN 10512u

extern const u8  g_matoki_sans_ttf[MATOKI_SANS_TTF_LEN];
extern const u32 g_matoki_sans_ttf_len;

#endif
