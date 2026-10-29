/* scene/transform.c — hiyerarşi döngü denetimi ve dünya matrisi güncellemesi (quaternion işlemleri core/quat.h).
 * Ebeveyn-çocuk ağacında bellek içi DFS (memoization) ile her düğümün dünya matrisi tek geçişte
 * hesaplanır; döngü girişimleri hem atama anında hem güncelleme sırasında engellenir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/transform.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "scene/transform.h"

Transform transform_identity(void) {
    Transform t;
    t.pos   = v3(0.0f, 0.0f, 0.0f);
    t.rot   = quat_identity();
    t.scale = v3(1.0f, 1.0f, 1.0f);
    t.world = mat4_identity();
    return t;
}

Transform transform_trs(Vec3 pos, Quat rot, Vec3 scale) {
    Transform t;
    t.pos   = pos;
    t.rot   = rot;
    t.scale = scale;
    t.world = mat4_identity();
    return t;
}

Mat4 transform_local_mat4(const Transform* t) {
    Quat q = quat_normalize(t->rot);
    f32 x = q.x, y = q.y, z = q.z, w = q.w;
    f32 xx = x * x, yy = y * y, zz = z * z;
    f32 xy = x * y, xz = x * z, yz = y * z;
    f32 wx = w * x, wy = w * y, wz = w * z;

    f32 sx = t->scale.x, sy = t->scale.y, sz = t->scale.z;

    Mat4 r;
    r.m[0]  = (1.0f - 2.0f * (yy + zz)) * sx;
    r.m[1]  = (2.0f * (xy + wz)) * sx;
    r.m[2]  = (2.0f * (xz - wy)) * sx;
    r.m[3]  = 0.0f;

    r.m[4]  = (2.0f * (xy - wz)) * sy;
    r.m[5]  = (1.0f - 2.0f * (xx + zz)) * sy;
    r.m[6]  = (2.0f * (yz + wx)) * sy;
    r.m[7]  = 0.0f;

    r.m[8]  = (2.0f * (xz + wy)) * sz;
    r.m[9]  = (2.0f * (yz - wx)) * sz;
    r.m[10] = (1.0f - 2.0f * (xx + yy)) * sz;
    r.m[11] = 0.0f;

    r.m[12] = t->pos.x;
    r.m[13] = t->pos.y;
    r.m[14] = t->pos.z;
    r.m[15] = 1.0f;
    return r;
}

Vec3 transform_world_point(const Transform* t, Vec3 local_pt) {
    Vec4 r = mat4_mul_v4(&t->world, v4(local_pt.x, local_pt.y, local_pt.z, 1.0f));
    return v3(r.x, r.y, r.z);
}

void transform_register(EcsWorld* w) {
    static const EcsFieldDesc k_transform_fields[] = {
        { "pos",   ECS_FIELD_VEC3, ECS_OFFSET(Transform, pos)   },
        { "rot",   ECS_FIELD_QUAT, ECS_OFFSET(Transform, rot)   },
        { "scale", ECS_FIELD_VEC3, ECS_OFFSET(Transform, scale) }
    };
    static const EcsCompDesc k_transform_desc = {
        k_transform_fields, ARRAY_COUNT(k_transform_fields)
    };

    static const EcsFieldDesc k_parent_fields[] = {
        { "parent", ECS_FIELD_ENTITY, ECS_OFFSET(Parent, parent) }
    };
    static const EcsCompDesc k_parent_desc = {
        k_parent_fields, ARRAY_COUNT(k_parent_fields)
    };

    w->transform_cid = ecs_register(w, "Transform", sizeof(Transform), _Alignof(Transform), &k_transform_desc);
    w->parent_cid    = ecs_register(w, "Parent",    sizeof(Parent),    _Alignof(Parent),    &k_parent_desc);
}

EcsCompId transform_comp_id(const EcsWorld* w) {
    return w->transform_cid;
}

EcsCompId parent_comp_id(const EcsWorld* w) {
    return w->parent_cid;
}

EcsEntity transform_get_parent(const EcsWorld* w, EcsEntity child) {
    if (w->parent_cid == ECS_COMP_NIL) return ECS_NULL;
    const Parent* p = (const Parent*)ecs_get_const(w, child, w->parent_cid);
    if (!p || !ecs_alive(w, p->parent)) return ECS_NULL;
    return p->parent;
}

b32 transform_set_parent(EcsWorld* w, EcsEntity child, EcsEntity parent) {
    if (!ecs_alive(w, child)) return FALSE;
    if (w->parent_cid == ECS_COMP_NIL) {
        transform_register(w);
    }
    if (parent == ECS_NULL) {
        ecs_remove(w, child, w->parent_cid);
        return TRUE;
    }
    if (!ecs_alive(w, parent)) return FALSE;
    if (child == parent) return FALSE;

    /* Ata zincirini yürüyerek döngü oluşup oluşmadığını denetle */
    EcsEntity cur = parent;
    u32 steps = 0u;
    while (cur != ECS_NULL) {
        if (cur == child) return FALSE;
        if (++steps > w->ent_cap) return FALSE;
        cur = transform_get_parent(w, cur);
    }

    Parent comp = { parent };
    ecs_set(w, child, w->parent_cid, &comp);
    return TRUE;
}

/* Özyinelemesiz: e'den köke doğru henüz hesaplanmamış Transform'lu ataları yığına toplar, sonra kökten
 * aşağı doğru dünya matrislerini kurar. visit_state: 0 = bekliyor, 1 = yığında, 2 = hesaplandı.
 * Döngü (yığındaki bir ataya geri dönüş) ebeveynsiz kabul edilir. Yalnız Transform'lu entity'ler işaretlenir. */
static void transform_eval_entity(EcsWorld* w, EcsEntity e) {
    u32* stack = w->walk_stack;
    u32 n = 0u;
    EcsEntity cur = e;
    while (cur != ECS_NULL) {
        u32 idx = ecs_entity_index(cur);
        if (w->visit_state[idx] != 0u) break;
        if (!ecs_get(w, cur, w->transform_cid)) break;
        w->visit_state[idx] = 1u;
        stack[n++] = cur;
        cur = transform_get_parent(w, cur);
    }

    const Transform* base = NULL;
    if (cur != ECS_NULL && w->visit_state[ecs_entity_index(cur)] == 2u) {
        base = (const Transform*)ecs_get_const(w, cur, w->transform_cid);
    }
    while (n > 0u) {
        EcsEntity x = stack[--n];
        stack[n] = 0u;
        Transform* t = (Transform*)ecs_get(w, x, w->transform_cid);
        Mat4 local = transform_local_mat4(t);
        t->world = base ? mat4_mul(base->world, local) : local;
        w->visit_state[ecs_entity_index(x)] = 2u;
        base = t;
    }
}

void transform_update(EcsWorld* w) {
    if (!w || w->transform_cid == ECS_COMP_NIL || w->transform_cid >= w->comp_count) return;
    EcsStore* ts = &w->stores[w->transform_cid];
    if (ts->count == 0u) return;

    for (u32 i = 0u; i < ts->count; i++) {
        EcsEntity e = ts->dense_ent[i];
        if (w->visit_state[ecs_entity_index(e)] == 0u) {
            transform_eval_entity(w, e);
        }
    }

    for (u32 i = 0u; i < ts->count; i++) {
        w->visit_state[ecs_entity_index(ts->dense_ent[i])] = 0u;
    }
}
