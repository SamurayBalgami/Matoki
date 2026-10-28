/* raster/ref_shaders.h — C referans örnekleme ve 3D referans shader'ları.
 * ref_sample_tex_mip, Matoki'nin @mipmap örneklemesi için bit-bit kâhindir (M8 §2.3).
 * 3D shader'lar standart varying yerleşimini (VAR3D_*) kullanır; gfx3d boru hattı bunu doldurur. */
#ifndef RASTER_REF_SHADERS_H
#define RASTER_REF_SHADERS_H
#include "core/math.h"
#include "img/image.h"
#include "raster/shade_abi.h"

/* 3D boru hattının varying yerleşimi */
#define VAR3D_WPOS  0   /* dünya konumu x,y,z */
#define VAR3D_NRM   3   /* dünya normali x,y,z (normalize edilmemiş) */
#define VAR3D_UV    6   /* u, v */
#define VAR3D_COUNT 8

/* ---- örnekleme ---- */
u32 ref_sample_level(const Texture* t, f32 u, f32 v, u32 level);     /* en yakın komşu */
u32 ref_mip_level_from_rho2(f32 rho2, u32 levels);
u32 ref_mip_level(const Texture* t, const f32 u[4], const f32 v[4]); /* quad türevlerinden */
void ref_sample_tex_mip(const Texture* t, const f32 u[4], const f32 v[4], u32 out[4]);

/* ---- 3D shader'lar ---- */
typedef struct LitUniforms {
    const Texture* tex;
    Vec3 to_light;      /* ışığa doğru birim vektör */
    f32  ambient;
    Vec3 light_color;
    f32  fog_start;
    Vec3 fog_color;
    f32  fog_end;
    Vec3 eye;
    b32  use_mip;
    Vec3 tint;
    u32  lv_count;                  /* lit_uniforms_prepare doldurur */
    TexLevel lv[TEX_MAX_LEVELS];
} LitUniforms;

/* tex değiştiğinde çağrılmalı: mip seviye tablosunu hesaplar. */
void lit_uniforms_prepare(LitUniforms* u);
/* quad'ın mip seviyesi (use_mip kapalıysa 0), lv_count'a kenetli */
u32  lit_mip_level(const LitUniforms* u, const f32 uu[4], const f32 vv[4]);

/* Üç uygulama, hepsi BİT-BİT aynı çıktı (test: shade_lit_variants_bit_exact):
 *  shade_lit_tex       — referans (her texel'de img_mip_offset)
 *  shade_lit_tex_lut   — skaler + seviye tablosu
 *  shade_lit_tex_simd  — wasm SIMD (f32x4) + seviye tablosu */
void shade_lit_tex(const ShadeQuad* q, const void* uniforms, u32 out[4]);
void shade_lit_tex_lut(const ShadeQuad* q, const void* uniforms, u32 out[4]);
#if defined(__wasm_simd128__)
void shade_lit_tex_simd(const ShadeQuad* q, const void* uniforms, u32 out[4]);
#endif

/* Ölçüm: neredeyse sıfır maliyetli shader (raster maliyetini shade'den ayırmak için) */
void shade_flat(const ShadeQuad* q, const void* uniforms, u32 out[4]);

/* Hata ayıklama: UV'yi renk olarak yaz */
void shade_uv_debug(const ShadeQuad* q, const void* uniforms, u32 out[4]);

#endif
