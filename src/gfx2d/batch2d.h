/* gfx2d/batch2d.h — 2D sprite yığınlayıcı: her sprite 2 üçgen, bizim ertelenmiş rasterizer'a (raster_triangle).
 * Kamera Mat3 (dünya → ekran pikseli), döndürme, ölçek ve UV kırpma köşelerde hesaplanır; durum (doku, renk
 * çarpanı, shader) değişince kare arenasından yeni uniform bloğu alınır (raster durumu üçgen başına saklar).
 * Karışım raster'ın BlendMode'u ile (düz alfa / toplama); kesik mod alfa < 0,5'i atar (derinlik yazan sprite için).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/batch2d.h), bizim raster API'sine uyarlandı (D37). */
#ifndef GFX2D_BATCH2D_H
#define GFX2D_BATCH2D_H
#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/raster.h"

typedef struct Rect {
    f32 x, y, w, h;
} Rect;

static inline Rect rect(f32 x, f32 y, f32 w, f32 h) {
    Rect r = { x, y, w, h };
    return r;
}

typedef enum B2dBlend {
    B2D_OPAQUE,   /* doku rengi olduğu gibi */
    B2D_CUTOUT,   /* alfa × renk çarpanı alfası < 0,5 → piksel atılır */
    B2D_ALPHA,    /* düz alfa karışımı (BLEND_ALPHA) */
    B2D_ADD       /* toplamalı (BLEND_ADD): ışık, kıvılcım */
} B2dBlend;

/* varsayılan sprite shader'ının uniform'ları (var0 = u, var1 = v) */
typedef struct SpriteUniforms {
    const Texture* tex;
    f32 tint[4];      /* RGBA çarpanı 0..1 */
    u32 cutout;       /* B2D_CUTOUT */
} SpriteUniforms;

typedef struct Batch2D {
    Raster*        raster;
    Arena*         frame;      /* uniform blokları (raster_flush'a kadar yaşamalı) */
    Mat3           camera;
    b32            cam_is_identity;
    ShadeFn        shade_fn;   /* uniform'u SpriteUniforms olan shader */
    B2dBlend       blend;
    BlendMode      saved_blend;   /* b2d_begin'deki raster karışımı; b2d_end geri koyar */
    const Texture* cur_tex;
    Vec4           cur_tint;
    b32            state_dirty;
    u32            sprites;    /* çizilen sprite sayısı */
} Batch2D;

void b2d_begin(Batch2D* b, Raster* r, Arena* frame, Mat3 camera);
void b2d_set_blend(Batch2D* b, B2dBlend blend);
void b2d_set_shader(Batch2D* b, ShadeFn shade_fn);
/* src_rect: piksel (w ya da h > 1) ya da 0..1 UV; dst_pos sprite merkezi (dünya); rot radyan */
void b2d_sprite(Batch2D* b, const Texture* tex, Rect src_rect, Vec2 dst_pos, Vec2 size, f32 rot, Vec4 tint);
/* raster_flush çağırır, raster'ın karışım ayarını geri koyar */
void b2d_end(Batch2D* b);

/* varsayılan sprite shader'ı: en yakın komşu örnekleme × tint (seviye 0) */
void shade_sprite(const ShadeQuad* q, const void* uniforms, u32 out[4]);

#endif
