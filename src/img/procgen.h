/* img/procgen.h — prosedürel doku üreteçleri (dış varlık yok).
 * Hepsi yalnızca seviye 0'ı doldurur; mip için ardından tex_build_mips çağrılır.
 * Malzeme dokuları (D52) kenardan kenara dikişsiz, kare maliyeti yok: başlangıçta bir kez üretilir.
 * Renkler 0xAABBGGRR. */
#ifndef IMG_PROCGEN_H
#define IMG_PROCGEN_H
#include "raster/shade_abi.h"

void procgen_checker(Texture* t, u32 cell, u32 c0, u32 c1);
void procgen_grid(Texture* t, u32 cell, u32 bg, u32 line);
void procgen_bricks(Texture* t, u32 seed);
/* beton döşeme: slabs × slabs taş, satır başına yarım kayık, taş başına ton, leke, derz */
void procgen_pavers(Texture* t, u32 slabs, u32 seed);
/* kaya / kauçuk: damarlı gri gürültü */
void procgen_rock(Texture* t, u32 seed);
/* çim: yeşil tonları, kuru lekeler */
void procgen_grass(Texture* t, u32 seed);
/* dokuma kumaş: ince atkı-çözgü, tint ile boyanır */
void procgen_cloth(Texture* t, u32 seed);
/* yıpranmış boya: tint ile boyanır, kenarda kir, yüzeyde leke ve çizik */
void procgen_paint(Texture* t, u32 seed);
/* büyük ölçekli çeşitlilik: B = 0..255 leke, diğer kanallar 0; zemin alanının statik tabanı (D53) */
void procgen_macro(Texture* t, u32 seed);
/* bulut: R yoğunluk, G güneş tarafından aydınlanma (1 = aydınlık); (sun_x, sun_y) doku düzleminde güneş yönü */
void procgen_clouds(Texture* t, u32 seed, f32 sun_x, f32 sun_y);

#endif
