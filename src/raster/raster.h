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

/* MSAA (samples = 4): raster örnek tamponlarına yazar (piksel i, örnek s → [i*4 + s]); kare sonunda
 * fb_resolve örnekleri ortalayıp color'a yazar. Örnek konumları D3D 4x döndürülmüş ızgara.
 * samples 0 veya 1: tek örnek, color/depth doğrudan. */
#define RASTER_MSAA 4

typedef struct Framebuffer {
    u32* color;
    f32* depth;      /* NULL olabilir */
    i32  w, h;
    u32  samples;    /* 0/1 veya RASTER_MSAA */
    u32* color_ms;   /* samples == 4 iken w*h*4 */
    f32* depth_ms;   /* samples == 4 iken w*h*4 (NULL olabilir) */
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

/* Renk karışımı (s = shader çıktısı, d = hedef, a = s.alfa; kanallar 0..255 tamsayı, /255 yuvarlamalı):
 *   NONE    c = s
 *   ALPHA   c.rgb = (s·a + d·(255−a)) / 255,  c.a = a + d.a·(255−a) / 255   (düz alfa, "over")
 *   PREMUL  c = s + d·(255−a) / 255 (255'e kenetli)                        (ön-çarpılmış alfa)
 *   ADD     c = min(255, s + d)
 * Tamsayı: skaler / SIMD / MSAA yolları bit-bit aynı. Karo içi çizim sırası gönderim sırasıdır (belirlenimci). */
typedef enum BlendMode {
    BLEND_NONE,
    BLEND_ALPHA,
    BLEND_PREMUL,
    BLEND_ADD,
    BLEND_COUNT
} BlendMode;

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
    u32 preludes;         /* çağrılan çizim önsözü sayısı */
    u32 tris_dropped;     /* raster arenası dolduğu için atlanan (kısmen ya da tamamen) üçgen */
    u32 overflows;        /* taşma görülen flush sayısı (motor durmaz, sığanlar çizilir) */
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
    PreludeFn    prelude;
    const void*  uniforms;
    u32          var_count;
    b32          depth_test;
    b32          depth_write;
    DepthFunc    depth_func;
    CullMode     cull;
    BlendMode    blend;
    RasterMode   mode;
    RasterStats  stats;
    /* bin'leme: parça (chunk) başına karo kutuları; flush parçaları sırayla gezer */
    Arena*       arena;
    usize        arena_mark;
    struct Bin** chunk_bins;
    u32          nchunks;
    i32          serial_chunk;   /* raster_triangle'ın yazdığı parça (-1: yok) */
    i32          tiles_x, tiles_y;
    u32          pending;
    u32          overflow;       /* paralel kurulumda arena doldu: üçgenler atlandı (flush sayar, sıfırlar) */
    u32*         tile_list;      /* flush'ta boş olmayan karolar (parça tablosuyla birlikte önceden ayrılır) */
} Raster;

/* Bir üçgenin çizim durumu (paralel kurulumda Raster'ın "set" alanları yerine) */
typedef struct RasterState {
    ShadeFn     shade;
    PreludeFn   prelude;
    const void* uniforms;
    u32         var_count;
    CullMode    cull;
    b32         depth_test, depth_write;
    DepthFunc   depth_func;
    BlendMode   blend;
} RasterState;

void fb_clear(Framebuffer* fb, u32 color, f32 depth);
/* MSAA: örneklerin kanal ortalaması ((a+b+c+d+2)>>2) → color; tek örnekte bir şey yapmaz. Satırlar paralel. */
void fb_resolve(Framebuffer* fb);
static inline b32 fb_msaa(const Framebuffer* fb) { return fb->samples == RASTER_MSAA && fb->color_ms; }

void raster_init(Raster* r, Framebuffer* fb);
void raster_set_shader(Raster* r, ShadeFn fn, const void* uniforms, u32 var_count);   /* önsözü sıfırlar */
/* Sonraki üçgenlerin önsözü: flush sırasında (önsöz, uniforms) çifti değiştikçe gölgelemeden önce çağrılır. */
void raster_set_prelude(Raster* r, PreludeFn fn);
void raster_set_depth(Raster* r, b32 test, b32 write, DepthFunc func);
void raster_set_cull(Raster* r, CullMode mode);
void raster_set_blend(Raster* r, BlendMode mode);   /* sonraki üçgenler; geçersiz değer → NONE */
void raster_set_mode(Raster* r, RasterMode mode);
void raster_reset_stats(Raster* r);
/* arena yalnızca raster'a ait olmalı (başkası mark/reset yapmamalı); kare başına bir kez çağrılır. */
void raster_begin(Raster* r, Arena* arena);
void raster_triangle(Raster* r, const RasterVert* a, const RasterVert* b, const RasterVert* c);

/* Paralel kurulum (gfx3d/pipeline):
 *   raster_chunks_alloc — yalnızca ana thread; n ardışık parça ayırır, ilkinin indeksini döndürür
 *   raster_triangle_to  — herhangi bir thread; parçaya yalnızca bir thread aynı anda yazmalı.
 *                         Bellek işin dilimlerinden (RasterAlloc); dolarsa r->overflow (çağıran raster_room ile bütçe tutar).
 * Flush'ta karo içi sıra: parça indeksi, sonra parçaya ekleme sırası. */
/* iş başına ayırıcı: sıfırla başlat, işin sonunda raster_alloc_done (bekleyen üçgen sayısını ekler) */
typedef struct RasterAlloc {
    u8* cur;
    u8* end;
    u32 tris;
} RasterAlloc;

u32   raster_chunks_alloc(Raster* r, u32 n);
void  raster_triangle_to(Raster* r, u32 chunk, const RasterState* s, const RasterVert* a, const RasterVert* b,
                         const RasterVert* c, RasterStats* st, RasterAlloc* ra);
void  raster_alloc_done(Raster* r, RasterAlloc* ra);
usize raster_room(const Raster* r);
void raster_flush(Raster* r);

#endif
