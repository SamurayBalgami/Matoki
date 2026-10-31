/* gfx3d/pipeline.h — 3D boru hattı: vertex aşaması → frustum/arka yüz → kırpma → raster.
 * Kırpma homojen klip uzayında: yakın düzlem (z >= 0) her zaman gerçek kırpma;
 * x/y için guard band (±PIPE_GUARD_PX piksel) — yalnızca bandı aşan üçgenler kırpılır.
 * Uzak düzlem: tamamen dışarıdakiler atılır, kısmi olanları derinlik testi keser. */
#ifndef GFX3D_PIPELINE_H
#define GFX3D_PIPELINE_H
#include "core/arena.h"
#include "core/math.h"
#include "raster/raster.h"
#include "raster/ref_shaders.h"
#include "gfx3d/mesh.h"

#define PIPE_GUARD_PX  8192.0f
#define CLIP_MAX_VERTS 16

/* Köşe girdilerinin standart yerleşimi (VertexIn.attr yuvaları; Matoki'de attribute sırası) */
#define ATTR3D_POS   0   /* x, y, z */
#define ATTR3D_NRM   3   /* nx, ny, nz */
#define ATTR3D_UV    6   /* u, v */
#define ATTR3D_COUNT 8

#define VERT_PARAMS 8

/* Vertex shader uniform bloğu (shaders/std_vert.mtk ile aynı sıra; çizim başına boru hattı doldurur).
 * Ofsetler: model 0, view_proj 64, normal_mat 128, time 164, params 168. */
typedef struct VertUniforms {
    Mat4 model;
    Mat4 view_proj;
    Mat3 normal_mat;              /* modelin ters-devrik üst 3×3'ü */
    f32  time;                    /* pipe_set_time */
    f32  params[VERT_PARAMS];     /* materyale özel (Material.vert_params) */
} VertUniforms;

typedef struct Material {
    ShadeFn     shade;
    PreludeFn   prelude;    /* NULL olabilir; varsa shade onun önbelleğini okuyan hızlı yoldur */
    const void* uniforms;
    CullMode    cull;
    VertexFn    vert;       /* NULL → vert_std_c (C kâhini) */
    PreludeFn   vert_prelude;
    f32         vert_params[VERT_PARAMS];
    f32         cull_margin;  /* vertex shader'ın köşeleri en çok bu kadar oynatır (frustum culling payı) */
} Material;

typedef struct DrawItem {
    const Mesh*     mesh;
    const Material* mat;
    Mat4            model;
} DrawItem;

/* ertelenmiş çizim kaydı (pipe_draw → pipe_end) */
typedef struct DrawRec {
    const Mesh*     mesh;
    const Material* mat;
    VertUniforms*   vu;
    RasterState     rs;
    struct ClipVert* cv;      /* parti içinde: köşe aşaması çıktısı */
    u32*            oc;       /* outcode */
    u32             vert_base, tri_base;   /* parti içi önek toplamları */
    u32             nv, nt;   /* partide işlenecek köşe / üçgen (bellek yetmezse 0: çizim atlanır) */
} DrawRec;

/* std_vert.mtk'nin C kâhini: wp = model·p, klip = view_proj·(wp, 1), n = normal_mat·n, uv geçer */
void vert_std_c(const VertexIn* in, const void* uniforms, VertexOut* out);

#define PIPE_MAX_DRAWS 16384

typedef struct ClipVert {
    f32 x, y, z, w;
    f32 var[SHADE_MAX_VARS];
} ClipVert;

typedef struct PipeStats {
    u32 objects_in;
    u32 objects_culled;       /* küre veya AABB frustum dışında */
    u32 tris_in;
    u32 tris_rejected;        /* tümü tek bir frustum düzleminin dışında */
    u32 tris_near_clipped;
    u32 tris_guard_clipped;
    u32 tris_emitted;         /* raster'a giden (kırpma sonrası fan dahil) */
    u32 verts_transformed;
    u32 draws_dropped;        /* kare belleği ya da PIPE_MAX_DRAWS yetmediği için atlanan çizim */
} PipeStats;

typedef struct Pipeline {
    Raster*   r;
    Arena*    frame;
    Mat4      view, proj, view_proj;
    Vec3      eye;
    Vec4      frustum[6];     /* sol, sağ, alt, üst, yakın, uzak (normalize, içeri bakar) */
    f32       vw, vh;
    f32       guard_x, guard_y;
    f32       time;
    usize     frame_mark;     /* pipe_end frame arenasını buraya geri alır */
    DrawRec*  draws;
    u32       ndraws;
    PipeStats stats;
    u32       batches;        /* pipe_end'deki parti sayısı (arena bütçesi) */
    f64       vert_ms, setup_ms, raster_ms;   /* pipe_end aşama süreleri (partilerin toplamı) */
    struct GpuCmd* gpu;       /* != NULL: GPU kipi (faz 8a) — pipe_draw elemeden sonra komut akışına yazar */
} Pipeline;

/* r üzerinde raster_begin çağrılmış olmalı; frame, raster'ın arenasından AYRI olmalı.
 * pipe_draw yalnızca kaydeder (görünmeyeni eler); iş pipe_end'de: köşe aşaması ve üçgen kurulumu
 * iş sistemiyle paralel, sonra raster_flush. Sonuç thread sayısından bağımsız, gönderim sırası korunur.
 * Materyal / mesh / uniform işaretçileri pipe_end'e dek geçerli kalmalı. pipe_end, pipe_begin'den sonra frame\n * arenasından ayrılan her şeyi geri alır. */
void pipe_begin(Pipeline* p, Raster* r, Arena* frame, Mat4 view, Mat4 proj, Vec3 eye);
void pipe_set_time(Pipeline* p, f32 t);
/* pipe_begin'den sonra: çizimler raster yerine GPU komut akışına (gpu/gcmd.h). Akış pipe_begin'den ÖNCE ayrılmış
 * olmalı (pipe_end kare arenasını pipe_begin noktasına geri sarar). Köşe shader'ı (Material.vert) GPU'da std. */
void pipe_set_gpu(Pipeline* p, struct GpuCmd* c);
void pipe_draw(Pipeline* p, const DrawItem* d);
void pipe_end(Pipeline* p);

/* Test/araç erişimi için açık */
void pipe_extract_frustum(const Mat4* view_proj, Vec4 out[6]);
b32  pipe_sphere_visible(const Vec4 planes[6], Vec3 c, f32 r);
b32  pipe_aabb_visible(const Vec4 planes[6], Vec3 mn, Vec3 mx);
void pipe_transform_aabb(const Mat4* m, Vec3 mn, Vec3 mx, Vec3* out_mn, Vec3* out_mx);
/* Sutherland–Hodgman: dot(plane, (x,y,z,w)) >= 0 tarafı tutulur. out en az n+1 yer. */
u32  pipe_clip_polygon(const ClipVert* in, u32 n, ClipVert* out, Vec4 plane, u32 var_count);
void pipe_project(const Pipeline* p, const ClipVert* c, RasterVert* out, u32 var_count);

#endif
