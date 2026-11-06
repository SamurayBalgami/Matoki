/* gfx3d/cloth_mesh.h — kumaş parçacık ızgarasından çizim yüzeyi (D58).
 * Fizik ızgarası (nu × nv) Catmull-Rom tensör yüzeyiyle r kat sıklaştırılır: yüzey her parçacıktan geçer, kıvrımlar köşeli
 * değil eğri görünür; normal yüzeyin analitik türevlerinden (∂u × ∂v), üçgen ortalaması değil. Fizik maliyeti değişmez.
 * Kenarda dış komşu yansıtılır (P[-1] = 2·P[0] − P[1]). İki yüzlü: arka kopya ters sarım + ters normal. */
#ifndef GFX3D_CLOTH_MESH_H
#define GFX3D_CLOTH_MESH_H
#include "core/arena.h"
#include "core/math.h"
#include "gfx3d/mesh.h"

#define CLOTH_MESH_MAX_R 8u

typedef struct ClothMesh {
    Mesh* mesh;
    u32   nu, nv;       /* parçacık ızgarası */
    u32   r;            /* sıklaştırma (aralık başına alt aralık) */
    u32   mu, mv;       /* çizim ızgarası: (nu − 1)·r + 1 × (nv − 1)·r + 1 */
    Vec3* row_p;        /* ara tampon: her parçacık satırının u yönünde sıklaştırılmışı (nv × mu) */
    Vec3* row_du;
    f32   w[CLOTH_MESH_MAX_R + 1][4];    /* t = k / r için ağırlıklar */
    f32   dw[CLOTH_MESH_MAX_R + 1][4];   /* ve türevleri */
} ClothMesh;

/* uv: u ∈ [0, u_len], v ∈ [0, v_len] (parçacık ızgarasıyla aynı yön); r ∈ [1, 8] */
ClothMesh* cloth_mesh_alloc(Arena* a, u32 nu, u32 nv, u32 r, f32 u_len, f32 v_len);
/* x: nu × nv parçacık konumu (satır satır, j·nu + i). Sonunda mesh_compute_bounds. */
void cloth_mesh_update(ClothMesh* c, const Vec3* x);

#endif
