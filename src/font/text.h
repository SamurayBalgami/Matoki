/* font/text.h — TrueType FontAtlas tabanlı UTF-8 metin ölçümü, satır kaydırma (word wrap)
 * ve Framebuffer üzerine alfa harmanlamalı metin çizimi.
 * Renk, kerning, sol/orta/sağ hizalama, gölge ve kontur (outline) stillerini destekler.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/text.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef FONT_TEXT_H
#define FONT_TEXT_H

#include "core/types.h"
#include "raster/raster.h"
#include "font/atlas.h"

/* Hostile/bozuk dizgelere karşı üst bayt ve kontur sınırları */
#define TEXT_MAX_INPUT_BYTES (1u << 20)
#define TEXT_MAX_OUTLINE_PX  8

typedef enum {
    TEXT_ALIGN_LEFT   = 0,
    TEXT_ALIGN_CENTER = 1,
    TEXT_ALIGN_RIGHT  = 2
} TextAlign;

typedef struct {
    u32       color;          /* Ana metin rengi: color_pack(r, g, b, a) */
    TextAlign align;          /* Satır hizalaması: SOL / ORTA / SAĞ */
    f32       max_width;      /* > 0 ise otomatik satır kaydırma (word wrap) genişliği (piksel) */
    f32       line_spacing;   /* Satır yüksekliği çarpanı (<= 0 ise varsayılan 1.0f) */
    f32       letter_spacing; /* Ek harf arası boşluk (piksel, varsayılan 0.0f) */
    b32       no_kerning;     /* FALSE (0) ise kerning aktif, TRUE ise devre dışı */
    u32       shadow_color;   /* Alfa > 0 ise gölge çizilir: color_pack(r, g, b, a) */
    i32       shadow_dx;      /* Gölge x ofseti (piksel) */
    i32       shadow_dy;      /* Gölge y ofseti (piksel) */
    u32       outline_color;  /* Alfa > 0 ve outline_px > 0 ise kontur çizilir */
    i32       outline_px;     /* Kontur yarıçapı (piksel, 1..TEXT_MAX_OUTLINE_PX) */
    b32       y_is_baseline;  /* TRUE ise y doğrudan ilk satırın taban çizgisidir (baseline) */
} TextStyle;

typedef struct {
    const char* start;    /* Satırın girdi UTF-8 dizgesi içindeki başlangıç adresi */
    u32         byte_len; /* Satırın bayt uzunluğu ('\n' ve kırpılan son boşluklar hariç) */
    f32         width;    /* Satırın kerning uygulanmış piksel genişliği */
} TextLine;

/* Varsayılan metin stili oluşturucu (kerning açık, sola hizalı, sarmalamasız). */
static inline TextStyle text_style_default(u32 color) {
    TextStyle s = {0};
    s.color = color;
    return s;
}

/* UTF-8 metnini verilen stile (kerning, max_width ile word wrap, line_spacing) göre ölçer.
 * En geniş satırın piksel genişliğini döner; out_w ve out_h doluysa genişlik/yükseklik yazar. */
f32  text_measure(const FontAtlas* atlas, const char* utf8, const TextStyle* style,
                  f32* out_w, f32* out_h);

/* UTF-8 metnini max_width piksel genişliğine göre kelime bölmeden satırlara ayırır
 * (tek kelime max_width'ten uzunsa karakter bazlı böler). Toplam satır sayısını döner;
 * out_lines != NULL ise en fazla max_lines satır bilgisini doldurur. */
u32  text_wrap(const FontAtlas* atlas, const char* utf8, f32 max_width,
               TextLine* out_lines, u32 max_lines);

/* Stil parametreli (no_kerning, letter_spacing destekli) satır kaydırma yardımcısı. */
u32  text_wrap_ex(const FontAtlas* atlas, const char* utf8, f32 max_width,
                  const TextStyle* style, TextLine* out_lines, u32 max_lines);

/* UTF-8 metnini Framebuffer üzerine alfa harmanlamalı olarak çizer.
 * Tüm koordinatlar [0..((u32)fb->w)) x [0..((u32)fb->h)) sınırlarına güvenle kırpılır. */
void text_draw(Framebuffer* fb, const FontAtlas* atlas, i32 x, i32 y,
               const char* utf8, const TextStyle* style);

#endif
