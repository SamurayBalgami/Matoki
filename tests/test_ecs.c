/* tests/test_ecs.c — ECS çekirdeği, hiyerarşik Transform, JSON kaydet/yükle ve 100k benchmark testleri.
 * Nesil doğrulaması, 1M rastgele işlem referans model testi, sorgu içi ertelenmiş silme,
 * 3 seviyeli dönüşüm matrisi doğruluğu (±1e-5), döngü reddi ve JSON gidiş-dönüş testlerini içerir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_ecs.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "test.h"
#include "core/sys.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/embed.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/fmt.h"
#include "scene/ecs.h"
#include "scene/transform.h"
#include "scene/scene_io.h"


static b32 t_streq(const char* a, const char* b) {
    usize n = cstr_len(a);
    return n == cstr_len(b) && mem_equal(a, b, n);
}

static u8* g_ecs_mem;   /* MB(64), her TEST başında test_arena'dan */

typedef struct {
    f32 x, y, z;
} CompPos;

typedef struct {
    f32 vx, vy, vz;
} CompVel;

typedef struct {
    i32 hp;
    u32 flags;
} CompStats;

/* 1. Nesil (generation) ve yaşam döngüsü testi */
TEST(ecs_generation_and_lifecycle) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(4));

    EcsWorld w;
    ecs_world_init(&w, &a, 128);
    EcsCompId c_pos = ecs_register(&w, "Pos", sizeof(CompPos), _Alignof(CompPos), NULL);

    CHECK(!ecs_alive(&w, ECS_NULL));
    CHECK(ecs_alive_count(&w) == 0u);

    EcsEntity e1 = ecs_spawn(&w);
    CHECK(e1 != ECS_NULL);
    CHECK(ecs_alive(&w, e1));
    CHECK(ecs_alive_count(&w) == 1u);
    CHECK(ecs_entity_index(e1) == 0u);
    CHECK(ecs_entity_gen(e1) == 1u);

    CompPos p1 = { 1.0f, 2.0f, 3.0f };
    ecs_set(&w, e1, c_pos, &p1);
    CHECK(ecs_has(&w, e1, c_pos));

    /* Sil ve yeniden oluştur: aynı indeks, yeni nesil; eski tutamak geçersiz olmalı */
    ecs_destroy(&w, e1);
    CHECK(!ecs_alive(&w, e1));
    CHECK(!ecs_has(&w, e1, c_pos));
    CHECK(ecs_get(&w, e1, c_pos) == NULL);
    CHECK(ecs_alive_count(&w) == 0u);

    EcsEntity e2 = ecs_spawn(&w);
    CHECK(ecs_alive(&w, e2));
    CHECK(!ecs_alive(&w, e1));
    CHECK(ecs_entity_index(e2) == ecs_entity_index(e1));
    CHECK(ecs_entity_gen(e2) == 2u);
    CHECK(!ecs_has(&w, e2, c_pos));

    /* Eski tutamak üzerinden silme veya bileşen ekleme yeni entity'yi etkilememeli */
    ecs_destroy(&w, e1);
    CHECK(ecs_alive(&w, e2));
    CHECK(ecs_set(&w, e1, c_pos, &p1) == NULL);
    CHECK(!ecs_remove(&w, e1, c_pos));

    /* 12-bit nesil taşması (4096 döngü) sonrasında nesil 0 olmamalı (ECS_NULL ile çakışmamalı) */
    EcsEntity cur = e2;
    for (u32 i = 0u; i < 4100u; i++) {
        EcsEntity old = cur;
        ecs_destroy(&w, old);
        CHECK(!ecs_alive(&w, old));
        cur = ecs_spawn(&w);
        CHECK(ecs_alive(&w, cur));
        CHECK(!ecs_alive(&w, old));
        CHECK(ecs_entity_gen(cur) != 0u);
    }
}

/* 2. 1M rastgele ekle/sil/bileşen işlemi — dizi tabanlı referans model ile her adımda doğrulama */
#define REF_SLOTS 256u

typedef struct {
    b32       alive;
    EcsEntity handle;
    b32       has_pos;
    b32       has_vel;
    b32       has_stats;
    CompPos   pos;
    CompVel   vel;
    CompStats stats;
} RefSlot;

static void verify_slot_against_ref(TestCtx* t_, EcsWorld* w, const RefSlot* s,
                                    EcsCompId c_pos, EcsCompId c_vel, EcsCompId c_stats) {
    if (!s->alive) {
        if (s->handle != ECS_NULL) {
            CHECK(!ecs_alive(w, s->handle));
            CHECK(!ecs_has(w, s->handle, c_pos));
            CHECK(ecs_get(w, s->handle, c_pos) == NULL);
        }
        return;
    }

    CHECK(ecs_alive(w, s->handle));
    CHECK(ecs_has(w, s->handle, c_pos) == s->has_pos);
    CHECK(ecs_has(w, s->handle, c_vel) == s->has_vel);
    CHECK(ecs_has(w, s->handle, c_stats) == s->has_stats);

    if (s->has_pos) {
        const CompPos* p = (const CompPos*)ecs_get_const(w, s->handle, c_pos);
        CHECK(p != NULL);
        if (p) {
            CHECK(p->x == s->pos.x && p->y == s->pos.y && p->z == s->pos.z);
        }
    } else {
        CHECK(ecs_get_const(w, s->handle, c_pos) == NULL);
    }

    if (s->has_vel) {
        const CompVel* v = (const CompVel*)ecs_get_const(w, s->handle, c_vel);
        CHECK(v != NULL);
        if (v) {
            CHECK(v->vx == s->vel.vx && v->vy == s->vel.vy && v->vz == s->vel.vz);
        }
    } else {
        CHECK(ecs_get_const(w, s->handle, c_vel) == NULL);
    }

    if (s->has_stats) {
        const CompStats* st = (const CompStats*)ecs_get_const(w, s->handle, c_stats);
        CHECK(st != NULL);
        if (st) {
            CHECK(st->hp == s->stats.hp && st->flags == s->stats.flags);
        }
    } else {
        CHECK(ecs_get_const(w, s->handle, c_stats) == NULL);
    }
}

static void verify_query_against_ref(TestCtx* t_, EcsWorld* w, const RefSlot* ref, u32 mask,
                                     EcsCompId c_pos, EcsCompId c_vel, EcsCompId c_stats) {
    u8 seen[REF_SLOTS];
    mem_set(seen, (u8)(0), sizeof(seen));

    u32 q_count = 0u;
    EcsQuery q = ecs_query(w, mask);
    while (ecs_query_next(&q)) {
        EcsEntity e = q.entity;
        u32 idx = ecs_entity_index(e);
        CHECK(idx < REF_SLOTS);
        CHECK(!seen[idx]);
        seen[idx] = 1u;
        CHECK(ref[idx].alive);
        CHECK(ref[idx].handle == e);
        q_count++;
    }

    u32 expected = 0u;
    for (u32 i = 0u; i < REF_SLOTS; i++) {
        if (!ref[i].alive) continue;
        u32 rmask = 0u;
        if (ref[i].has_pos)   rmask |= (1u << c_pos);
        if (ref[i].has_vel)   rmask |= (1u << c_vel);
        if (ref[i].has_stats) rmask |= (1u << c_stats);
        b32 should_match = ((rmask & mask) == mask);
        CHECK((seen[i] != 0u) == should_match);
        if (should_match) expected++;
    }
    CHECK(q_count == expected);
}

TEST(ecs_random_1m_vs_reference_model) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(8));

    EcsWorld w;
    ecs_world_init(&w, &a, REF_SLOTS);
    EcsCompId c_pos   = ecs_register(&w, "Pos",   sizeof(CompPos),   _Alignof(CompPos),   NULL);
    EcsCompId c_vel   = ecs_register(&w, "Vel",   sizeof(CompVel),   _Alignof(CompVel),   NULL);
    EcsCompId c_stats = ecs_register(&w, "Stats", sizeof(CompStats), _Alignof(CompStats), NULL);

    RefSlot ref[REF_SLOTS];
    mem_set(ref, (u8)(0), sizeof(ref));
    u32 ref_alive = 0u;

    EcsEntity stale_ring[64];
    mem_set(stale_ring, (u8)(0), sizeof(stale_ring));
    u32 stale_pos = 0u;

    Rng rng;
    (*(&rng) = rng_make(0xC0FFEE1234ULL));

    for (u32 step = 0u; step < 1000000u; step++) {
        u32 op  = rng_u32(&rng) % 4u;
        u32 idx = rng_u32(&rng) % REF_SLOTS;

        if (op == 0u) {
            /* Yeni entity oluştur (kapasite doluysa rastgele birini sil) */
            if (ref_alive < REF_SLOTS) {
                EcsEntity e = ecs_spawn(&w);
                u32 e_idx = ecs_entity_index(e);
                CHECK(e_idx < REF_SLOTS);
                CHECK(!ref[e_idx].alive);
                ref[e_idx].alive     = TRUE;
                ref[e_idx].handle    = e;
                ref[e_idx].has_pos   = FALSE;
                ref[e_idx].has_vel   = FALSE;
                ref[e_idx].has_stats = FALSE;
                ref_alive++;
                verify_slot_against_ref(t_, &w, &ref[e_idx], c_pos, c_vel, c_stats);
            } else if (ref[idx].alive) {
                stale_ring[stale_pos++ & 63u] = ref[idx].handle;
                ecs_destroy(&w, ref[idx].handle);
                ref[idx].alive     = FALSE;
                ref[idx].has_pos   = FALSE;
                ref[idx].has_vel   = FALSE;
                ref[idx].has_stats = FALSE;
                ref_alive--;
                verify_slot_against_ref(t_, &w, &ref[idx], c_pos, c_vel, c_stats);
            }
        } else if (op == 1u) {
            /* Entity sil */
            if (ref[idx].alive) {
                EcsEntity old_h = ref[idx].handle;
                stale_ring[stale_pos++ & 63u] = old_h;
                ecs_destroy(&w, old_h);
                CHECK(!ecs_alive(&w, old_h));
                ref[idx].alive     = FALSE;
                ref[idx].has_pos   = FALSE;
                ref[idx].has_vel   = FALSE;
                ref[idx].has_stats = FALSE;
                ref_alive--;
            } else {
                ecs_destroy(&w, ref[idx].handle);
            }
            verify_slot_against_ref(t_, &w, &ref[idx], c_pos, c_vel, c_stats);
        } else if (op == 2u) {
            /* Bileşen ekle / güncelle */
            u32 which = rng_u32(&rng) % 3u;
            if (ref[idx].alive) {
                if (which == 0u) {
                    CompPos p = { (f32)step, (f32)idx, -1.5f };
                    CHECK(ecs_set(&w, ref[idx].handle, c_pos, &p) != NULL);
                    ref[idx].has_pos = TRUE;
                    ref[idx].pos     = p;
                } else if (which == 1u) {
                    CompVel v = { 0.5f, (f32)step * 0.25f, 1.0f };
                    CHECK(ecs_set(&w, ref[idx].handle, c_vel, &v) != NULL);
                    ref[idx].has_vel = TRUE;
                    ref[idx].vel     = v;
                } else {
                    CompStats st = { (i32)step, rng_u32(&rng) };
                    CHECK(ecs_set(&w, ref[idx].handle, c_stats, &st) != NULL);
                    ref[idx].has_stats = TRUE;
                    ref[idx].stats     = st;
                }
            } else {
                CompPos p = { 0.0f, 0.0f, 0.0f };
                CHECK(ecs_set(&w, ref[idx].handle, c_pos, &p) == NULL);
            }
            verify_slot_against_ref(t_, &w, &ref[idx], c_pos, c_vel, c_stats);
        } else {
            /* Bileşen sil */
            u32 which = rng_u32(&rng) % 3u;
            if (ref[idx].alive) {
                if (which == 0u) {
                    b32 r = ecs_remove(&w, ref[idx].handle, c_pos);
                    CHECK(r == ref[idx].has_pos);
                    ref[idx].has_pos = FALSE;
                } else if (which == 1u) {
                    b32 r = ecs_remove(&w, ref[idx].handle, c_vel);
                    CHECK(r == ref[idx].has_vel);
                    ref[idx].has_vel = FALSE;
                } else {
                    b32 r = ecs_remove(&w, ref[idx].handle, c_stats);
                    CHECK(r == ref[idx].has_stats);
                    ref[idx].has_stats = FALSE;
                }
            } else {
                CHECK(!ecs_remove(&w, ref[idx].handle, c_pos));
            }
            verify_slot_against_ref(t_, &w, &ref[idx], c_pos, c_vel, c_stats);
        }

        CHECK(ecs_alive_count(&w) == ref_alive);
        verify_slot_against_ref(t_, &w, &ref[step & (REF_SLOTS - 1u)], c_pos, c_vel, c_stats);

        /* Periyodik tam küme ve sorgu doğrulaması */
        if ((step + 1u) % 50000u == 0u) {
            for (u32 i = 0u; i < REF_SLOTS; i++) {
                verify_slot_against_ref(t_, &w, &ref[i], c_pos, c_vel, c_stats);
            }
            verify_query_against_ref(t_, &w, ref, (1u << c_pos), c_pos, c_vel, c_stats);
            verify_query_against_ref(t_, &w, ref, (1u << c_pos) | (1u << c_vel), c_pos, c_vel, c_stats);
            verify_query_against_ref(t_, &w, ref, (1u << c_pos) | (1u << c_vel) | (1u << c_stats),
                                     c_pos, c_vel, c_stats);
        }
    }
}

/* 3. Sorgu en küçük depoyu sürmesi ve yineleme sırasında güvenli silme (komut tamponu) */
TEST(ecs_query_and_deferred_deletion) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(4));

    EcsWorld w;
    ecs_world_init(&w, &a, 512);
    EcsCompId c_pos   = ecs_register(&w, "Pos",   sizeof(CompPos),   _Alignof(CompPos),   NULL);
    EcsCompId c_vel   = ecs_register(&w, "Vel",   sizeof(CompVel),   _Alignof(CompVel),   NULL);
    EcsCompId c_stats = ecs_register(&w, "Stats", sizeof(CompStats), _Alignof(CompStats), NULL);

    EcsEntity ents[100];
    for (u32 i = 0u; i < 100u; i++) {
        ents[i] = ecs_spawn(&w);
        CompPos p = { (f32)i, 0.0f, 0.0f };
        ecs_set(&w, ents[i], c_pos, &p);
        if (i % 2u == 0u) {
            CompVel v = { 1.0f, 0.0f, 0.0f };
            ecs_set(&w, ents[i], c_vel, &v);
        }
        if (i % 10u == 0u) {
            CompStats st = { (i32)i, i };
            ecs_set(&w, ents[i], c_stats, &st);
        }
    }

    /* En küçük depo c_stats (10 eleman) sürücü olarak seçilmeli */
    u32 mask_all = (1u << c_pos) | (1u << c_vel) | (1u << c_stats);
    EcsQuery q = ecs_query(&w, mask_all);
    CHECK(q.driver == &w.stores[c_stats]);

    u32 visited = 0u;
    while (ecs_query_next(&q)) {
        visited++;
        const CompStats* st = (const CompStats*)ecs_get_const(&w, q.entity, c_stats);
        CHECK(st != NULL);
        /* Yineleme sırasında çift indeksli (20, 40, ...) olanları sil, diğerlerinden c_vel kaldır */
        if (st && (st->hp % 20 == 0)) {
            ecs_destroy(&w, q.entity);
            /* Ertelenmiş silme: sorgu bitene kadar entity hala bellekte, ancak sorgu döngüsü bozulmaz */
        } else {
            ecs_remove(&w, q.entity, c_vel);
        }
    }
    /* 10 elemanın tamamı hiçbir atlama olmadan ziyaret edilmiş olmalı */
    CHECK(visited == 10u);
    CHECK(w.iter_depth == 0u);
    CHECK(w.cmd_count == 0u);

    /* Sorgu bittiğinde komut tamponu otomatik boşaltılmış olmalı */
    for (u32 i = 0u; i < 100u; i += 10u) {
        if (i % 20u == 0u) {
            CHECK(!ecs_alive(&w, ents[i]));
        } else {
            CHECK(ecs_alive(&w, ents[i]));
            CHECK(!ecs_has(&w, ents[i], c_vel));
            CHECK(ecs_has(&w, ents[i], c_pos));
        }
    }
}

/* 4. Hiyerarşi: 3 seviyeli dünya matrisi (±1e-5), ebeveyn silme politikası ve döngü reddi */
TEST(ecs_hierarchy_transform_and_cycles) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(4));

    EcsWorld w;
    ecs_world_init(&w, &a, 128);
    transform_register(&w);
    EcsCompId c_tr = transform_comp_id(&w);

    /* Bilerek ters sırada (torun -> çocuk -> kök) oluşturup ekliyoruz ki yoğun dizi sırasından
     * bağımsız olarak doğru ata-çocuk sırasıyla dünya matrisinin hesaplandığı kanıtlansın. */
    EcsEntity e_grand = ecs_spawn(&w);
    EcsEntity e_child = ecs_spawn(&w);
    EcsEntity e_root  = ecs_spawn(&w);
    EcsEntity e_other = ecs_spawn(&w);

    Transform t_root  = transform_trs(v3(10.0f, -2.0f, 3.5f),
                                      quat_from_axis_angle(v3(0.0f, 1.0f, 0.0f), M_PI_F * 0.25f),
                                      v3(2.0f, 2.0f, 2.0f));
    Transform t_child = transform_trs(v3(4.0f, 1.0f, -1.0f),
                                      quat_from_axis_angle(v3(0.0f, 0.0f, 1.0f), M_PI_F / 3.0f),
                                      v3(1.0f, 0.5f, 1.5f));
    Transform t_grand = transform_trs(v3(-1.0f, 3.0f, 2.0f),
                                      quat_from_axis_angle(v3(1.0f, 0.0f, 0.0f), M_PI_F / 6.0f),
                                      v3(0.5f, 3.0f, 1.0f));
    Transform t_other = transform_identity();

    ecs_set(&w, e_grand, c_tr, &t_grand);
    ecs_set(&w, e_child, c_tr, &t_child);
    ecs_set(&w, e_root,  c_tr, &t_root);
    ecs_set(&w, e_other, c_tr, &t_other);

    CHECK(transform_set_parent(&w, e_child, e_root));
    CHECK(transform_set_parent(&w, e_grand, e_child));

    /* Döngü oluşturma girişimleri reddedilmeli */
    CHECK(!transform_set_parent(&w, e_root, e_root));
    CHECK(!transform_set_parent(&w, e_root, e_child));
    CHECK(!transform_set_parent(&w, e_root, e_grand));
    CHECK(!transform_set_parent(&w, e_child, e_grand));
    CHECK(transform_get_parent(&w, e_root) == ECS_NULL);
    CHECK(transform_get_parent(&w, e_child) == e_root);
    CHECK(transform_get_parent(&w, e_grand) == e_child);

    transform_update(&w);

    /* Elle hesaplanan yerel ve dünya matrisleri */
    Mat4 m_root_exp  = transform_local_mat4(&t_root);
    Mat4 m_child_exp = mat4_mul(m_root_exp, transform_local_mat4(&t_child));
    Mat4 m_grand_exp = mat4_mul(m_child_exp, transform_local_mat4(&t_grand));

    const Transform* tr_root  = (const Transform*)ecs_get_const(&w, e_root, c_tr);
    const Transform* tr_child = (const Transform*)ecs_get_const(&w, e_child, c_tr);
    const Transform* tr_grand = (const Transform*)ecs_get_const(&w, e_grand, c_tr);
    CHECK(tr_root && tr_child && tr_grand);

    for (u32 i = 0u; i < 16u; i++) {
        CHECK_NEAR(tr_root->world.m[i],  m_root_exp.m[i],  1e-5);
        CHECK_NEAR(tr_child->world.m[i], m_child_exp.m[i], 1e-5);
        CHECK_NEAR(tr_grand->world.m[i], m_grand_exp.m[i], 1e-5);
    }

    /* Bir noktanın 3 seviyeli zincir boyunca adım adım (ölçek -> döndür -> ötele) elle dönüşümü */
    Vec3 pt = v3(1.25f, -0.75f, 2.5f);
    Vec3 p1 = v3(pt.x * t_grand.scale.x, pt.y * t_grand.scale.y, pt.z * t_grand.scale.z);
    p1 = v3_add(quat_rotate(t_grand.rot, p1), t_grand.pos);

    Vec3 p2 = v3(p1.x * t_child.scale.x, p1.y * t_child.scale.y, p1.z * t_child.scale.z);
    p2 = v3_add(quat_rotate(t_child.rot, p2), t_child.pos);

    Vec3 p3 = v3(p2.x * t_root.scale.x, p2.y * t_root.scale.y, p2.z * t_root.scale.z);
    p3 = v3_add(quat_rotate(t_root.rot, p3), t_root.pos);

    Vec3 world_pt = transform_world_point(tr_grand, pt);
    CHECK_NEAR(world_pt.x, p3.x, 1e-5);
    CHECK_NEAR(world_pt.y, p3.y, 1e-5);
    CHECK_NEAR(world_pt.z, p3.z, 1e-5);

    /* Ebeveyn silinince çocuk ve torun da birlikte silinmeli; bağımsız entity hayatta kalmalı */
    ecs_destroy(&w, e_root);
    CHECK(!ecs_alive(&w, e_root));
    CHECK(!ecs_alive(&w, e_child));
    CHECK(!ecs_alive(&w, e_grand));
    CHECK(ecs_alive(&w, e_other));
    CHECK(ecs_alive_count(&w) == 1u);
}

/* 5. Kaydet -> Yükle -> Kaydet: iki JSON birebir aynı, entity referansları yeniden eşlenir */
typedef struct {
    f32       weight;
    i32       score;
    u32       mask;
    b32       active;
    Vec3      offset;
    Vec4      tint;
    Quat      orient;
    EcsEntity target;
    u32       tag_str;
} RichComp;

static void register_rich_comp(EcsWorld* w, EcsCompId* out_cid) {
    static const EcsFieldDesc fields[] = {
        { "weight",  ECS_FIELD_F32,    ECS_OFFSET(RichComp, weight)  },
        { "score",   ECS_FIELD_I32,    ECS_OFFSET(RichComp, score)   },
        { "mask",    ECS_FIELD_U32,    ECS_OFFSET(RichComp, mask)    },
        { "active",  ECS_FIELD_B32,    ECS_OFFSET(RichComp, active)  },
        { "offset",  ECS_FIELD_VEC3,   ECS_OFFSET(RichComp, offset)  },
        { "tint",    ECS_FIELD_VEC4,   ECS_OFFSET(RichComp, tint)    },
        { "orient",  ECS_FIELD_QUAT,   ECS_OFFSET(RichComp, orient)  },
        { "target",  ECS_FIELD_ENTITY, ECS_OFFSET(RichComp, target)  },
        { "tag_str", ECS_FIELD_STR_ID, ECS_OFFSET(RichComp, tag_str) }
    };
    static const EcsCompDesc desc = { fields, ARRAY_COUNT(fields) };
    *out_cid = ecs_register(w, "RichComp", sizeof(RichComp), _Alignof(RichComp), &desc);
}

TEST(ecs_scene_save_load_roundtrip) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(16));
    Arena a1 = arena_sub(&a, MB(2));
    Arena a2 = arena_sub(&a, MB(2));
    Arena scratch = arena_sub(&a, MB(4));

    EcsWorld w1;
    ecs_world_init(&w1, &a1, 128);
    transform_register(&w1);
    EcsCompId c_rich1 = 0u;
    register_rich_comp(&w1, &c_rich1);

    /* Arada silinmiş yuvalar oluştur ki entity indeksleri seyrek ve nesilleri > 1 olsun */
    EcsEntity hole0 = ecs_spawn(&w1);
    EcsEntity e_a   = ecs_spawn(&w1);
    EcsEntity hole1 = ecs_spawn(&w1);
    EcsEntity e_b   = ecs_spawn(&w1);
    EcsEntity e_c   = ecs_spawn(&w1);
    ecs_destroy(&w1, hole0);
    ecs_destroy(&w1, hole1);

    Transform ta = transform_trs(v3(1.5f, 2.25f, -3.0f),
                                 quat_from_axis_angle(v3(0.0f, 1.0f, 0.0f), 0.7f),
                                 v3(1.0f, 1.0f, 1.0f));
    Transform tb = transform_trs(v3(-4.0f, 0.5f, 8.0f),
                                 quat_from_axis_angle(v3(1.0f, 0.0f, 0.0f), -0.4f),
                                 v3(2.0f, 0.5f, 1.0f));
    ecs_set(&w1, e_a, transform_comp_id(&w1), &ta);
    ecs_set(&w1, e_b, transform_comp_id(&w1), &tb);
    CHECK(transform_set_parent(&w1, e_b, e_a));
    transform_update(&w1);

    /* e_a -> e_c (ileri referans), e_b -> e_a (geri referans), e_c -> ECS_NULL */
    RichComp ra = {
        3.1415927f, -42, 0xCAFEBABEu, TRUE,
        v3(0.1f, -0.2f, 0.3f),
        v4(0.25f, 0.5f, 0.75f, 1.0f),
        quat_from_axis_angle(v3(0.0f, 0.0f, 1.0f), 1.2f),
        e_c,
        ecs_str_intern(&w1, "oyuncu_lider")
    };
    RichComp rb = {
        0.0f, 1000, 7u, FALSE,
        v3(10.0f, 20.0f, 30.0f),
        v4(1.0f, 0.0f, 0.0f, 0.5f),
        quat_identity(),
        e_a,
        ecs_str_intern(&w1, "takipci_birim")
    };
    RichComp rc = {
        -12.5f, 0, 0u, TRUE,
        v3(0.0f, 0.0f, 0.0f),
        v4(0.0f, 1.0f, 0.0f, 1.0f),
        quat_identity(),
        ECS_NULL,
        ecs_str_intern(&w1, "hedef_nokta")
    };
    ecs_set(&w1, e_a, c_rich1, &ra);
    ecs_set(&w1, e_b, c_rich1, &rb);
    ecs_set(&w1, e_c, c_rich1, &rc);

    static char json1[65536];
    static char json2[65536];
    u32 len1 = scene_save(&w1, json1, sizeof(json1), &scratch);
    CHECK(len1 > 0u && len1 < sizeof(json1));

    /* Yeni dünyaya yükle */
    EcsWorld w2;
    ecs_world_init(&w2, &a2, 128);
    transform_register(&w2);
    EcsCompId c_rich2 = 0u;
    register_rich_comp(&w2, &c_rich2);

    SceneIoResult res;
    CHECK(scene_load(&w2, json1, len1, &scratch, &res));
    CHECK(res.warnings == 0u);
    CHECK(res.entities_loaded == 3u);
    CHECK(ecs_alive_count(&w2) == 3u);

    /* w2'deki yeni entity tutamakları 0, 1, 2 indekslerinde oluşturulmuş olmalı */
    EcsEntity n_a = w2.ent_handle[0];
    EcsEntity n_b = w2.ent_handle[1];
    EcsEntity n_c = w2.ent_handle[2];
    CHECK(ecs_alive(&w2, n_a) && ecs_alive(&w2, n_b) && ecs_alive(&w2, n_c));
    /* w1'de e_a indeks 1 idi, w2'de n_a indeks 0 oldu -> tutamaklar yeniden eşlendi */
    CHECK(n_a != e_a);
    CHECK(transform_get_parent(&w2, n_b) == n_a);

    const RichComp* l_ra = (const RichComp*)ecs_get_const(&w2, n_a, c_rich2);
    const RichComp* l_rb = (const RichComp*)ecs_get_const(&w2, n_b, c_rich2);
    const RichComp* l_rc = (const RichComp*)ecs_get_const(&w2, n_c, c_rich2);
    CHECK(l_ra && l_rb && l_rc);
    if (l_ra && l_rb && l_rc) {
        CHECK(l_ra->target == n_c);
        CHECK(l_rb->target == n_a);
        CHECK(l_rc->target == ECS_NULL);
        CHECK(l_ra->score == -42 && l_ra->mask == 0xCAFEBABEu && l_ra->active == TRUE);
        CHECK(t_streq(ecs_str_get(&w2, l_ra->tag_str), "oyuncu_lider"));
        CHECK(t_streq(ecs_str_get(&w2, l_rb->tag_str), "takipci_birim"));
        CHECK(t_streq(ecs_str_get(&w2, l_rc->tag_str), "hedef_nokta"));
    }

    /* İkinci kez kaydet ve iki JSON'un birebir aynı olduğunu doğrula */
    u32 len2 = scene_save(&w2, json2, sizeof(json2), &scratch);
    CHECK(len1 == len2);
    CHECK(t_streq(json1, json2));

    /* Bilinmeyen bileşen ve bilinmeyen alan uyarı olarak atlanmalı */
    static const char* unknown_json =
        "{\"entities\":[{\"id\":0,\"components\":{"
        "\"BilinmeyenBilesen\":{\"x\":1},"
        "\"Transform\":{\"pos\":[5,6,7],\"bilinmeyen_alan\":999}"
        "}}]}";
    SceneIoResult ures;
    CHECK(scene_load(&w2, unknown_json, (u32)cstr_len(unknown_json), &scratch, &ures));
    CHECK(ures.entities_loaded == 1u);
    CHECK(ures.warnings == 2u);
    const Transform* ut = (const Transform*)ecs_get_const(&w2, w2.ent_handle[0], transform_comp_id(&w2));
    CHECK(ut != NULL);
    if (ut) {
        CHECK_NEAR(ut->pos.x, 5.0f, 1e-5);
        CHECK_NEAR(ut->pos.y, 6.0f, 1e-5);
        CHECK_NEAR(ut->pos.z, 7.0f, 1e-5);
    }
}

/* 6. Benchmark (host): 100k entity, 3 bileşen; tam sorgu yineleme ve 10k ekle/sil süresi */
TEST(ecs_benchmark_100k) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(64));

    const u32 N = 100000u;
    EcsWorld w;
    ecs_world_init(&w, &a, N);

    EcsCompId c_pos   = ecs_register(&w, "Pos",   sizeof(CompPos),   _Alignof(CompPos),   NULL);
    EcsCompId c_vel   = ecs_register(&w, "Vel",   sizeof(CompVel),   _Alignof(CompVel),   NULL);
    EcsCompId c_stats = ecs_register(&w, "Stats", sizeof(CompStats), _Alignof(CompStats), NULL);

    static EcsEntity ents[100000];
    for (u32 i = 0u; i < N; i++) {
        EcsEntity e = ecs_spawn(&w);
        ents[i] = e;
        CompPos   p  = { (f32)i, 1.0f, 2.0f };
        CompVel   v  = { 0.1f, 0.2f, 0.3f };
        CompStats st = { 100, i };
        ecs_set(&w, e, c_pos,   &p);
        ecs_set(&w, e, c_vel,   &v);
        ecs_set(&w, e, c_stats, &st);
    }

    u32 mask = (1u << c_pos) | (1u << c_vel) | (1u << c_stats);

    /* Isınma (warmup) */
    for (u32 it = 0u; it < 5u; it++) {
        EcsQuery q = ecs_query(&w, mask);
        while (ecs_query_next(&q)) {
            CompPos*   p  = (CompPos*)ecs_get(&w, q.entity, c_pos);
            CompVel*   v  = (CompVel*)ecs_get(&w, q.entity, c_vel);
            CompStats* st = (CompStats*)ecs_get(&w, q.entity, c_stats);
            p->x += v->vx;
            st->hp += 1;
        }
    }

    const u32 ITERS = 25u;
    f64 t0 = sys_now_ms();
    for (u32 it = 0u; it < ITERS; it++) {
        EcsQuery q = ecs_query(&w, mask);
        while (ecs_query_next(&q)) {
            CompPos*   p  = (CompPos*)ecs_query_get(&q, c_pos);
            CompVel*   v  = (CompVel*)ecs_query_get(&q, c_vel);
            CompStats* st = (CompStats*)ecs_query_get(&q, c_stats);
            p->x += v->vx;
            p->y += v->vy;
            p->z += v->vz;
            st->hp += 1;
        }
    }
    f64 t1 = sys_now_ms();
    f64 iter_ms     = (t1 - t0) / (f64)ITERS;
    f64 ns_per_ent  = (iter_ms * 1e6) / (f64)N;

    /* Ayrıca tam nesil/sınır denetimli ecs_get ile sorgu süresi */
    f64 tg0 = sys_now_ms();
    for (u32 it = 0u; it < ITERS; it++) {
        EcsQuery q = ecs_query(&w, mask);
        while (ecs_query_next(&q)) {
            CompPos*   p  = (CompPos*)ecs_get(&w, q.entity, c_pos);
            CompVel*   v  = (CompVel*)ecs_get(&w, q.entity, c_vel);
            CompStats* st = (CompStats*)ecs_get(&w, q.entity, c_stats);
            p->x += v->vx;
            p->y += v->vy;
            p->z += v->vz;
            st->hp += 1;
        }
    }
    f64 tg1 = sys_now_ms();
    f64 iter_get_ms = (tg1 - tg0) / (f64)ITERS;
    f64 ns_get_ent  = (iter_get_ms * 1e6) / (f64)N;

    /* 10k entity sil + yeniden oluştur + 3 bileşen ekle ölçümü */
    const u32 CHURN_N = 10000u;
    f64 tc0 = sys_now_ms();
    for (u32 i = 0u; i < CHURN_N; i++) {
        ecs_destroy(&w, ents[i]);
    }
    for (u32 i = 0u; i < CHURN_N; i++) {
        EcsEntity e = ecs_spawn(&w);
        ents[i] = e;
        CompPos   p  = { (f32)i, 1.0f, 2.0f };
        CompVel   v  = { 0.1f, 0.2f, 0.3f };
        CompStats st = { 100, i };
        ecs_set(&w, e, c_pos,   &p);
        ecs_set(&w, e, c_vel,   &v);
        ecs_set(&w, e, c_stats, &st);
    }
    f64 tc1 = sys_now_ms();
    f64 churn_ms = tc1 - tc0;

    /* 10k tekil bileşen sil + ekle ölçümü */
    f64 tr0 = sys_now_ms();
    for (u32 i = 0u; i < CHURN_N; i++) {
        ecs_remove(&w, ents[i], c_vel);
    }
    for (u32 i = 0u; i < CHURN_N; i++) {
        CompVel v = { 0.1f, 0.2f, 0.3f };
        ecs_set(&w, ents[i], c_vel, &v);
    }
    f64 tr1 = sys_now_ms();
    f64 comp_churn_ms = tr1 - tr0;

    LOG("ecs bench (100k entity, 3 bilesen): sorgu(query_get) %.3f ms (%.2f ns/entity), "
        "sorgu(ecs_get) %.3f ms (%.2f ns/entity), "
        "10k entity sil+ekle %.3f ms (%.1f ns/entity), 10k bilesen sil+ekle %.3f ms (%.1f ns/op)",
        iter_ms, ns_per_ent,
        iter_get_ms, ns_get_ent,
        churn_ms, (churn_ms * 1e6) / (f64)CHURN_N,
        comp_churn_ms, (comp_churn_ms * 1e6) / (f64)CHURN_N);

    CHECK(ecs_alive_count(&w) == N);
}

/* İnceleme: derin zincir özyinelemesiz, Transform'suz ara ebeveyn, bozuk yükleme dünyayı korur */
TEST(ecs_review_deep_chain_and_load_safety) {
    g_ecs_mem = (u8*)arena_alloc_nozero(test_arena(), MB(64), 16);
    Arena a;
    arena_init(&a, g_ecs_mem, MB(60));
    EcsWorld w;
    const u32 n = 100000u;   /* eski özyinelemeli sürüm burada yığını taşırırdı (~100k çerçeve) */
    ecs_world_init(&w, &a, n + 8u);
    transform_register(&w);
    EcsCompId c_tr = transform_comp_id(&w);

    /* 100k derinlikli zincir: her düğüm +1 x ötelemesi → son düğüm x = n.
     * Bağlama aşağıdan yukarı (e[i] → e[i-1], i azalarak): transform_set_parent döngü denetimi için ebeveynin ata
     * zincirini yürür; yukarıdan aşağı kurmak her adımda i ata → Σi = 5·10⁹ adım (wasm'da 22 s). Aşağıdan yukarıda
     * ebeveyn henüz köktür → adım başına O(1); ortaya çıkan ağaç ve kontrol sayısı aynı. */
    EcsEntity* chain = ARENA_ARRAY(test_arena(), EcsEntity, n);
    for (u32 i = 0; i < n; i++) {
        chain[i] = ecs_spawn(&w);
        Transform t = transform_trs(v3(1.0f, 0.0f, 0.0f), quat_identity(), v3(1.0f, 1.0f, 1.0f));
        ecs_set(&w, chain[i], c_tr, &t);
    }
    for (u32 i = n - 1u; i > 0u; i--) CHECK(transform_set_parent(&w, chain[i], chain[i - 1u]));
    EcsEntity last = chain[n - 1u];
    transform_update(&w);
    const Transform* tl = (const Transform*)ecs_get(&w, last, c_tr);
    CHECK_NEAR(tl->world.m[12], (f64)n, 1e-1);

    /* Transform'suz ara ebeveyn: zincir orada kopar; sonra Transform eklenince hesaba girer */
    ecs_world_clear(&w);
    EcsEntity r = ecs_spawn(&w), mid = ecs_spawn(&w), c = ecs_spawn(&w);
    Transform t2 = transform_trs(v3(2.0f, 0.0f, 0.0f), quat_identity(), v3(1.0f, 1.0f, 1.0f));
    ecs_set(&w, r, c_tr, &t2);
    ecs_set(&w, c, c_tr, &t2);
    CHECK(transform_set_parent(&w, mid, r));
    CHECK(transform_set_parent(&w, c, mid));
    transform_update(&w);
    CHECK_NEAR(((const Transform*)ecs_get(&w, c, c_tr))->world.m[12], 2.0, 1e-6);
    ecs_set(&w, mid, c_tr, &t2);
    transform_update(&w);
    CHECK_NEAR(((const Transform*)ecs_get(&w, c, c_tr))->world.m[12], 6.0, 1e-6);

    /* Bozuk yükleme: mevcut dünya değişmeden kalır */
    u32 alive = ecs_alive_count(&w);
    SceneIoResult res;
    const char* bad = "{\"entities\":[{\"id\":0,\"components\":{}}, 5]}";
    CHECK(!scene_load(&w, bad, (u32)cstr_len(bad), &a, &res));
    CHECK(res.err[0] != 0);
    CHECK(ecs_alive_count(&w) == alive && ecs_alive(&w, c));
}
