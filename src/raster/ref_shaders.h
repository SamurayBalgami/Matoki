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
/* çift doğrusal filtre, seviye içinde (out = RGBA 0..1); Matoki @bilinear kâhini */
void ref_sample_bilinear(const Texture* t, f32 u, f32 v, u32 level, f32 out[4]);

/* ---- 3D shader'lar ---- */
typedef struct LitUniforms {
    /* ---- kare (D56): motor her geçişte yazar; tüm malzemelerde aynı ---- */
    Vec3 eye;
    Vec3 to_light;      /* ışığa doğru birim vektör */
    f32  ambient;
    Vec3 light_color;
    f32  time;          /* s: su kırışıklığı, bulut rüzgârı */
    f32  screen_ix, screen_iy;   /* 1 / ekran boyu: water.mtk yansıma haritasını ekran konumundan okur (D47) */
    /* gölge (yalnız lit_hq.mtk, water.mtk; gfx3d/shadow.h): px f32 derinlik bitleri, texel_fetch + bitcast_f32 ile.
     * Kapalıyken shadow_none(): 1×1, derinlik 2 → hiçbir nokta gölgede değil, çarpan tam 1 (çıktı bit-bit aynı) */
    const Texture* shadow;
    Mat4 shadow_mat;    /* yakın kademe: dünya → (u, v, derinlik), u, v ∈ [0, 1] */
    f32  shadow_bias;   /* derinlik sapması (ışık uzayı, 0..1 ölçeğinde) */
    Mat4 shadow_mat2;   /* uzak kademe (D46) */
    f32  shadow_bias2;
    f32  shadow_nofs;   /* normal ofseti (m, D58): gölge sorgusu normal boyunca bu kadar dışarıdan; yakın kademe */
    f32  shadow_nofs2;  /* uzak kademe */
    const Texture* clouds;   /* bulut (D53): sky.mtk ile aynı doku; yoksa refl_none */
    /* ---- malzeme ---- */
    const Texture* tex;
    Vec3 tint;
    u32  use_mip;       /* bitler: LIT_MIP; LIT_BEVEL, LIT_FIELD_FINE yalnız lit_hq.mtk (D55, D58) */
    f32  fog_start;
    f32  fog_end;
    Vec3 fog_color;
    /* yalnızca lit_hq.mtk (lit.mtk ve C shader'ları okumaz) */
    f32  wrap;          /* diffuse: 0 = Lambert, 1 = half-Lambert² (yumuşak, kıvrımlar belli) */
    f32  translucency;  /* arkadan gelen ışığın geçen payı (kumaş) */
    f32  metal;         /* 0 yalıtkan, 1 metal (D48) */
    f32  rough;         /* pürüz: ≥ 0,97 → parlak kol çalışmaz */
    f32  bump;          /* doku parlaklığı → yükseklik (m) */
    /* fizik alanı (D49): R gerilme, G katlanma, B zemin lekesi; fizik modülü yazar, malzeme okur. water.mtk burada dalga alanını okur */
    const Texture* field;
    f32  field_scale;   /* uv → alan dokusu; 0 = alan yok, kol çalışmaz */
    /* ---- yalnız CPU: GPU'ya gitmez ---- */
    u32  lv_count;                  /* lit_uniforms_prepare doldurur */
    TexLevel lv[TEX_MAX_LEVELS];
} LitUniforms;

#define LIT_MIP   1u   /* mip seviyesi seçilir */
#define LIT_BEVEL 2u   /* kutu kenarına pah: doku koordinatının kenarından türetilen yükseklik (D55) */
#define LIT_FIELD_FINE 4u   /* fizik alanı piksel başına okunur (küçük cisim: bayrak, jöle); yoksa quad başına (D58) */

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
