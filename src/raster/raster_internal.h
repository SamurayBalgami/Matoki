/* raster/raster_internal.h — rasterizer'ın iç veri yapıları (public API değil).
 * Üçgen kurulumu (TriSetup) bir kez yapılır, 64×64 karolara bin'lenir; raster_flush karoları
 * sırayla işler. Karo içi döngünün iki uygulaması var: skaler (raster.c) ve wasm SIMD
 * (raster_simd_wasm.c). İkisi BİT-BİT aynı çıktıyı üretmek zorunda (test: raster_simd_matches_scalar). */
#ifndef RASTER_RASTER_INTERNAL_H
#define RASTER_RASTER_INTERNAL_H
#include "raster/raster.h"

/* f(px, py) = c + a*fx + b*fy; fx = px + 0.5 - x0, fy = py + 0.5 - y0 (işlem sırası sözleşmedir) */
typedef struct AttrPlane {
    f32 a, b, c;
} AttrPlane;

/* E(px, py) = c + sx*px + sy*py : piksel (px, py) merkezinde kenar fonksiyonu (top-left yanlılığı dahil).
 * İçeride E >= 0. Birim: (1/256 px)^2. */
typedef struct EdgeEq {
    i64 c, sx, sy;
} EdgeEq;

typedef struct TriSetup {
    ShadeFn     shade;
    PreludeFn   prelude;                  /* NULL olabilir */
    const void* uniforms;
    u32         var_count;
    u8          depth_test, depth_write, depth_func, drawn;
    u8          blend, _pad[3];           /* BlendMode */
    i32         minx, miny, maxx, maxy;   /* framebuffer'a kenetlenmiş piksel sınır kutusu (dahil) */
    f32         x0, y0;
    EdgeEq      e[3];
    AttrPlane   z, w;
    AttrPlane   v[];                      /* var_count adet: var*inv_w düzlemleri */
} TriSetup;

typedef struct BinItem {
    TriSetup* t;
    u32       full;    /* 1: karo tamamen üçgenin içinde → kenar testi atlanır */
} BinItem;

#define BIN_CHUNK_ITEMS 30   /* küçük: paralel kurulumda (parça × karo) başına bir düğüm */

typedef struct BinChunk {
    struct BinChunk* next;
    u32              count;
    BinItem          items[BIN_CHUNK_ITEMS];
} BinChunk;

typedef struct Bin {
    BinChunk* head;
    BinChunk* tail;
} Bin;

static inline b32 raster_depth_pass(u32 f, f32 z, f32 d) {
    switch ((DepthFunc)f) {
    case DEPTH_LESS:    return z < d;
    case DEPTH_LEQUAL:  return z <= d;
    case DEPTH_GREATER: return z > d;
    case DEPTH_GEQUAL:  return z >= d;
    case DEPTH_ALWAYS:  return TRUE;
    }
    return TRUE;
}

static inline f32 attr_eval(const AttrPlane* p, f32 fx, f32 fy) { return p->c + p->a * fx + p->b * fy; }

/* round(x / 255), x ∈ [0, 65535] için kesin */
static inline u32 div255(u32 x) { return (x + 128u + ((x + 128u) >> 8)) >> 8; }

/* raster.h BlendMode tablosu; mode != NONE */
static inline u32 raster_blend_px(u32 mode, u32 s, u32 d) {
    u32 a = s >> 24, ia = 255u - a, r = 0;
    for (u32 k = 0; k < 32; k += 8) {
        u32 sc = (s >> k) & 0xFFu, dc = (d >> k) & 0xFFu, c;
        if (mode == BLEND_ALPHA) c = k == 24 ? a + div255(dc * ia) : div255(sc * a + dc * ia);
        else if (mode == BLEND_PREMUL) c = sc + div255(dc * ia);
        else c = sc + dc;   /* ADD */
        r |= (c > 255u ? 255u : c) << k;
    }
    return r;
}

/* Karo [tx0..tx1] × [ty0..ty1] (dahil, framebuffer'a kenetli, tx0/ty0 çift) içinde üçgeni çiz.
 * Farklı karolar farklı thread'lerde aynı anda çağrılabilir: istatistik thread'in st'sine yazılır,
 * r yalnızca okunur. */
void raster_tile_scalar(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st);
#if defined(__wasm_simd128__)
void raster_tile_simd(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st);
#endif
/* MSAA 4x: piksel başına 4 örnekte kapsama + derinlik, shader piksel merkezinde bir kez */
void raster_tile_msaa(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st);
#if defined(__wasm_simd128__)
void raster_tile_msaa_simd(Raster* r, TriSetup* t, i32 tx0, i32 ty0, i32 tx1, i32 ty1, b32 full, RasterStats* st);
#endif

#endif
