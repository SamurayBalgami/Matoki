/* asset/gltf.h — glTF 2.0 (.gltf JSON) ve GLB (ikili) yükleyici, sıfırdan (asset/json + base64 + img/png).
 * Desteklenen: buffers (gömülü base64 / GLB BIN), bufferViews (byteStride), accessors (tüm componentType'lar,
 * normalized), mesh primitive'leri (TRIANGLES; POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0/indices),
 * node hiyerarşisi (matrix veya TRS) → dünya matrisleri, skins (joints, inverseBindMatrices, skeleton),
 * animations (translation/rotation/scale; LINEAR, STEP, CUBICSPLINE) ve pbrMetallicRoughness materyalleri.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/asset/gltf.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ASSET_GLTF_H
#define ASSET_GLTF_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "raster/shade_abi.h"
#include "gfx3d/mesh.h"
#include "anim/skeleton.h"
#include "anim/clip.h"
#include "anim/skin.h"

#define GLTF_ALPHA_OPAQUE 0u
#define GLTF_ALPHA_MASK   1u
#define GLTF_ALPHA_BLEND  2u

typedef struct {
    Mesh*        mesh;
    Mat4         world;
    i32          material;       /* -1: varsayılan */
    i32          skin;           /* GltfModel.skins indeksi, -1: skin yok */
    SkinnedMesh* skinned;        /* skin/JOINTS_0/WEIGHTS_0 varsa != NULL */
} GltfDraw;

typedef struct {
    Vec4 base_color;
    i32  base_texture;    /* GltfModel.textures indeksi, -1 yok */
    b32  double_sided;
    u32  alpha_mode;
    f32  alpha_cutoff;
} GltfMaterial;

/* Harici dosya (uri) çözücü: bulunamazsa NULL. GLB/gömülü dosyalar için gerekmez. */
typedef const u8* (*GltfResolveFn)(void* user, const char* uri, u32* out_len);

typedef struct {
    GltfDraw*     draws;
    u32           draw_count;
    GltfMaterial* materials;
    u32           material_count;
    Texture*      textures;      /* glTF "textures" dizisiyle aynı indeksler; çözülemeyen = px NULL */
    u32           texture_count;
    Skeleton*     skins;
    u32           skin_count;
    AnimClip*     animations;
    u32           anim_count;
    Vec3          bounds_min, bounds_max;   /* tüm çizimlerin dünya uzayı AABB'si */
    u32           triangles;
    u32           warnings;
    f64           t_json_ms, t_tex_ms, t_mesh_ms;   /* ölçüm: JSON, PNG çözme + mip, accessor + mesh */
    char          warn[256];     /* ilk birkaç uyarının özeti */
    char          err[128];
} GltfModel;

/* data: .gltf metni veya .glb baytları (başlıktan otomatik ayırt edilir).
 * out: kalıcı sonuçlar (mesh'ler, dokular, iskeletler, animasyonlar),
 * scratch: geçici (JSON DOM, çözülmüş tamponlar). */
b32 gltf_load(const u8* data, u32 len, Arena* out, Arena* scratch,
              GltfResolveFn resolve, void* user, GltfModel* model);

/* Birim quaternion (x,y,z,w) → dönme matrisi (yardımcı, testlerde de kullanılır) */
Mat4 mat4_from_quat(f32 x, f32 y, f32 z, f32 w);

#endif
