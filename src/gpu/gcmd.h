/* gpu/gcmd.h — GPU arka ucu için komut akışı (M13 faz 8a-8b, D38, D42). Boru hattı GPU kipinde DrawRec'leri rasterize
 * etmek yerine buraya yazar; web/gpu.mjs akışı okuyup WebGPU çağrılarına çevirir (JS'te motor mantığı yok). Akış u32
 * sözcüklerdir, işaretçiler wasm32 bellek ofseti. Mesh'ler kimlik (adres) + sürüm (Mesh.gen), dokular kimlik (adres) +
 * px ile: JS aynı olanı yeniden yüklemez. Uniform blokları 256 baytlık yuvalarda (WebGPU dinamik ofset hizası) tek blokta. */
#ifndef GPU_GCMD_H
#define GPU_GCMD_H
#include "core/arena.h"
#include "gfx3d/pipeline.h"
#include "matoki/wgsl.h"

#define GCMD_MAGIC   0x5550474Du   /* "MGPU" */
#define GCMD_VERSION 2u
#define GCMD_SLOT    256u          /* uniform yuvası (bayt) */

enum {
    GC_END = 0,
    GC_FRAME = 1,   /* w, h, clear_rgba, clear_depth (f32 bitleri) */
    GC_MESH = 2,    /* id, gen, vcount, icount, px, py, pz, nx, ny, nz, u, v, indices */
    GC_DRAW = 3,    /* mesh_id, vert yuvası, frag yuvası, vert prog, frag prog, doku 0, doku 1, cull, depth_test, depth_write */
    GC_TEX = 4,     /* id (Texture adresi), px, px sözcük sayısı */
};

/* GPU programı: Matoki'den üretilmiş WGSL. id 0 = gömülü düz shader (assets/gpu_flat.wgsl, Material.gpu_frag NULL) */
#define GPROG_FLAT 0u

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
} GpuCmd;

/* kare başı: arenadan akış + uniform bloğu (kare arenası; kare sonunda geri sarılır) */
b32  gcmd_begin(GpuCmd* c, Arena* frame, u32 max_draws, u32 w, u32 h, u32 clear_rgba, f32 clear_depth, const GpuProgram* std_vert);
/* vu köşe uniform'u; frag NULL → düz shader. Dokular uniform bloğundaki işaretçilerden (frag->wgsl.tex_ofs) */
void gcmd_draw(GpuCmd* c, const Mesh* m, const VertUniforms* vu, const GpuProgram* vert, const GpuProgram* frag, const void* uniforms,
               CullMode cull, b32 depth_test, b32 depth_write);
/* akışı kapatır; başlık: [magic, sürüm, sözcük sayısı, çizim, uniform ptr, uniform bayt] */
void gcmd_end(GpuCmd* c);

#endif
