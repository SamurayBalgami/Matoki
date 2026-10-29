/* font/atlas.h — TrueType gliflerini raf (shelf) algoritmasıyla tek bir u8 alfa atlasına
 * paketleyen FontAtlas modülü. ASCII (32..126), Latin-1 (160..255) ve Türkçe (ğĞıİşŞçÇöÖüÜ)
 * karakterlerini içerir; UTF-8 çözme ve kerning destekli metin ölçümü (font_measure) sunar.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/atlas.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef FONT_ATLAS_H
#define FONT_ATLAS_H

#include "core/types.h"
#include "core/arena.h"
#include "font/ttf.h"
#include "font/raster.h"

#define FONT_ATLAS_FAST_CP_CAP 384u

typedef struct {
    u32 codepoint;
    u16 glyph_index;
    u16 x, y, w, h;       /* atlas içi piksel kutusu */
    i32 x0, y0, x1, y1;   /* taban çizgisine göre tamsayı piksel kutusu */
    f32 advance;          /* yatay ilerleme (piksel) */
    f32 bearing_x;        /* sol yan boşluk (piksel) */
    f32 bearing_y;        /* üst kenar ofseti (piksel, -y0) */
    f32 u0, v0, u1, v1;   /* normalize [0, 1] atlas UV koordinatları */
} FontGlyph;

typedef struct {
    const TtfFont* font;
    f32            px_height;
    f32            scale;       /* px_height / units_per_em */
    f32            ascent;      /* piksel cinsinden ascent */
    f32            descent;     /* piksel cinsinden descent */
    f32            line_gap;    /* piksel cinsinden line_gap */
    f32            line_height; /* satır yüksekliği (piksel) */
    u32            w, h;        /* atlas boyutları */
    u8*            px;          /* u8 alfa pikselleri [w * h] */
    u32            glyph_count;
    FontGlyph*     glyphs;      /* paketlenen glifler [glyph_count] */
    u16            cp_map[FONT_ATLAS_FAST_CP_CAP]; /* cp < 384 için (indeks + 1), 0 = yok */
} FontAtlas;

/* Verilen piksel yüksekliği için ASCII + Latin-1 + Türkçe gliflerini raf paketlemeyle
 * tek bir u8 alfa atlasına rasterler. atlas_w/atlas_h 0 ise boyut otomatik seçilir. */
b32 font_atlas_build(const TtfFont* font, f32 px_height, u32 atlas_w, u32 atlas_h,
                     Arena* out_arena, Arena* scratch, FontAtlas* out);

/* Kod noktasına karşılık gelen paketlenmiş glifi döner (bulunamazsa NULL). */
const FontGlyph* font_atlas_glyph(const FontAtlas* atlas, u32 codepoint);

/* UTF-8 dizgesinden bir sonraki Unicode kod noktasını okur ve işaretçiyi ilerletir. */
u32 font_atlas_utf8_next(const char** str);

/* UTF-8 dizgesinin kerning uygulanmış piksel genişliğini ve yüksekliğini ölçer; genişliği döner. */
f32 font_measure(const FontAtlas* atlas, const char* utf8, f32* out_w, f32* out_h);

#endif
