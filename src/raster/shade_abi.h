/* raster/shade_abi.h — rasterizer ile shader'lar arasındaki SÖZLEŞME.
 * Bir ShadeFn bir 2×2 quad'ı işler. Lane sırası: lane0=(x,y) lane1=(x+1,y)
 * lane2=(x,y+1) lane3=(x+1,y+1). var[i][lane] perspektif-düzeltilmiş değerlerdir ve
 * KAPSANMAYAN (yardımcı) lane'ler için de düzlem denklemiyle dışdeğerlenir → quad
 * türevleri (dFdx = v[1]-v[0], dFdy = v[2]-v[0]) her zaman geçerlidir. */
#ifndef RASTER_SHADE_ABI_H
#define RASTER_SHADE_ABI_H
#include "core/types.h"

#define SHADE_MAX_VARS 16

typedef struct ShadeQuad {
    i32 x, y;                        /* lane0 pikseli */
    u32 mask;                        /* bit l: lane l kapsandı ve derinlik testini geçti */
    u32 _pad;
    f32 z[4];                        /* NDC derinliği (0..1) */
    f32 var[SHADE_MAX_VARS][4];      /* varying[i][lane] */
} ShadeQuad;

/* out[l] = 0xAABBGGRR renk; yalnızca mask'taki lane'ler yazılır. */
typedef void (*ShadeFn)(const ShadeQuad* q, const void* uniforms, u32 out[4]);

/* Doku. Boyut 32 bayt (wasm32), ofsetler sabit (codegen bunlara gömülü erişir).
 * Mip zinciri: tüm seviyeler px'ten başlayarak bitişik; seviye k: wk = max(1, w>>k),
 * hk = max(1, h>>k), başlangıç = px + Σ_{i<k} wi*hi. levels = 0 → 1 kabul edilir.
 * Wrap adreslemesi w,h'nin 2'nin kuvveti olmasını gerektirir. */
typedef struct Texture {
    u32* px;
    u32  w, h;
    u32  w_mask, h_mask;   /* w-1, h-1 */
    u32  flags;
    u32  log2_w;
    u32  levels;           /* mip seviye sayısı (>= 1) */
} Texture;

#define TEX_OFF_PX     0
#define TEX_OFF_W      4
#define TEX_OFF_H      8
#define TEX_OFF_WMASK  12
#define TEX_OFF_HMASK  16
#define TEX_OFF_FLAGS  20
#define TEX_OFF_LOG2W  24
#define TEX_OFF_LEVELS 28

#define TEX_FLAG_BILINEAR 1u
#define TEX_FLAG_CLAMP    2u
#define TEX_FLAG_MIPMAP   4u

#if defined(__wasm32__)
_Static_assert(sizeof(Texture) == 32, "Texture ABI boyutu 32 bayt olmalı");
#endif

static inline u32 color_pack(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

#endif
