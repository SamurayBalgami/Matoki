/* img/filter.h — RGBA8 piksel dizileri (0xAABBGGRR) için görüntü işleme.
 * Bulanıklık ayrılabilir (yatay + dikey), O(w·h·r); kenarlarda kenara kenetlenir. Geçici bellek scratch'ten.
 * Normal haritası: parlaklıktan 3×3 Sobel, teğet uzayı (z yukarı), wrap ile döşenebilir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/img/image.h), izinle uyarlandı (D37). */
#ifndef IMG_FILTER_H
#define IMG_FILTER_H
#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

void img_blur_box(u32* px, u32 w, u32 h, i32 radius, Arena* scratch);
/* radius en çok 64; çekirdek toplamı 1'e normalize */
void img_blur_gaussian(u32* px, u32 w, u32 h, i32 radius, f32 sigma, Arena* scratch);

/* piksel merkezleri hizalı çift doğrusal; kutu: hedef pikselin kapladığı kaynak alanın ortalaması */
void img_resize_bilinear(const u32* src, u32 src_w, u32 src_h, u32* dst, u32 dst_w, u32 dst_h);
void img_resize_box(const u32* src, u32 src_w, u32 src_h, u32* dst, u32 dst_w, u32 dst_h);

void img_normal_from_height(const u32* src_rgba, u32* dst_rgba, u32 w, u32 h, f32 strength, b32 wrap);

/* parlaklık [-1, 1], kontrast [0, 4], doygunluk [0, 4], gama (1 = değişmez); alfa korunur */
void img_color_grade(u32* px, u32 w, u32 h, f32 brightness, f32 contrast, f32 saturation, f32 gamma);
/* R, G, B *= A / 255 (yuvarlamalı) */
void img_premultiply_alpha(u32* px, u32 w, u32 h);

#endif
