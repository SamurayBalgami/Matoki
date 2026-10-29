/* font/raster.h — İkinci derece Bezier düzleştirici ve işaretli alan (signed-area) tabanlı
 * kenar yumuşatmalı (AA) glif rasterleyici. Nonzero sargı kuralı ve alt piksel x/y kaydırması
 * destekler; dış kütüphane veya kod kopyası kullanmadan sıfırdan uygulanmıştır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/raster.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef FONT_RASTER_H
#define FONT_RASTER_H

#include "core/types.h"
#include "core/arena.h"
#include "font/ttf.h"

#define FONT_DEFAULT_FLATNESS 0.05f

typedef struct {
    f32 x0, y0;
    f32 x1, y1;
} FontEdge;

/* Ölçeklenmiş ve alt-piksel kaydırılmış glifin tamsayı piksel sınırlayıcı kutusunu döner.
 * Ekran uzayında +x sağa, +y aşağıdır (taban çizgisi y = 0). */
void font_glyph_bbox(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                     f32 shift_x, f32 shift_y,
                     i32* out_x0, i32* out_y0, i32* out_x1, i32* out_y1);

/* Glif konturlarını ikinci derece Bezier düzleştirmesiyle doğru parçalarına (FontEdge) çevirir. */
u32  font_flatten_glyph(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                        f32 offset_x, f32 offset_y, f32 flatness,
                        Arena* arena, FontEdge** out_edges);

/* Doğru parçalarını [0..w) x [0..h) piksel tamponuna işaretli alan birikimiyle rasterler. */
b32  font_rasterize_edges(const FontEdge* edges, u32 edge_count,
                          u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch);

/* Glifi verilen piksel ofsetiyle (x_px = x * scale_x + offset_x, y_px = -y * scale_y + offset_y)
 * doğrudan [0..w) x [0..h) tamponuna rasterler. */
b32  font_rasterize_glyph_ex(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                             f32 offset_x, f32 offset_y,
                             u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch);

/* Glifi kendi sınırlayıcı kutusunun sol-üst köşesine hizalayarak rasterler. */
b32  font_rasterize_glyph(const TtfGlyph* glyph, f32 scale_x, f32 scale_y,
                          f32 shift_x, f32 shift_y,
                          u8* out_px, u32 w, u32 h, u32 stride, Arena* scratch);

#endif
