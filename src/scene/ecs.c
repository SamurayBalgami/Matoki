/* scene/ecs.c — Seyrek küme (sparse set) tabanlı ECS dünya, bileşen deposu ve sorgu motoru.
 * Tüm bellek dünya arenası üzerinden ayrılır; bileşen silme swap-and-pop ile O(1) zamanda çalışır.
 * Yineleme sırasında silinen entity/bileşenler komut tamponuna yazılır ve sorgu sonunda temizlenir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/ecs.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "scene/ecs.h"
#include "core/fmt.h"

static u32 ecs_strlen(const char* s) {
    if (!s) return 0u;
    u32 n = 0u;
    while (s[n]) n++;
    return n;
}

static b32 ecs_streq(const char* a, const char* b) {
    if (a == b) return TRUE;
    if (!a || !b) return FALSE;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const char* ecs_strdup(Arena* a, const char* s) {
    if (!s) return "";
    u32 len = ecs_strlen(s);
    char* copy = (char*)arena_alloc(a, (usize)len + 1u, 1u);
    __builtin_memcpy(copy, s, len);
    copy[len] = 0;
    return copy;
}

void ecs_world_init(EcsWorld* w, Arena* a, u32 max_entities) {
    __builtin_memset(w, 0, sizeof(*w));
    if (max_entities == 0u) max_entities = 1024u;
    if (max_entities > ECS_MAX_ENTITIES) max_entities = ECS_MAX_ENTITIES;

    w->arena          = a;
    w->ent_cap        = max_entities;
    w->ent_high_water = 0u;
    w->alive_count    = 0u;

    w->ent_handle  = ARENA_ARRAY(a, EcsEntity, max_entities);
    w->ent_gen     = ARENA_ARRAY(a, u16, max_entities);
    w->ent_mask    = ARENA_ARRAY(a, u32, max_entities);
    w->ent_flags   = ARENA_ARRAY(a, u8, max_entities);
    w->visit_state = ARENA_ARRAY(a, u8, max_entities);
    w->free_list   = ARENA_ARRAY(a, u32, max_entities);
    w->walk_stack  = ARENA_ARRAY(a, u32, max_entities);

    __builtin_memset(w->ent_handle, 0xFF, sizeof(EcsEntity) * (usize)max_entities);

    w->transform_cid = ECS_COMP_NIL;
    w->parent_cid    = ECS_COMP_NIL;

    w->cmd_cap = 256u;
    w->cmds    = ARENA_ARRAY(a, EcsCmd, w->cmd_cap);

    w->str_cap = 64u;
    w->strings = ARENA_ARRAY(a, const char*, w->str_cap);
}

EcsWorld* ecs_world_create(Arena* a, u32 max_entities) {
    EcsWorld* w = ARENA_NEW(a, EcsWorld);
    ecs_world_init(w, a, max_entities);
    return w;
}

void ecs_world_clear(EcsWorld* w) {
    for (u32 c = 0u; c < w->comp_count; c++) {
        EcsStore* s = &w->stores[c];
        if (s->count > 0u) {
            __builtin_memset(s->dense_ent, 0, sizeof(EcsEntity) * (usize)s->count);
            if (s->stride > 0u) {
                __builtin_memset(s->data, 0, (usize)s->count * (usize)s->stride);
            }
        }
        s->count = 0u;
        __builtin_memset(s->sparse, 0xFF, sizeof(u32) * (usize)w->ent_cap);
    }
    for (u32 i = 0u; i < w->ent_high_water; i++) {
        u32 next_gen = ((u32)w->ent_gen[i] + 1u) & ECS_GEN_MASK;
        if (next_gen == 0u) next_gen = 1u;
        w->ent_gen[i] = (u16)next_gen;
    }
    __builtin_memset(w->ent_handle, 0xFF, sizeof(EcsEntity) * (usize)w->ent_cap);
    __builtin_memset(w->ent_mask, 0, sizeof(u32) * (usize)w->ent_cap);
    __builtin_memset(w->ent_flags, 0, sizeof(u8) * (usize)w->ent_cap);
    if (w->free_count > 0u) {
        __builtin_memset(w->free_list, 0, sizeof(u32) * (usize)w->free_count);
    }
    if (w->cmd_count > 0u) {
        __builtin_memset(w->cmds, 0, sizeof(EcsCmd) * (usize)w->cmd_count);
    }
    w->ent_high_water = 0u;
    w->alive_count    = 0u;
    w->free_count     = 0u;
    w->iter_depth     = 0u;
    w->cmd_count      = 0u;
}

EcsCompId ecs_find_comp(const EcsWorld* w, const char* name) {
    if (!name) return ECS_COMP_NIL;
    for (u32 i = 0u; i < w->comp_count; i++) {
        if (ecs_streq(w->stores[i].name, name)) return i;
    }
    return ECS_COMP_NIL;
}

EcsCompId ecs_register(EcsWorld* w, const char* name, u32 size, u32 align, const EcsCompDesc* desc) {
    EcsCompId existing = ecs_find_comp(w, name);
    if (existing != ECS_COMP_NIL) return existing;

    ASSERT(w->comp_count < ECS_MAX_COMPS);
    if (align == 0u) align = 4u;
    ASSERT(IS_POW2(align));

    EcsCompId cid = w->comp_count++;
    EcsStore* s   = &w->stores[cid];
    __builtin_memset(s, 0, sizeof(*s));

    s->name   = ecs_strdup(w->arena, name);
    s->size   = size;
    s->align  = align;
    s->stride = (size == 0u) ? 0u : ALIGN_UP(size, align);

    if (desc && desc->fields && desc->field_count > 0u) {
        ASSERT(desc->field_count <= ECS_MAX_FIELDS);
        s->field_count = desc->field_count;
        for (u32 i = 0u; i < desc->field_count; i++) {
            static const u32 k_field_size[] = { 4, 4, 4, 4, 12, 16, 16, 4, 4 };
            ASSERT((u32)desc->fields[i].type < ARRAY_COUNT(k_field_size));
            ASSERT(desc->fields[i].offset + k_field_size[desc->fields[i].type] <= size);
            s->fields[i].name   = ecs_strdup(w->arena, desc->fields[i].name);
            s->fields[i].type   = desc->fields[i].type;
            s->fields[i].offset = desc->fields[i].offset;
        }
    }

    s->sparse = ARENA_ARRAY(w->arena, u32, w->ent_cap);
    __builtin_memset(s->sparse, 0xFF, sizeof(u32) * (usize)w->ent_cap);

    u32 init_cap = (w->ent_cap < 256u) ? w->ent_cap : 256u;
    if (init_cap == 0u) init_cap = 16u;
    s->dense_cap = init_cap;
    s->dense_ent = ARENA_ARRAY(w->arena, EcsEntity, s->dense_cap);
    if (s->stride > 0u) {
        usize data_align = (s->align > 16u) ? (usize)s->align : 16u;
        s->data = (u8*)arena_alloc(w->arena, (usize)s->dense_cap * (usize)s->stride, data_align);
    }

    if (ecs_streq(s->name, "Transform")) w->transform_cid = cid;
    if (ecs_streq(s->name, "Parent"))    w->parent_cid    = cid;

    return cid;
}

static void ecs_store_grow(EcsWorld* w, EcsStore* s) {
    u32 new_cap = s->dense_cap * 2u;
    if (new_cap < s->dense_cap || new_cap > w->ent_cap) new_cap = w->ent_cap;
    ASSERT(new_cap > s->dense_cap);

    EcsEntity* new_ent = ARENA_ARRAY(w->arena, EcsEntity, new_cap);
    __builtin_memcpy(new_ent, s->dense_ent, sizeof(EcsEntity) * (usize)s->count);
    s->dense_ent = new_ent;

    if (s->stride > 0u) {
        usize data_align = (s->align > 16u) ? (usize)s->align : 16u;
        u8* new_data = (u8*)arena_alloc(w->arena, (usize)new_cap * (usize)s->stride, data_align);
        __builtin_memcpy(new_data, s->data, (usize)s->count * (usize)s->stride);
        s->data = new_data;
    }
    s->dense_cap = new_cap;
}

static void ecs_push_cmd(EcsWorld* w, u8 type, EcsCompId cid, EcsEntity e) {
    if (w->cmd_count == w->cmd_cap) {
        u32 new_cap = w->cmd_cap * 2u;
        EcsCmd* new_cmds = ARENA_ARRAY(w->arena, EcsCmd, new_cap);
        __builtin_memcpy(new_cmds, w->cmds, sizeof(EcsCmd) * (usize)w->cmd_count);
        w->cmds    = new_cmds;
        w->cmd_cap = new_cap;
    }
    EcsCmd* cmd = &w->cmds[w->cmd_count++];
    cmd->type   = type;
    cmd->cid    = cid;
    cmd->entity = e;
}

EcsEntity ecs_spawn(EcsWorld* w) {
    u32 idx;
    if (w->free_count > 0u) {
        idx = w->free_list[--w->free_count];
        w->free_list[w->free_count] = 0u;
    } else {
        ASSERT(w->ent_high_water < w->ent_cap);
        idx = w->ent_high_water++;
    }

    if (w->ent_gen[idx] == 0u) w->ent_gen[idx] = 1u;
    EcsEntity e = ecs_entity_make(idx, (u32)w->ent_gen[idx]);
    w->ent_handle[idx] = e;
    w->ent_mask[idx]   = 0u;
    w->ent_flags[idx]  = 0u;
    w->alive_count++;
    return e;
}

u32 ecs_alive_count(const EcsWorld* w) {
    return w->alive_count;
}

static b32 ecs_store_remove_idx(EcsWorld* w, EcsCompId cid, u32 idx) {
    EcsStore* s = &w->stores[cid];
    u32 dense = s->sparse[idx];
    if (dense == ECS_SPARSE_NIL) return FALSE;

    u32 last = --s->count;
    if (dense != last) {
        EcsEntity last_ent = s->dense_ent[last];
        u32 last_idx       = ecs_entity_index(last_ent);
        s->dense_ent[dense] = last_ent;
        s->sparse[last_idx] = dense;
        if (s->stride > 0u) {
            __builtin_memcpy(s->data + (usize)dense * (usize)s->stride,
                             s->data + (usize)last * (usize)s->stride,
                             s->stride);
        }
    }
    s->dense_ent[last] = 0u;
    if (s->stride > 0u) {
        __builtin_memset(s->data + (usize)last * (usize)s->stride, 0, s->stride);
    }
    s->sparse[idx] = ECS_SPARSE_NIL;
    w->ent_mask[idx] &= ~(1u << cid);
    return TRUE;
}

static void ecs_destroy_single(EcsWorld* w, EcsEntity e) {
    if (!ecs_alive(w, e)) return;
    u32 idx  = ecs_entity_index(e);
    u32 mask = w->ent_mask[idx];
    for (u32 cid = 0u; cid < w->comp_count && mask != 0u; cid++) {
        if (mask & (1u << cid)) {
            ecs_store_remove_idx(w, cid, idx);
            mask &= ~(1u << cid);
        }
    }

    w->ent_handle[idx] = U32_MAX;
    w->ent_mask[idx]   = 0u;
    w->ent_flags[idx]  = 0u;

    u32 next_gen = ((u32)w->ent_gen[idx] + 1u) & ECS_GEN_MASK;
    if (next_gen == 0u) next_gen = 1u;
    w->ent_gen[idx] = (u16)next_gen;

    w->free_list[w->free_count++] = idx;
    w->alive_count--;
}

static void ecs_destroy_immediate(EcsWorld* w, EcsEntity e) {
    if (!ecs_alive(w, e)) return;
    ecs_destroy_single(w, e);

    /* Ebeveyn silindiğinde çocuk (ve torun) entity'leri de kademeli olarak sil */
    if (w->parent_cid != ECS_COMP_NIL && w->parent_cid < w->comp_count) {
        EcsStore* ps = &w->stores[w->parent_cid];
        b32 changed = TRUE;
        while (changed && ps->count > 0u) {
            changed = FALSE;
            u32 i = 0u;
            while (i < ps->count) {
                EcsEntity parent_ent = ECS_NULL;
                __builtin_memcpy(&parent_ent, ps->data + (usize)i * (usize)ps->stride, sizeof(EcsEntity));
                if (parent_ent != ECS_NULL && !ecs_alive(w, parent_ent)) {
                    EcsEntity child_ent = ps->dense_ent[i];
                    ecs_destroy_single(w, child_ent);
                    changed = TRUE;
                } else {
                    i++;
                }
            }
        }
    }
}

void ecs_destroy(EcsWorld* w, EcsEntity e) {
    if (!ecs_alive(w, e)) return;
    if (w->iter_depth > 0u) {
        u32 idx = ecs_entity_index(e);
        if (!(w->ent_flags[idx] & ECS_FLAG_PENDING_DESTROY)) {
            w->ent_flags[idx] |= ECS_FLAG_PENDING_DESTROY;
            ecs_push_cmd(w, (u8)ECS_CMD_DESTROY, 0u, e);
        }
        return;
    }
    ecs_destroy_immediate(w, e);
}

void* ecs_set(EcsWorld* w, EcsEntity e, EcsCompId cid, const void* data) {
    if (!ecs_alive(w, e) || cid >= w->comp_count) return NULL;
    u32 idx     = ecs_entity_index(e);
    EcsStore* s = &w->stores[cid];
    u32 dense   = s->sparse[idx];

    if (dense == ECS_SPARSE_NIL) {
        if (s->count == s->dense_cap) {
            ecs_store_grow(w, s);
        }
        dense = s->count++;
        s->sparse[idx]     = dense;
        s->dense_ent[dense] = e;
        w->ent_mask[idx] |= (1u << cid);

        u8* dst = s->data + (usize)dense * (usize)s->stride;
        if (s->stride > 0u) {
            if (s->stride > s->size) {
                __builtin_memset(dst + s->size, 0, s->stride - s->size);
            }
            if (s->size > 0u) {
                if (data) __builtin_memcpy(dst, data, s->size);
                else      __builtin_memset(dst, 0, s->size);
            }
        }
        return dst;
    } else {
        u8* dst = s->data + (usize)dense * (usize)s->stride;
        if (data && s->size > 0u) {
            __builtin_memcpy(dst, data, s->size);
        }
        return dst;
    }
}

void* ecs_add(EcsWorld* w, EcsEntity e, EcsCompId cid) {
    return ecs_set(w, e, cid, NULL);
}

b32 ecs_remove(EcsWorld* w, EcsEntity e, EcsCompId cid) {
    if (!ecs_alive(w, e) || cid >= w->comp_count) return FALSE;
    u32 idx = ecs_entity_index(e);
    if (w->stores[cid].sparse[idx] == ECS_SPARSE_NIL) return FALSE;

    if (w->iter_depth > 0u) {
        ecs_push_cmd(w, (u8)ECS_CMD_REMOVE, cid, e);
        return TRUE;
    }
    return ecs_store_remove_idx(w, cid, idx);
}

void ecs_flush(EcsWorld* w) {
    if (w->iter_depth > 0u || w->cmd_count == 0u) return;
    u32 n = w->cmd_count;
    w->cmd_count = 0u;
    for (u32 i = 0u; i < n; i++) {
        EcsCmd cmd = w->cmds[i];
        __builtin_memset(&w->cmds[i], 0, sizeof(EcsCmd));
        if (cmd.type == (u8)ECS_CMD_DESTROY) {
            ecs_destroy_immediate(w, cmd.entity);
        } else if (cmd.type == (u8)ECS_CMD_REMOVE) {
            if (ecs_alive(w, cmd.entity) && cmd.cid < w->comp_count) {
                ecs_store_remove_idx(w, cmd.cid, ecs_entity_index(cmd.entity));
            }
        }
    }
}

EcsQuery ecs_query(EcsWorld* w, u32 mask) {
    EcsQuery q;
    q.world  = w;
    q.mask   = mask;
    q.driver = NULL;
    q.cursor = 0u;
    q.entity = ECS_NULL;
    q.active = FALSE;

    if (!w || mask == 0u) return q;

    u32 min_count = U32_MAX;
    for (u32 cid = 0u; cid < w->comp_count; cid++) {
        if (mask & (1u << cid)) {
            EcsStore* s = &w->stores[cid];
            if (s->count < min_count) {
                min_count = s->count;
                q.driver  = s;
            }
        }
    }
    /* Maskede henüz kayıtlı olmayan bir bit varsa eşleşme olamaz */
    u32 valid_bits = (w->comp_count == 32u) ? U32_MAX : ((1u << w->comp_count) - 1u);
    if ((mask & ~valid_bits) != 0u) {
        q.driver = NULL;
    }
    return q;
}

b32 ecs_query_next(EcsQuery* q) {
    EcsStore* driver = q->driver;
    if (!driver) return FALSE;

    EcsWorld* w = q->world;
    if (!q->active) {
        q->active = TRUE;
        w->iter_depth++;
    }

    u32 mask  = q->mask;
    u32 count = driver->count;
    const EcsEntity* dense_ent = driver->dense_ent;
    const u32* ent_mask        = w->ent_mask;
    const u8*  ent_flags       = w->ent_flags;

    while (q->cursor < count) {
        EcsEntity e = dense_ent[q->cursor++];
        u32 idx     = ecs_entity_index(e);
        if ((ent_mask[idx] & mask) == mask && !(ent_flags[idx] & ECS_FLAG_PENDING_DESTROY)) {
            q->entity = e;
            return TRUE;
        }
    }

    q->entity = ECS_NULL;
    if (q->active) {
        q->active = FALSE;
        if (w->iter_depth > 0u) w->iter_depth--;
        if (w->iter_depth == 0u && w->cmd_count > 0u) {
            ecs_flush(w);
        }
    }
    return FALSE;
}

void ecs_query_end(EcsQuery* q) {
    if (q && q->active && q->world) {
        q->active = FALSE;
        if (q->world->iter_depth > 0u) q->world->iter_depth--;
        if (q->world->iter_depth == 0u && q->world->cmd_count > 0u) {
            ecs_flush(q->world);
        }
    }
}

u32 ecs_str_intern(EcsWorld* w, const char* s) {
    if (!s || s[0] == 0) return 0u;
    for (u32 i = 0u; i < w->str_count; i++) {
        if (ecs_streq(w->strings[i], s)) return i + 1u;
    }
    if (w->str_count == w->str_cap) {
        u32 new_cap = w->str_cap * 2u;
        const char** new_strs = ARENA_ARRAY(w->arena, const char*, new_cap);
        __builtin_memcpy(new_strs, w->strings, sizeof(const char*) * (usize)w->str_count);
        w->strings = new_strs;
        w->str_cap = new_cap;
    }
    w->strings[w->str_count] = ecs_strdup(w->arena, s);
    w->str_count++;
    return w->str_count;
}

const char* ecs_str_get(const EcsWorld* w, u32 id) {
    if (id == 0u || id > w->str_count) return "";
    return w->strings[id - 1u];
}
