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

typedef struct Material {
    ShadeFn     shade;
    const void* uniforms;
    CullMode    cull;
} Material;

typedef struct DrawItem {
    const Mesh*     mesh;
    const Material* mat;
    Mat4            model;
} DrawItem;

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
} PipeStats;

typedef struct Pipeline {
    Raster*   r;
    Arena*    frame;
    Mat4      view, proj, view_proj;
    Vec3      eye;
    Vec4      frustum[6];     /* sol, sağ, alt, üst, yakın, uzak (normalize, içeri bakar) */
    f32       vw, vh;
    f32       guard_x, guard_y;
    PipeStats stats;
} Pipeline;

/* r üzerinde raster_begin çağrılmış olmalı; frame, raster'ın arenasından AYRI olmalı. */
void pipe_begin(Pipeline* p, Raster* r, Arena* frame, Mat4 view, Mat4 proj, Vec3 eye);
void pipe_draw(Pipeline* p, const DrawItem* d);
void pipe_end(Pipeline* p);   /* raster_flush */

/* Test/araç erişimi için açık */
void pipe_extract_frustum(const Mat4* view_proj, Vec4 out[6]);
b32  pipe_sphere_visible(const Vec4 planes[6], Vec3 c, f32 r);
b32  pipe_aabb_visible(const Vec4 planes[6], Vec3 mn, Vec3 mx);
void pipe_transform_aabb(const Mat4* m, Vec3 mn, Vec3 mx, Vec3* out_mn, Vec3* out_mx);
/* Sutherland–Hodgman: dot(plane, (x,y,z,w)) >= 0 tarafı tutulur. out en az n+1 yer. */
u32  pipe_clip_polygon(const ClipVert* in, u32 n, ClipVert* out, Vec4 plane, u32 var_count);
void pipe_project(const Pipeline* p, const ClipVert* c, RasterVert* out, u32 var_count);

#endif
