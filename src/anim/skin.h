/* anim/skin.h — Doğrusal karışımlı CPU iskelet kaplama (Linear Blend Skinning - LBS).
 * Köşe başına 4 eklem indeksi (u16) ve 4 ağırlık (f32) ile konum ve normalleri dönüştürür;
 * çıktı sıradan bir gfx3d Mesh'idir (mevcut 3D boru hattıyla doğrudan çizilir).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/skin.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ANIM_SKIN_H
#define ANIM_SKIN_H

#include "anim/skeleton.h"
#include "gfx3d/mesh.h"

#define SKIN_INFLUENCES   4u
#define SKIN_MAX_VERTICES (1u << 20)

typedef struct {
    const Mesh* bind_mesh;    /* Bağlama (bind) pozundaki kaynak mesh (salt okunur) */
    u32         vertex_count;
    u16*        joints;       /* [vertex_count * 4]: köşe başına 4 eklem indeksi */
    f32*        weights;      /* [vertex_count * 4]: köşe başına 4 ağırlık */
} SkinnedMesh;

/* Eklem indekslerinin (< joint_count) ve ağırlıkların (sonlu, >= 0, toplam > 0) geçerliliğini denetler */
b32 skin_validate_weights(const u16* joints, const f32* weights, u32 vertex_count, u32 joint_count);

/* Bozuk eklem indekslerini (>= joint_count), NaN/negatif ağırlıkları ve 0 toplamlı ağırlıkları
 * yerinde temizleyip normalize eder; düzeltilen köşe sayısını döndürür */
u32 skin_sanitize_weights(u16* joints, f32* weights, u32 vertex_count, u32 joint_count);

/* Bağlama mesh'inin (bind_mesh) yapısını kopyalayarak çıktı için yeni bir hedef Mesh ayırır */
Mesh* skin_clone_mesh(Arena* a, const Mesh* bind_mesh);

/* Skaler referans köşe skinning çekirdeği (hem host'ta hem SIMD karşılaştırma testinde kullanılır) */
void skin_vertices_scalar(const Mat4* skin_mats, u32 joint_count,
                          const f32* in_px, const f32* in_py, const f32* in_pz,
                          const f32* in_nx, const f32* in_ny, const f32* in_nz,
                          const u16* joints, const f32* weights, u32 vertex_count,
                          f32* out_px, f32* out_py, f32* out_pz,
                          f32* out_nx, f32* out_ny, f32* out_nz);

/* Aktif köşe skinning çekirdeği (wasm'da skin_wasm.c SIMD, host'ta skin.c skaler) */
void skin_vertices(const Mat4* skin_mats, u32 joint_count,
                   const f32* in_px, const f32* in_py, const f32* in_pz,
                   const f32* in_nx, const f32* in_ny, const f32* in_nz,
                   const u16* joints, const f32* weights, u32 vertex_count,
                   f32* out_px, f32* out_py, f32* out_pz,
                   f32* out_nx, f32* out_ny, f32* out_nz);

/* SkinnedMesh'i verilen skin_mats (world * inv_bind) ile out_mesh'e uygular ve sınırları günceller */
b32 skin_mesh(const SkinnedMesh* sm, const Mat4* skin_mats, u32 joint_count, Mesh* out_mesh);

#endif
