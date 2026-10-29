/* scene/transform.h — Sahne hiyerarşisi (Parent) ve 3D dönüşüm (Transform) bileşenleri.
 * Yerel konum (Vec3), dönme (Quat) ve ölçekten (Vec3) yerel TRS matrisi üretir;
 * ebeveyn-çocuk zinciri boyunca dünya matrislerini (Mat4) döngü denetimli olarak günceller.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/transform.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef SCENE_TRANSFORM_H
#define SCENE_TRANSFORM_H

#include "core/types.h"
#include "core/math.h"
#include "core/quat.h"
#include "scene/ecs.h"

typedef struct {
    Vec3 pos;
    Quat rot;
    Vec3 scale;
    Mat4 world;
} Transform;

typedef struct {
    EcsEntity parent;
} Parent;

Transform transform_identity(void);
Transform transform_trs(Vec3 pos, Quat rot, Vec3 scale);
Mat4      transform_local_mat4(const Transform* t);
Vec3      transform_world_point(const Transform* t, Vec3 local_pt);

/* Transform ve Parent bileşenlerini dünyaya kaydeder */
void      transform_register(EcsWorld* w);
EcsCompId transform_comp_id(const EcsWorld* w);
EcsCompId parent_comp_id(const EcsWorld* w);

/* Hiyerarşi yönetimi: döngü oluşturacak ebeveyn atamalarını FALSE ile reddeder */
b32       transform_set_parent(EcsWorld* w, EcsEntity child, EcsEntity parent);
EcsEntity transform_get_parent(const EcsWorld* w, EcsEntity child);

/* Tüm Transform bileşenlerinin dünya matrislerini (world) ebeveyn zincirine göre günceller */
void      transform_update(EcsWorld* w);

#endif
