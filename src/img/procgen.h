/* img/procgen.h — prosedürel doku üreteçleri (dış varlık yok).
 * Hepsi yalnızca seviye 0'ı doldurur; mip için ardından tex_build_mips çağrılır.
 * Renkler 0xAABBGGRR. */
#ifndef IMG_PROCGEN_H
#define IMG_PROCGEN_H
#include "raster/shade_abi.h"

void procgen_checker(Texture* t, u32 cell, u32 c0, u32 c1);
void procgen_grid(Texture* t, u32 cell, u32 bg, u32 line);
void procgen_bricks(Texture* t, u32 seed);

#endif
