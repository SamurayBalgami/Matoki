/* gfx3d/lit_layout.c — LitUniforms alanlarının Matoki tablosu (D56). Her alan bir kez, kendi adıyla: takma ad yok.
 * Kare alanları (göz, güneş, gölge, bulut, zaman, ekran) motorun her karede yazdıkları; malzeme alanları malzemenin kendi değerleri.
 * Yeni alan: ref_shaders.h'deki yapıya ekle, buraya bir satır; shader adla kullanır. */
#include "gfx3d/lit_layout.h"
#include "raster/ref_shaders.h"

#define F(f, b, k) { #f, (u32)__builtin_offsetof(LitUniforms, f), b, k }
static const MatokiUniformField LIT_FIELDS[] = {
    /* kare */
    F(eye, TB_FLOAT, 3), F(to_light, TB_FLOAT, 3), F(ambient, TB_FLOAT, 1), F(light_color, TB_FLOAT, 3),
    F(time, TB_FLOAT, 1), F(screen_ix, TB_FLOAT, 1), F(screen_iy, TB_FLOAT, 1),
    F(shadow, TB_TEX, 1), F(shadow_mat, TB_MAT, 4), F(shadow_bias, TB_FLOAT, 1), F(shadow_mat2, TB_MAT, 4), F(shadow_bias2, TB_FLOAT, 1),
    F(clouds, TB_TEX, 1),
    /* malzeme */
    F(tex, TB_TEX, 1), F(tint, TB_FLOAT, 3), F(use_mip, TB_INT, 1),
    F(fog_start, TB_FLOAT, 1), F(fog_end, TB_FLOAT, 1), F(fog_color, TB_FLOAT, 3),
    F(wrap, TB_FLOAT, 1), F(translucency, TB_FLOAT, 1), F(metal, TB_FLOAT, 1), F(rough, TB_FLOAT, 1), F(bump, TB_FLOAT, 1),
    F(field, TB_TEX, 1), F(field_scale, TB_FLOAT, 1),
};
#undef F

const MatokiLayout LIT_LAYOUT = { LIT_FIELDS, (u32)ARRAY_COUNT(LIT_FIELDS) };
