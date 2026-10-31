/* gfx3d/mesh.h — SoA mesh ve basit üreteçler (küp, düzlem).
 * Mesh verisi arena'da yaşar; tangent ve vertex rengi isteğe bağlı (NULL).
 * Sınırlar (AABB + küre) frustum culling için mesh_compute_bounds ile doldurulur. */
#ifndef GFX3D_MESH_H
#define GFX3D_MESH_H
#include "core/arena.h"
#include "core/math.h"

typedef struct Mesh {
    f32 *px, *py, *pz;
    f32 *nx, *ny, *nz;
    f32 *u, *v;
    f32 *tx, *ty, *tz, *tw;   /* isteğe bağlı */
    u32 *color;               /* isteğe bağlı */
    u32 *indices;
    u32  vertex_count, index_count;
    Vec3 aabb_min, aabb_max;
    Vec3 sphere_center;
    f32  sphere_radius;
    u32  gen;    /* içerik sürümü (tüm mesh'ler arası tekil): mesh_touch; GPU arka ucu aynı (adres, gen)'i yeniden yüklemez */
} Mesh;

Mesh* mesh_alloc(Arena* a, u32 vertex_count, u32 index_count);
/* yeni sürüm (gen); köşe / indeks değiştiren kod sonunda mesh_compute_bounds ya da bunu çağırır */
void  mesh_touch(Mesh* m);
/* sınırlar + mesh_touch */
void  mesh_compute_bounds(Mesh* m);
/* alan ağırlıklı köşe normalleri (üçgen kenar çarpımı); aralık dışı indeksli üçgen atlanır,
 * hiçbir üçgene değmeyen köşenin normali 0 kalır (MatokiEngine'den, D37) */
void  mesh_compute_normals(Mesh* m);

/* Birim küp [-0.5, 0.5]^3, yüz başına 4 köşe (24), her yüzde uv 0..1. */
Mesh* mesh_cube(Arena* a);
/* y=0 düzleminde [-sx/2, sx/2] × [-sz/2, sz/2], normal +y, uv = konum * uv_per_unit.
 * segments: kenar başına bölüm (>= 1). */
Mesh* mesh_plane(Arena* a, f32 sx, f32 sz, f32 uv_per_unit, u32 segments);
/* Birim küre (yarıçap 1), enlem/boylam ızgarası; uv = (boylam, enlem) */
Mesh* mesh_sphere(Arena* a, u32 lat, u32 lon);
/* Silindir: eksen x, yarıçap 1, uzunluk 1 (x ∈ [-0.5, 0.5]), kapaklı; tekerlek için */
Mesh* mesh_cylinder(Arena* a, u32 segments);

#endif
