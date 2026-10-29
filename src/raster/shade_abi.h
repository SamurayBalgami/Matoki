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
    u32 kill;                        /* shader'ın TEK çıktı alanı: bit l = lane l atıldı (discard).
                                      * Raster çağrıdan önce 0 yazar, sonra mask &= ~kill uygular;
                                      * q raster'da yazılabilir bellektedir (const yalnızca sözleşme). */
    f32 z[4];                        /* NDC derinliği (0..1) */
    f32 var[SHADE_MAX_VARS][4];      /* varying[i][lane] */
} ShadeQuad;

/* out[l] = 0xAABBGGRR renk; yalnızca mask'taki lane'ler yazılır. */
typedef void (*ShadeFn)(const ShadeQuad* q, const void* uniforms, u32 out[4]);

/* ---- vertex aşaması: bir çağrı 4 köşe (lane = köşe) ----
 * attr[k][lane]: köşe girdisi bileşeni k (boru hattının standart yerleşimi gfx3d/pipeline.h ATTR3D_*).
 * count: geçerli lane sayısı (1..4); geçersiz lane'ler son geçerli köşenin kopyasıdır.
 * Çıktı: pos = klip uzayı (x, y, z, w), var = fragment aşamasına giden varying'ler (ShadeQuad.var ile aynı yuvalar).
 * İmza wasm'da ShadeFn ile aynı (i32, i32, i32): Matoki JIT, önsöz ve thread önbelleği değişmeden çalışır. */
#define VERT_MAX_ATTRS 16

typedef struct VertexIn {
    u32 count;
    u32 _pad[3];
    f32 attr[VERT_MAX_ATTRS][4];
} VertexIn;

typedef struct VertexOut {
    f32 pos[4][4];
    f32 var[SHADE_MAX_VARS][4];
} VertexOut;

typedef void (*VertexFn)(const VertexIn* in, const void* uniforms, VertexOut* out);

#define VIN_ATTR_OFS  16    /* VertexIn.attr */
#define VOUT_POS_OFS  0     /* VertexOut.pos */
#define VOUT_VAR_OFS  64    /* VertexOut.var */

/* İsteğe bağlı çizim önsözü: uniform'lardan türeyen değerleri shader'ın önbelleğine yazar.
 * Raster, önsözü olan bir üçgeni gölgelemeden önce (önsöz, uniforms) çifti değiştiyse çağırır;
 * uniform içeriği flush boyunca sabit olmalıdır (zaten sözleşme). */
typedef void (*PreludeFn)(const void* uniforms);

/* Doku. Boyut 352 bayt (wasm32), ofsetler sabit (codegen bunlara gömülü erişir).
 * Mip zinciri: tüm seviyeler px'ten başlayarak bitişik; seviye k: wk = max(1, w>>k),
 * hk = max(1, h>>k), başlangıç = px + lv_ofs[k] (texel), lv_ofs[k] = Σ_{i<k} wi*hi.
 * levels = 0 → 1 kabul edilir; lv_ofs[k >= levels] son seviyeyi tekrarlar (tex_create doldurur).
 * Wrap adreslemesi w,h'nin 2'nin kuvveti olmasını gerektirir. */
#define TEX_MAX_LEVELS 16

typedef struct Texture {
    u32* px;
    u32  w, h;
    u32  w_mask, h_mask;   /* w-1, h-1 */
    u32  flags;
    u32  log2_w;
    u32  levels;           /* mip seviye sayısı (>= 1) */
    u32  lv_ofs[TEX_MAX_LEVELS];
    u32  lv_w[TEX_MAX_LEVELS];    /* seviye boyutu wk, hk (k >= levels → son seviye) */
    u32  lv_h[TEX_MAX_LEVELS];
    f32  lv_fw[TEX_MAX_LEVELS];   /* (f32)wk, (f32)hk */
    f32  lv_fh[TEX_MAX_LEVELS];
} Texture;

#define TEX_OFF_PX     0
#define TEX_OFF_W      4
#define TEX_OFF_H      8
#define TEX_OFF_WMASK  12
#define TEX_OFF_HMASK  16
#define TEX_OFF_FLAGS  20
#define TEX_OFF_LOG2W  24
#define TEX_OFF_LEVELS 28
#define TEX_OFF_LVOFS  32
#define TEX_WORD_LVOFS 8    /* lv_ofs[0]'ın u32 indeksi */
#define TEX_WORD_LVW   24   /* lv_w[0] */
#define TEX_WORD_LVH   40   /* lv_h[0] */
#define TEX_WORD_LVFW  56   /* lv_fw[0] (f32 bitleri) */
#define TEX_WORD_LVFH  72   /* lv_fh[0] */

#define TEX_FLAG_BILINEAR 1u
#define TEX_FLAG_CLAMP    2u
#define TEX_FLAG_MIPMAP   4u

#if defined(__wasm32__)
_Static_assert(sizeof(Texture) == 352, "Texture ABI boyutu 352 bayt olmalı");
_Static_assert(__builtin_offsetof(Texture, lv_fh) == TEX_WORD_LVFH * 4, "Texture seviye tablosu ofsetleri");
#endif

static inline u32 color_pack(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

#endif
