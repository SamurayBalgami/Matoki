/* anim/skeleton.h — İskelet (Skeleton) veri yapısı, eklem hiyerarşisi ve poz matrisi hesabı.
 * Her eklem yerel TRS (JointPose), üst eklem indeksi ve ters bağlama matrisi (inverseBindMatrix)
 * taşır. Dünya ve skinning matrisleri özyinelemesiz ve döngü korumalı olarak hesaplanır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/skeleton.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ANIM_SKELETON_H
#define ANIM_SKELETON_H

#include "core/types.h"
#include "core/arena.h"
#include "core/math.h"
#include "scene/transform.h"

#define SKEL_MAX_JOINTS 256u

typedef struct {
    Vec3 translation;
    Quat rotation;
    Vec3 scale;
} JointPose;

typedef struct {
    u32        joint_count;
    i32*       parents;        /* [joint_count]: üst eklem indeksi (0..joint_count-1) veya -1 (kök) */
    Mat4*      inv_bind;       /* [joint_count]: inverseBindMatrices (varsayılan: birim matris) */
    JointPose* bind_pose;      /* [joint_count]: bağlama (dinlenme) yerel TRS pozu */
    i32*       node_indices;   /* [joint_count]: glTF node indeksi (yoksa -1) */
    i32        skeleton_node;  /* glTF skin.skeleton kök node indeksi (yoksa -1) */
    Mat4       root_transform; /* İskelet kökünün üst dönüşümü (varsayılan: birim matris) */
} Skeleton;

/* Yardımcı: sonlu f32 denetimi (NaN ve +-Inf -> FALSE) */
static inline b32 anim_f32_finite(f32 x) {
    u32 u;
    __builtin_memcpy(&u, &x, sizeof(u));
    return (u & 0x7f800000u) != 0x7f800000u;
}

JointPose joint_pose_identity(void);
JointPose joint_pose_trs(Vec3 t, Quat r, Vec3 s);
Mat4      joint_pose_mat4(const JointPose* p);
JointPose joint_pose_from_mat4(Mat4 m);

/* Quaternion en kısa yol doğrusal normalize (NLERP) ve küresel (SLERP) interpolasyon */
Quat quat_nlerp(Quat a, Quat b, f32 t);
Quat quat_slerp(Quat a, Quat b, f32 t);

/* Arena üzerinde joint_count eklemli varsayılan iskelet ilklendirir (sınır dışıysa FALSE) */
b32 skeleton_init(Skeleton* skel, Arena* a, u32 joint_count);

/* Ebeveyn dizisinde sınır dışı indeks, öz-ebeveyn veya döngü olup olmadığını ve NaN bulunmadığını denetler */
b32 skeleton_validate(const Skeleton* skel);

/* Döngü veya geçersiz üst eklem indekslerini (-1 yaparak) ve NaN değerleri güvenli hale getirir;
 * düzeltilen sorun sayısını döndürür */
u32 skeleton_sanitize(Skeleton* skel);

/* Yerel pozlardan (NULL ise bind_pose kullanılır) dünya matrislerini (out_world, opsiyonel) ve
 * skinning matrislerini (out_skin[j] = world[j] * inv_bind[j], opsiyonel) özyinelemesiz hesaplar */
b32 skeleton_compute_matrices(const Skeleton* skel, const JointPose* local_poses,
                              Mat4* out_world, Mat4* out_skin);

#endif
