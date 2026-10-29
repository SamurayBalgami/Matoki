/* ui/bitfont.h — MATOKI özgün 8x8 ve 8x16 bitmap font ve UTF-8 metin çizici.
 * ASCII 32..126 ve Türkçe karakterler (ç Ç ğ Ğ ı İ ö Ö ş Ş ü Ü) için el tasarımı glifler.
 * UTF-8 kod çözme (geçersiz dizilerde U+FFFD '?' ikamesi), alfa harmanlamalı çizim ve ölçüm.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/ui/font.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef UI_BITFONT_H
#define UI_BITFONT_H

#include "core/types.h"
#include "raster/raster.h"

/* Türkçe Unicode kod noktaları */
#define FONT_CP_C_CEDILLA_UPPER 0x00C7u /* Ç */
#define FONT_CP_O_UMLAUT_UPPER  0x00D6u /* Ö */
#define FONT_CP_U_UMLAUT_UPPER  0x00DCu /* Ü */
#define FONT_CP_C_CEDILLA_LOWER 0x00E7u /* ç */
#define FONT_CP_O_UMLAUT_LOWER  0x00F6u /* ö */
#define FONT_CP_U_UMLAUT_LOWER  0x00FCu /* ü */
#define FONT_CP_G_BREVE_UPPER   0x011Eu /* Ğ */
#define FONT_CP_G_BREVE_LOWER   0x011Fu /* ğ */
#define FONT_CP_I_DOTTED_UPPER  0x0130u /* İ */
#define FONT_CP_I_DOTLESS_LOWER 0x0131u /* ı */
#define FONT_CP_S_CEDILLA_UPPER 0x015Eu /* Ş */
#define FONT_CP_S_CEDILLA_LOWER 0x015Fu /* ş */

/* UTF-8 dizisinden bir sonraki Unicode kod noktasını okur ve işaretçiyi ilerletir.
 * Bozuk/eksik UTF-8 baytlarında 0xFFFD döner ve en az 1 bayt ilerler (sonsuz döngü olmaz). */
u32         bitfont_utf8_next(const char** str);

/* Verilen Unicode kod noktasının 8x8 veya 8x16 glif satır baytlarını döner (MSB = sol piksel).
 * Tanımsız kod noktaları için '?' glifini döner (asla NULL dönmez). */
const u8*   bitfont_glyph_8x8(u32 codepoint);
const u8*   bitfont_glyph_8x16(u32 codepoint);

/* Kod noktasının tabloda açıkça tanımlı olup olmadığını döner (test doğrulaması için). */
b32         bitfont_has_glyph(u32 codepoint);

/* 8x8 font ile metin çizimi (scale >= 1; '\n' yeni satıra geçer) */
void        bitfont_draw(Framebuffer* fb, i32 x, i32 y, const char* text, u32 rgba, i32 scale);

/* Kırpma dikdörtgeni (clip rect) içinde 8x8 metin çizimi */
void        bitfont_draw_clipped(Framebuffer* fb, i32 x, i32 y, const char* text, u32 rgba, i32 scale,
                              i32 clip_x0, i32 clip_y0, i32 clip_x1, i32 clip_y1);

/* 8x16 font ile metin çizimi */
void        bitfont_draw_8x16(Framebuffer* fb, i32 x, i32 y, const char* text, u32 rgba, i32 scale);
void        bitfont_draw_8x16_clipped(Framebuffer* fb, i32 x, i32 y, const char* text, u32 rgba, i32 scale,
                                   i32 clip_x0, i32 clip_y0, i32 clip_x1, i32 clip_y1);

/* Metin ölçümü (piksel cinsinden genişlik ve yükseklik) */
i32         bitfont_measure_width(const char* text, i32 scale);
i32         bitfont_measure_height(const char* text, i32 scale);
void        bitfont_measure_8x8(const char* text, i32 scale, i32* out_w, i32* out_h);
void        bitfont_measure_8x16(const char* text, i32 scale, i32* out_w, i32* out_h);

#endif
