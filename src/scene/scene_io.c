/* scene/scene_io.c — Yansıma (reflection) tabanlı sahne JSON kaydetme ve yükleme.
 * Kaydetme: canlı entity'lere 0..N-1 sıra kimliği verir ve bileşenleri kayıt sırasıyla JSON DOM'a kurar.
 * Yükleme: iki geçişli (1. geçiş entity oluşturma ve ID tablosu, 2. geçiş bileşen ve referans çözümü).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/scene/scene_io.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "scene/scene_io.h"
#include "scene/transform.h"
#include "asset/json.h"

static u32 sio_strlen(const char* s) {
    if (!s) return 0u;
    u32 n = 0u;
    while (s[n]) n++;
    return n;
}

static b32 sio_streq(const char* a, const char* b) {
    if (a == b) return TRUE;
    if (!a || !b) return FALSE;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void sio_set_err(SceneIoResult* res, const char* msg) {
    if (!res || res->err[0]) return;
    u32 i = 0u;
    while (msg[i] && i + 1u < sizeof(res->err)) {
        res->err[i] = msg[i];
        i++;
    }
    res->err[i] = 0;
}

static void sio_add_warn(SceneIoResult* res, const char* prefix, const char* name) {
    if (!res) return;
    res->warnings++;
    u32 n = 0u;
    while (res->warn[n]) n++;
    if (n + 4u >= sizeof(res->warn)) return;
    if (n > 0u) {
        res->warn[n++] = ';';
        res->warn[n++] = ' ';
    }
    for (u32 i = 0u; prefix[i] && n + 1u < sizeof(res->warn); i++) {
        res->warn[n++] = prefix[i];
    }
    if (name && name[0] && n + 2u < sizeof(res->warn)) {
        res->warn[n++] = ':';
        for (u32 i = 0u; name[i] && n + 1u < sizeof(res->warn); i++) {
            res->warn[n++] = name[i];
        }
    }
    res->warn[n] = 0;
}

static void sio_build_f32_array(Arena* a, JsonValue* out, const f32* vals, u32 count) {
    out->type     = JSON_ARRAY;
    out->count    = count;
    out->as.items = ARENA_ARRAY(a, JsonValue, count);
    for (u32 i = 0u; i < count; i++) {
        out->as.items[i].type   = JSON_NUMBER;
        out->as.items[i].count  = 0u;
        out->as.items[i].as.num = (f64)vals[i];
    }
}

static void sio_serialize_field(const EcsWorld* w, Arena* a, const i32* save_map,
                                const EcsFieldDesc* fd, const u8* base, JsonValue* out) {
    const u8* ptr = base + fd->offset;
    out->count = 0u;

    switch (fd->type) {
    case ECS_FIELD_F32: {
        f32 v = 0.0f;
        __builtin_memcpy(&v, ptr, sizeof(f32));
        out->type   = JSON_NUMBER;
        out->as.num = (f64)v;
    } break;
    case ECS_FIELD_I32: {
        i32 v = 0;
        __builtin_memcpy(&v, ptr, sizeof(i32));
        out->type   = JSON_NUMBER;
        out->as.num = (f64)v;
    } break;
    case ECS_FIELD_U32: {
        u32 v = 0u;
        __builtin_memcpy(&v, ptr, sizeof(u32));
        out->type   = JSON_NUMBER;
        out->as.num = (f64)v;
    } break;
    case ECS_FIELD_B32: {
        b32 v = FALSE;
        __builtin_memcpy(&v, ptr, sizeof(b32));
        out->type = JSON_BOOL;
        out->as.b = (v != 0u) ? TRUE : FALSE;
    } break;
    case ECS_FIELD_VEC3: {
        f32 v[3];
        __builtin_memcpy(v, ptr, sizeof(v));
        sio_build_f32_array(a, out, v, 3u);
    } break;
    case ECS_FIELD_VEC4:
    case ECS_FIELD_QUAT: {
        f32 v[4];
        __builtin_memcpy(v, ptr, sizeof(v));
        sio_build_f32_array(a, out, v, 4u);
    } break;
    case ECS_FIELD_ENTITY: {
        EcsEntity ref = ECS_NULL;
        __builtin_memcpy(&ref, ptr, sizeof(EcsEntity));
        i32 ref_id = -1;
        if (ecs_alive(w, ref)) {
            u32 r_idx = ecs_entity_index(ref);
            if (r_idx < w->ent_high_water) {
                ref_id = save_map[r_idx];
            }
        }
        out->type   = JSON_NUMBER;
        out->as.num = (f64)ref_id;
    } break;
    case ECS_FIELD_STR_ID: {
        u32 sid = 0u;
        __builtin_memcpy(&sid, ptr, sizeof(u32));
        if (sid <= w->str_count) {
            const char* s = ecs_str_get(w, sid);
            out->type   = JSON_STRING;
            out->as.str = s;
            out->count  = sio_strlen(s);
        } else {
            out->type   = JSON_NUMBER;
            out->as.num = (f64)sid;
        }
    } break;
    }
}

u32 scene_save(const EcsWorld* w, char* buf, u32 cap, Arena* scratch) {
    if (!w || !scratch) return 0u;
    usize mark = arena_mark(scratch);

    u32 hw = w->ent_high_water;
    i32* save_map = ARENA_ARRAY(scratch, i32, hw ? hw : 1u);
    for (u32 i = 0u; i < hw; i++) save_map[i] = -1;

    EcsEntity* live_ents = ARENA_ARRAY(scratch, EcsEntity, w->alive_count ? w->alive_count : 1u);
    u32 live_n = 0u;
    for (u32 idx = 0u; idx < hw; idx++) {
        EcsEntity e = w->ent_handle[idx];
        if (e != U32_MAX && !(w->ent_flags[idx] & ECS_FLAG_PENDING_DESTROY)) {
            save_map[idx] = (i32)live_n;
            live_ents[live_n++] = e;
        }
    }

    JsonValue root;
    root.type       = JSON_OBJECT;
    root.count      = 1u;
    root.as.members = ARENA_ARRAY(scratch, JsonMember, 1u);
    root.as.members[0].key     = "entities";
    root.as.members[0].key_len = 8u;

    JsonValue* ent_arr = &root.as.members[0].value;
    ent_arr->type     = JSON_ARRAY;
    ent_arr->count    = live_n;
    ent_arr->as.items = live_n ? ARENA_ARRAY(scratch, JsonValue, live_n) : NULL;

    for (u32 i = 0u; i < live_n; i++) {
        EcsEntity e = live_ents[i];
        u32 idx     = ecs_entity_index(e);
        u32 mask    = w->ent_mask[idx];

        u32 comp_n = 0u;
        for (u32 cid = 0u; cid < w->comp_count; cid++) {
            if (mask & (1u << cid)) comp_n++;
        }

        JsonValue* eobj  = &ent_arr->as.items[i];
        eobj->type       = JSON_OBJECT;
        eobj->count      = 2u;
        eobj->as.members = ARENA_ARRAY(scratch, JsonMember, 2u);

        eobj->as.members[0].key          = "id";
        eobj->as.members[0].key_len      = 2u;
        eobj->as.members[0].value.type   = JSON_NUMBER;
        eobj->as.members[0].value.count  = 0u;
        eobj->as.members[0].value.as.num = (f64)i;

        eobj->as.members[1].key     = "components";
        eobj->as.members[1].key_len = 10u;
        JsonValue* comps_obj        = &eobj->as.members[1].value;
        comps_obj->type             = JSON_OBJECT;
        comps_obj->count            = comp_n;
        comps_obj->as.members       = comp_n ? ARENA_ARRAY(scratch, JsonMember, comp_n) : NULL;

        u32 cm_idx = 0u;
        for (u32 cid = 0u; cid < w->comp_count; cid++) {
            if (!(mask & (1u << cid))) continue;
            const EcsStore* s = &w->stores[cid];
            const u8* cdata   = (const u8*)ecs_get_const(w, e, cid);

            JsonMember* cm = &comps_obj->as.members[cm_idx++];
            cm->key        = s->name;
            cm->key_len    = sio_strlen(s->name);

            JsonValue* cobj  = &cm->value;
            cobj->type       = JSON_OBJECT;
            cobj->count      = s->field_count;
            cobj->as.members = s->field_count ? ARENA_ARRAY(scratch, JsonMember, s->field_count) : NULL;

            for (u32 f = 0u; f < s->field_count; f++) {
                const EcsFieldDesc* fd = &s->fields[f];
                JsonMember* fm = &cobj->as.members[f];
                fm->key        = fd->name;
                fm->key_len    = sio_strlen(fd->name);
                sio_serialize_field(w, scratch, save_map, fd, cdata, &fm->value);
            }
        }
    }

    u32 written = json_write(&root, buf, cap);
    arena_reset(scratch, mark);
    return written;
}

static EcsEntity sio_resolve_entity(i64 ref_id, u32 ent_count, const i64* saved_ids,
                                    const EcsEntity* new_ents, SceneIoResult* res) {
    if (ref_id < 0) return ECS_NULL;
    if ((u64)ref_id < (u64)ent_count && saved_ids[ref_id] == ref_id) {
        return new_ents[ref_id];
    }
    for (u32 i = 0u; i < ent_count; i++) {
        if (saved_ids[i] == ref_id) return new_ents[i];
    }
    sio_add_warn(res, "gecersiz entity referansi", NULL);
    return ECS_NULL;
}

static void sio_deserialize_field(EcsWorld* w, const EcsFieldDesc* fd, u8* base,
                                  const JsonValue* val, u32 ent_count,
                                  const i64* saved_ids, const EcsEntity* new_ents,
                                  SceneIoResult* res) {
    u8* ptr = base + fd->offset;

    switch (fd->type) {
    case ECS_FIELD_F32: {
        if (val->type != JSON_NUMBER) { sio_add_warn(res, "f32 tip uyusmazligi", fd->name); return; }
        f32 v = (f32)val->as.num;
        __builtin_memcpy(ptr, &v, sizeof(f32));
    } break;
    case ECS_FIELD_I32: {
        if (val->type != JSON_NUMBER) { sio_add_warn(res, "i32 tip uyusmazligi", fd->name); return; }
        i32 v = (i32)json_int(val, 0);
        __builtin_memcpy(ptr, &v, sizeof(i32));
    } break;
    case ECS_FIELD_U32: {
        if (val->type != JSON_NUMBER) { sio_add_warn(res, "u32 tip uyusmazligi", fd->name); return; }
        u32 v = (u32)json_int(val, 0);
        __builtin_memcpy(ptr, &v, sizeof(u32));
    } break;
    case ECS_FIELD_B32: {
        b32 v = FALSE;
        if (val->type == JSON_BOOL) v = val->as.b ? TRUE : FALSE;
        else if (val->type == JSON_NUMBER) v = (val->as.num != 0.0) ? TRUE : FALSE;
        else { sio_add_warn(res, "b32 tip uyusmazligi", fd->name); return; }
        __builtin_memcpy(ptr, &v, sizeof(b32));
    } break;
    case ECS_FIELD_VEC3: {
        if (val->type != JSON_ARRAY || val->count != 3u) {
            sio_add_warn(res, "vec3 tip uyusmazligi", fd->name);
            return;
        }
        f32 v[3];
        for (u32 i = 0u; i < 3u; i++) v[i] = (f32)json_num(&val->as.items[i], 0.0);
        __builtin_memcpy(ptr, v, sizeof(v));
    } break;
    case ECS_FIELD_VEC4:
    case ECS_FIELD_QUAT: {
        if (val->type != JSON_ARRAY || val->count != 4u) {
            sio_add_warn(res, "vec4/quat tip uyusmazligi", fd->name);
            return;
        }
        f32 v[4];
        for (u32 i = 0u; i < 4u; i++) v[i] = (f32)json_num(&val->as.items[i], 0.0);
        __builtin_memcpy(ptr, v, sizeof(v));
    } break;
    case ECS_FIELD_ENTITY: {
        EcsEntity ref = ECS_NULL;
        if (val->type == JSON_NULL) {
            ref = ECS_NULL;
        } else if (val->type == JSON_NUMBER) {
            i64 ref_id = json_int(val, -1);
            ref = sio_resolve_entity(ref_id, ent_count, saved_ids, new_ents, res);
        } else {
            sio_add_warn(res, "entity tip uyusmazligi", fd->name);
            return;
        }
        __builtin_memcpy(ptr, &ref, sizeof(EcsEntity));
    } break;
    case ECS_FIELD_STR_ID: {
        u32 sid = 0u;
        if (val->type == JSON_STRING) {
            sid = ecs_str_intern(w, val->as.str);
        } else if (val->type == JSON_NUMBER) {
            sid = (u32)json_int(val, 0);
        } else {
            sio_add_warn(res, "str_id tip uyusmazligi", fd->name);
            return;
        }
        __builtin_memcpy(ptr, &sid, sizeof(u32));
    } break;
    }
}

b32 scene_load(EcsWorld* w, const char* json, u32 len, Arena* scratch, SceneIoResult* res) {
    if (res) __builtin_memset(res, 0, sizeof(*res));
    if (!w || !json || !scratch) {
        sio_set_err(res, "gecersiz arguman");
        return FALSE;
    }

    usize mark = arena_mark(scratch);
    JsonValue root;
    JsonError jerr;
    if (!json_parse(json, len, scratch, &root, &jerr)) {
        sio_set_err(res, jerr.msg[0] ? jerr.msg : "JSON ayristirma hatasi");
        arena_reset(scratch, mark);
        return FALSE;
    }

    if (root.type != JSON_OBJECT) {
        sio_set_err(res, "kok JSON nesnesi bekleniyordu");
        arena_reset(scratch, mark);
        return FALSE;
    }

    const JsonValue* ents = json_get(&root, "entities");
    if (!ents || ents->type != JSON_ARRAY) {
        sio_set_err(res, "entities dizisi bulunamadi");
        arena_reset(scratch, mark);
        return FALSE;
    }

    u32 ent_count = ents->count;
    if (ent_count > w->ent_cap) {
        sio_set_err(res, "entity sayisi dunya kapasitesini asiyor");
        arena_reset(scratch, mark);
        return FALSE;
    }

    /* Dünyayı silmeden önce yapıyı doğrula: hata durumunda mevcut dünya bozulmaz */
    for (u32 i = 0u; i < ent_count; i++) {
        if (ents->as.items[i].type != JSON_OBJECT) {
            sio_set_err(res, "entity ogesi nesne degil");
            arena_reset(scratch, mark);
            return FALSE;
        }
    }

    ecs_world_clear(w);

    i64*       saved_ids = ARENA_ARRAY(scratch, i64, ent_count ? ent_count : 1u);
    EcsEntity* new_ents  = ARENA_ARRAY(scratch, EcsEntity, ent_count ? ent_count : 1u);

    /* 1. Geçiş: Tüm entity'leri oluştur ve eski ID -> yeni EcsEntity eşlemesini kaydet */
    for (u32 i = 0u; i < ent_count; i++) {
        const JsonValue* eobj = &ents->as.items[i];
        saved_ids[i] = json_int(json_get(eobj, "id"), (i64)i);
        new_ents[i]  = ecs_spawn(w);
    }

    /* 2. Geçiş: Bileşenleri ve alanları yükle, entity referanslarını yeniden eşle */
    for (u32 i = 0u; i < ent_count; i++) {
        const JsonValue* eobj  = &ents->as.items[i];
        const JsonValue* comps = json_get(eobj, "components");
        if (!comps) continue;
        if (comps->type != JSON_OBJECT) {
            sio_add_warn(res, "components nesne degil", NULL);
            continue;
        }

        EcsEntity e = new_ents[i];
        for (u32 cm = 0u; cm < comps->count; cm++) {
            const JsonMember* comp_mem = &comps->as.members[cm];
            EcsCompId cid = ecs_find_comp(w, comp_mem->key);
            if (cid == ECS_COMP_NIL) {
                sio_add_warn(res, "bilinmeyen bilesen", comp_mem->key);
                continue;
            }
            if (comp_mem->value.type != JSON_OBJECT) {
                sio_add_warn(res, "bilesen degeri nesne degil", comp_mem->key);
                continue;
            }

            const EcsStore* s = &w->stores[cid];
            u8* cdata;
            if (cid == w->transform_cid && s->size == sizeof(Transform)) {
                Transform def_t = transform_identity();
                cdata = (u8*)ecs_set(w, e, cid, &def_t);
            } else {
                cdata = (u8*)ecs_add(w, e, cid);
            }
            if (!cdata) continue;

            const JsonValue* cobj = &comp_mem->value;
            for (u32 fm = 0u; fm < cobj->count; fm++) {
                const JsonMember* field_mem = &cobj->as.members[fm];
                const EcsFieldDesc* fd = NULL;
                for (u32 f = 0u; f < s->field_count; f++) {
                    if (sio_streq(s->fields[f].name, field_mem->key)) {
                        fd = &s->fields[f];
                        break;
                    }
                }
                if (!fd) {
                    sio_add_warn(res, "bilinmeyen alan", field_mem->key);
                    continue;
                }
                sio_deserialize_field(w, fd, cdata, &field_mem->value,
                                      ent_count, saved_ids, new_ents, res);
            }
        }
    }

    if (w->transform_cid != ECS_COMP_NIL) {
        transform_update(w);
    }

    if (res) res->entities_loaded = ent_count;
    arena_reset(scratch, mark);
    return TRUE;
}
