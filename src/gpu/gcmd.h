/* gpu/gcmd.h — GPU arka ucu için komut akışı (M13 faz 8a, D38). Boru hattı GPU kipinde DrawRec'leri rasterize etmek
 * yerine buraya yazar; web/gpu.mjs akışı okuyup WebGPU çağrılarına çevirir (JS'te motor mantığı yok). Akış u32
 * sözcüklerdir, işaretçiler wasm32 bellek ofseti. Mesh'ler kimlik (adres) + sürüm (Mesh.gen) ile: JS sürümü aynı
 * olanı yeniden yüklemez. Çizim başına 256 baytlık uniform yuvası (WebGPU dinamik ofset hizası) tek blokta. */
#ifndef GPU_GCMD_H
#define GPU_GCMD_H
#include "core/arena.h"
#include "gfx3d/mesh.h"
#include "raster/raster.h"

#define GCMD_MAGIC   0x5550474Du   /* "MGPU" */
#define GCMD_VERSION 1u
#define GCMD_SLOT    256u          /* uniform yuvası (bayt) */

enum {
    GC_END = 0,
    GC_FRAME = 1,   /* w, h, clear_rgba, clear_depth (f32 bitleri), kip */
    GC_MESH = 2,    /* id, gen, vcount, icount, px, py, pz, nx, ny, nz, indices */
    GC_DRAW = 3,    /* mesh_id, slot, cull, depth_test, depth_write */
};

/* GC_FRAME kipi: GPU shader'ı (8a; 8b'de Matoki → WGSL) */
enum { GPU_MODE_FLAT = 0, GPU_MODE_NORMAL = 1 };

/* uniform yuvası (shaders/gpu_8a.wgsl Draw yapısı ile aynı sıra) */
typedef struct GpuDrawUniforms {
    Mat4 model;
    Mat4 view_proj;
    f32  normal_mat[12];   /* mat3 → 3 × vec4 (WGSL mat3x3 hizası) */
} GpuDrawUniforms;

typedef struct GpuCmd {
    u32* words;
    u32  count, cap;
    u8*  uniforms;         /* GCMD_SLOT hizalı yuvalar */
    u32  draws, max_draws;
    u32* mesh_seen;        /* bu karede akışa yazılan mesh kimlikleri (açık adresli) */
    u32  mesh_seen_cap;
    u32  overflow;         /* sığmayan çizim (atlanır, motor durmaz) */
} GpuCmd;

/* kare başı: arenadan akış + uniform bloğu (kare arenası; kare sonunda geri sarılır) */
b32  gcmd_begin(GpuCmd* c, Arena* frame, u32 max_draws, u32 w, u32 h, u32 clear_rgba, f32 clear_depth, u32 mode);
void gcmd_draw(GpuCmd* c, const Mesh* m, const Mat4* model, const Mat4* view_proj, const Mat3* normal_mat, CullMode cull,
               b32 depth_test, b32 depth_write);
/* akışı kapatır; başlık: [magic, sürüm, sözcük sayısı, çizim, uniform ptr, uniform bayt] */
void gcmd_end(GpuCmd* c);

#endif
