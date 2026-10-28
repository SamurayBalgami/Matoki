/* raster/raster.h — rasterizer public API'si.
 * Girdi: ekran uzayında (piksel, y aşağı) RasterVert; z = NDC derinliği (0..1),
 * inv_w = 1/w_klip, var[] = HAM varying (raster kendisi inv_w ile çarpar ve böler).
 * Ön yüz: NDC'de saat yönünün tersi. Kaplama: piksel merkezleri, 24.8 sabit nokta, top-left.
 * Ertelenmiş çizim: raster_begin → raster_triangle (kur + 64×64 karolara bin'le) → raster_flush.
 * Shader/uniform işaretçileri flush'a kadar geçerli kalmalı. */
#ifndef RASTER_RASTER_H
#define RASTER_RASTER_H
#include "core/types.h"
#include "core/arena.h"
#include "raster/shade_abi.h"

#define RASTER_TILE_SHIFT 6
#define RASTER_TILE       (1 << RASTER_TILE_SHIFT)

#define RASTER_MAX_COORD 4194304.0f   /* 2^22 piksel; 24.8'de 2^30, kenar çarpımı i64'e sığar */

typedef struct Framebuffer {
    u32* color;
    f32* depth;   /* NULL olabilir */
    i32  w, h;
} Framebuffer;

typedef struct RasterVert {
    f32 x, y, z, inv_w;
    f32 var[SHADE_MAX_VARS];
} RasterVert;

typedef enum DepthFunc {
    DEPTH_ALWAYS,
    DEPTH_LESS,
    DEPTH_LEQUAL,
    DEPTH_GREATER,
    DEPTH_GEQUAL
} DepthFunc;

typedef enum CullMode {
    CULL_NONE,
    CULL_BACK,
    CULL_FRONT
} CullMode;

typedef struct RasterStats {
    u32 tris_in;
    u32 tris_culled;      /* arka/ön yüz */
    u32 tris_degenerate;  /* sıfır alan (sabit noktada) */
    u32 tris_empty;       /* hiçbir piksel merkezi yok / ekran dışı */
    u32 tris_overflow;    /* koordinat sınırı aşıldı (pipeline guard band'i bunu önler) */
    u32 tris_drawn;
    u32 quads_shaded;
    u32 pixels_written;
    u32 bin_items;        /* (üçgen, karo) çiftleri */
    u32 bin_full;         /* bunlardan tam kaplananlar (kenar testi atlanan) */
    u32 flushes;          /* arena dolduğu için yapılan ara flush'lar dahil */
} RasterStats;

typedef enum RasterMode {
    RASTER_MODE_SCALAR,
    RASTER_MODE_SIMD,     /* wasm SIMD yoksa skalere düşer */
    RASTER_MODE_COUNT
} RasterMode;

struct Bin;

typedef struct Raster {
    Framebuffer* fb;
    ShadeFn      shade;
    const void*  uniforms;
    u32          var_count;
    b32          depth_test;
    b32          depth_write;
    DepthFunc    depth_func;
    CullMode     cull;
    RasterMode   mode;
    RasterStats  stats;
    /* bin'leme */
    Arena*       arena;
    usize        arena_mark;
    struct Bin*  bins;
    i32          tiles_x, tiles_y;
    u32          pending;
} Raster;

void fb_clear(Framebuffer* fb, u32 color, f32 depth);

void raster_init(Raster* r, Framebuffer* fb);
void raster_set_shader(Raster* r, ShadeFn fn, const void* uniforms, u32 var_count);
void raster_set_depth(Raster* r, b32 test, b32 write, DepthFunc func);
void raster_set_cull(Raster* r, CullMode mode);
void raster_set_mode(Raster* r, RasterMode mode);
void raster_reset_stats(Raster* r);
/* arena yalnızca raster'a ait olmalı (başkası mark/reset yapmamalı); kare başına bir kez çağrılır. */
void raster_begin(Raster* r, Arena* arena);
void raster_triangle(Raster* r, const RasterVert* a, const RasterVert* b, const RasterVert* c);
void raster_flush(Raster* r);

#endif
