/* gpu/gcmd.h — GPU arka ucu için komut akışı (M13 faz 8a-8c, D38, D42, D44). Boru hattı GPU kipinde DrawRec'leri rasterize
 * etmek yerine buraya yazar; web/gpu.mjs akışı okuyup WebGPU çağrılarına çevirir (JS'te motor mantığı yok). Akış u32
 * sözcüklerdir, işaretçiler wasm32 bellek ofseti. Mesh'ler kimlik (adres) + sürüm (Mesh.gen), dokular kimlik (adres) +
 * px ile: JS aynı olanı yeniden yüklemez. Uniform blokları 256 baytlık yuvalarda (WebGPU dinamik ofset hizası) tek blokta. */
#ifndef GPU_GCMD_H
#define GPU_GCMD_H
#include "core/arena.h"
#include "gfx3d/pipeline.h"
#include "matoki/wgsl.h"

#define GCMD_MAGIC   0x5550474Du   /* "MGPU" */
#define GCMD_VERSION 4u
#define GCMD_SLOT    256u          /* uniform yuvası (bayt) */

enum {
    GC_END = 0,
    GC_FRAME = 1,   /* w, h, clear_rgba, clear_depth (f32 bitleri), örnek sayısı (1 / 4): ana geçiş başlar */
    GC_MESH = 2,    /* id, gen, vcount, icount, px, py, pz, nx, ny, nz, u, v, indices */
    GC_DRAW = 3,    /* mesh_id, vert yuvası, frag yuvası, vert prog, frag prog, doku 0, doku 1, cull, depth_test, depth_write */
    GC_TEX = 4,     /* id (Texture adresi), px, px sözcük sayısı */
    GC_SHADOW = 5,      /* doku id, boyut, kademe, kademe sayısı: yalnız derinlik geçişi başlar; id GPU'da doldurulur (px yüklenmez) */
    GC_SHADOW_END = 6,  /* geçiş biter, derinlik doku tamponunda kademenin satırlarına kopyalanır */
    GC_OVERLAY = 7,     /* x, y, w, h, px (önceden çarpılmış RGBA, sıkışık): ana geçişin üstüne karıştırılır */
};

/* GPU programı: Matoki'den üretilmiş WGSL. id 0 = gömülü düz shader (assets/gpu_flat.wgsl, Material.gpu_frag NULL) */
#define GPROG_FLAT 0u
#define GPROG_OVERLAY 1u        /* gömülü bindirme shader'ı (assets/gpu_overlay.wgsl) */
#define GPROG_NONE 0xFFFFFFFFu  /* GC_DRAW parça programı: yalnız derinlik */

typedef struct GpuProgram {
    u32        id;
    u32        gen;   /* canlı derlemede artar: JS modülü yeniler */
    MatokiWgsl wgsl;
} GpuProgram;

typedef struct GpuCmd {
    u32* words;
    u32  count, cap;
    u8*  uniforms;         /* GCMD_SLOT hizalı yuvalar */
    u32  slots, max_slots;
    u32  draws, max_draws;
    u32* seen;             /* bu karede akışa yazılan mesh / doku kimlikleri (açık adresli) */
    u32  seen_cap, seen_count;
    u32  overflow;         /* sığmayan çizim (atlanır, motor durmaz) */
    const GpuProgram* std_vert;   /* Material.gpu_vert NULL ise */
    const void* last_uniforms;    /* ardışık aynı materyal aynı parça yuvasını paylaşır */
    const GpuProgram* last_frag;
    u32  last_slot;
    u32  overlays;
} GpuCmd;

/* kare başı: arenadan akış + uniform bloğu (kare arenası; kare sonunda geri sarılır). Sıra: [gölge geçişi], gcmd_frame, çizimler, bindirmeler */
b32  gcmd_begin(GpuCmd* c, Arena* frame, u32 max_draws, const GpuProgram* std_vert);
void gcmd_shadow_begin(GpuCmd* c, const Texture* tex, u32 size, u32 layer, u32 layers);
void gcmd_shadow_end(GpuCmd* c);
void gcmd_frame(GpuCmd* c, u32 w, u32 h, u32 clear_rgba, f32 clear_depth, u32 samples);
/* vu köşe uniform'u; frag NULL → düz shader; depth_only → parça aşaması yok. Dokular uniform bloğundaki işaretçilerden */
void gcmd_draw(GpuCmd* c, const Mesh* m, const VertUniforms* vu, const GpuProgram* vert, const GpuProgram* frag, const void* uniforms,
               CullMode cull, b32 depth_test, b32 depth_write, b32 depth_only);
/* px, GPU akışı okunana dek geçerli kalmalı (kare arenası yeter) */
void gcmd_overlay(GpuCmd* c, u32 x, u32 y, u32 w, u32 h, const u32* px);
/* aynı çizim siyah ve beyaz zeminde → önceden çarpılmış renk + alfa: renk = siyahtaki, alfa = 255 − (beyaz − siyah) */
void gcmd_overlay_extract(const u32* on_black, const u32* on_white, u32* out, u32 n);
/* akışı kapatır; başlık: [magic, sürüm, sözcük sayısı, çizim, uniform ptr, uniform bayt] */
void gcmd_end(GpuCmd* c);

#endif
