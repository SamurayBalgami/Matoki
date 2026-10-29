/* scene/ecs.h — Seyrek küme (sparse set) tabanlı Entity-Component-System çekirdeği.
 * Entity = 32-bit tutamak {indeks:20, nesil:12}; silinen entity tutamakları nesil ile geçersizleşir.
 * Bileşen depoları O(1) ekleme/silme ve önbellek dostu yoğun dizi yinelemesi sağlar.
 * Yineleme sırasında yapılan silme işlemleri komut tamponunda ertelenir ve sorgu bitiminde uygulanır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/ecs.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef SCENE_ECS_H
#define SCENE_ECS_H

#include "core/types.h"
#include "core/arena.h"

typedef u32 EcsEntity;
typedef u32 EcsCompId;

#define ECS_NULL          0u
#define ECS_INDEX_BITS    20u
#define ECS_GEN_BITS      12u
#define ECS_INDEX_MASK    ((1u << ECS_INDEX_BITS) - 1u)
#define ECS_GEN_MASK      ((1u << ECS_GEN_BITS) - 1u)

#define ECS_MAX_ENTITIES  ((1u << ECS_INDEX_BITS) - 1u)
#define ECS_MAX_COMPS     32u
#define ECS_MAX_FIELDS    16u
#define ECS_SPARSE_NIL    U32_MAX
#define ECS_COMP_NIL      U32_MAX

#define ECS_FLAG_PENDING_DESTROY 1u

static inline u32 ecs_entity_index(EcsEntity e) {
    return e & ECS_INDEX_MASK;
}

static inline u32 ecs_entity_gen(EcsEntity e) {
    return (e >> ECS_INDEX_BITS) & ECS_GEN_MASK;
}

static inline EcsEntity ecs_entity_make(u32 index, u32 gen) {
    return ((gen & ECS_GEN_MASK) << ECS_INDEX_BITS) | (index & ECS_INDEX_MASK);
}

/* Serileştirme ve yansıma (reflection) için bileşen alan tipleri */
typedef enum {
    ECS_FIELD_F32 = 0,
    ECS_FIELD_I32,
    ECS_FIELD_U32,
    ECS_FIELD_B32,
    ECS_FIELD_VEC3,
    ECS_FIELD_VEC4,
    ECS_FIELD_QUAT,
    ECS_FIELD_ENTITY,
    ECS_FIELD_STR_ID
} EcsFieldType;

typedef struct {
    const char*  name;
    EcsFieldType type;
    u32          offset;
} EcsFieldDesc;

typedef struct {
    const EcsFieldDesc* fields;
    u32                 field_count;
} EcsCompDesc;

#define ECS_OFFSET(T, field) ((u32)__builtin_offsetof(T, field))

/* Tip başına seyrek küme (sparse set) bileşen deposu */
typedef struct {
    const char*  name;
    u32          size;
    u32          align;
    u32          stride;
    EcsFieldDesc fields[ECS_MAX_FIELDS];
    u32          field_count;

    u32*         sparse;      /* [world->ent_cap] entity_index -> dense_index (yoksa ECS_SPARSE_NIL) */
    EcsEntity*   dense_ent;   /* [dense_cap] yoğun dizideki entity tutamakları */
    u8*          data;        /* [dense_cap * stride] yoğun bileşen verisi */
    u32          count;
    u32          dense_cap;
} EcsStore;

typedef enum {
    ECS_CMD_DESTROY = 0,
    ECS_CMD_REMOVE  = 1
} EcsCmdType;

typedef struct {
    u8        type;
    EcsCompId cid;
    EcsEntity entity;
} EcsCmd;

typedef struct EcsWorld {
    Arena*     arena;

    /* Entity yuvaları */
    u32        ent_cap;
    u32        ent_high_water;
    u32        alive_count;
    EcsEntity* ent_handle;    /* [ent_cap] canlıysa tam tutamak, ölüyse U32_MAX */
    u16*       ent_gen;       /* [ent_cap] 12-bit nesil sayacı (1..4095) */
    u32*       ent_mask;      /* [ent_cap] bileşen bit maskesi */
    u8*        ent_flags;     /* [ent_cap] ertelenmiş silme vb. durum bayrakları */
    u8*        visit_state;   /* [ent_cap] hiyerarşi güncellemesi için geçici ziyaret durumu */
    u32*       walk_stack;    /* [ent_cap] hiyerarşi güncellemesi için özyinelemesiz yürüme yığını */

    /* Serbest entity indeks yığını */
    u32*       free_list;     /* [ent_cap] */
    u32        free_count;

    /* Kayıtlı bileşen depoları */
    EcsStore   stores[ECS_MAX_COMPS];
    u32        comp_count;

    /* Özel yerleşik bileşen kimlikleri (varsa) */
    EcsCompId  transform_cid;
    EcsCompId  parent_cid;

    /* Sorgu yinelemesi sırasında ertelenen komut tamponu */
    u32        iter_depth;
    EcsCmd*    cmds;
    u32        cmd_count;
    u32        cmd_cap;

    /* Dize havuzu (str_id alanları için) */
    const char** strings;
    u32          str_count;
    u32          str_cap;
} EcsWorld;

typedef struct {
    EcsWorld* world;
    u32       mask;
    EcsStore* driver;
    u32       cursor;
    EcsEntity entity;
    b32       active;
} EcsQuery;

/* Dünya kurulumu ve sıfırlama */
void      ecs_world_init(EcsWorld* w, Arena* a, u32 max_entities);
EcsWorld* ecs_world_create(Arena* a, u32 max_entities);
void      ecs_world_clear(EcsWorld* w);

/* Bileşen kaydı ve arama */
EcsCompId ecs_register(EcsWorld* w, const char* name, u32 size, u32 align, const EcsCompDesc* desc);
EcsCompId ecs_find_comp(const EcsWorld* w, const char* name);

/* Entity yaşam döngüsü */
EcsEntity ecs_spawn(EcsWorld* w);
void      ecs_destroy(EcsWorld* w, EcsEntity e);
u32       ecs_alive_count(const EcsWorld* w);

static inline b32 ecs_alive(const EcsWorld* w, EcsEntity e) {
    u32 idx = ecs_entity_index(e);
    return idx < w->ent_cap && w->ent_handle[idx] == e;
}

/* Bileşen ekleme / okuma / silme */
void* ecs_set(EcsWorld* w, EcsEntity e, EcsCompId cid, const void* data);
void* ecs_add(EcsWorld* w, EcsEntity e, EcsCompId cid);
b32   ecs_remove(EcsWorld* w, EcsEntity e, EcsCompId cid);

static inline b32 ecs_has(const EcsWorld* w, EcsEntity e, EcsCompId cid) {
    u32 idx = ecs_entity_index(e);
    if (idx >= w->ent_cap || w->ent_handle[idx] != e || cid >= w->comp_count) return FALSE;
    return (w->ent_mask[idx] & (1u << cid)) != 0u;
}

static inline void* ecs_get(EcsWorld* w, EcsEntity e, EcsCompId cid) {
    u32 idx = ecs_entity_index(e);
    if (idx >= w->ent_cap || cid >= w->comp_count) return NULL;
    EcsStore* s = &w->stores[cid];
    u32 dense = s->sparse[idx];
    if (dense == ECS_SPARSE_NIL || s->dense_ent[dense] != e) return NULL;
    return s->data + (usize)dense * s->stride;
}

static inline const void* ecs_get_const(const EcsWorld* w, EcsEntity e, EcsCompId cid) {
    u32 idx = ecs_entity_index(e);
    if (idx >= w->ent_cap || cid >= w->comp_count) return NULL;
    const EcsStore* s = &w->stores[cid];
    u32 dense = s->sparse[idx];
    if (dense == ECS_SPARSE_NIL || s->dense_ent[dense] != e) return NULL;
    return s->data + (usize)dense * s->stride;
}

/* Sorgu ve komut tamponu */
EcsQuery ecs_query(EcsWorld* w, u32 mask);
b32      ecs_query_next(EcsQuery* q);
void     ecs_query_end(EcsQuery* q);
void     ecs_flush(EcsWorld* w);

/* Sorgu döngüsü içinde doğrulanmış aktif entity'nin bileşenine hızlı erişim */
static inline void* ecs_query_get(const EcsQuery* q, EcsCompId cid) {
    const EcsStore* s = &q->world->stores[cid];
    u32 dense = s->sparse[ecs_entity_index(q->entity)];
    return s->data + (usize)dense * (usize)s->stride;
}

/* Dize internleme (ECS_FIELD_STR_ID için) */
u32         ecs_str_intern(EcsWorld* w, const char* s);
const char* ecs_str_get(const EcsWorld* w, u32 id);

#endif
