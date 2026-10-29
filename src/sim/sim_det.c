/* sim/sim_det.c — sim çekirdeğinin bizim fizik + ECS ile belirlenimcilik sahnesi ve fizik durum özeti (M13 faz 5).
 * Onların F0_G sahnesi kendi fizik API'lerine bağlıydı; burada aynı amaç (10 000 tick, girdi, ECS, temaslar) bizim
 * PhysWorld'le kurulur. PhysWorld'ün tüm dizileri durum arenasından ayrılır: anlık görüntü düz kopya, geri yükleme
 * aynı adrese yapıldığı için iç işaretçiler geçerli kalır. Özet işaretçi baytlarını değil değerleri karıştırır. */
#include "sim/sim.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"

#define DET_CRATES     12u
#define DET_SPHERES    4u
#define DET_MAX_BODIES 48u
#define DET_MAX_MANIF  256u
#define DET_STATE_SIZE (1024u * 1024u)
#define DET_MOVE_FORCE 60.0f    /* N, tam eksen */
#define DET_JUMP       9.0f     /* N·s */
#define DET_KICK       6.0f     /* N·s */
#define DET_COOLDOWN   20u      /* tick */

typedef struct DetPlayer {
    u32 body;
    u32 kicks;
    u32 cooldown;
    u32 crate_cursor;
} DetPlayer;

typedef struct DetState {
    u32       tick;
    PhysWorld phys;
    EcsWorld  ecs;
    EcsCompId cid_player;
    EcsEntity player[SIM_DET_PLAYERS];
    u32       crate[DET_CRATES];
} DetState;

u64 sim_hash_phys(u64 h, const PhysWorld* w) {
    if (!w) return h;
    h = sim_hash_vec3(h, w->gravity);
    h = sim_hash_u32(h, w->count);
    for (u32 i = 0; i < w->count; i++) {
        const Body* b = &w->bodies[i];
        h = sim_hash_u32(h, (u32)b->shape);
        h = sim_hash_u32(h, b->flags);
        h = sim_hash_vec3(h, b->pos);
        h = sim_hash_f32(h, b->rot.x);
        h = sim_hash_f32(h, b->rot.y);
        h = sim_hash_f32(h, b->rot.z);
        h = sim_hash_f32(h, b->rot.w);
        h = sim_hash_vec3(h, b->vel);
        h = sim_hash_vec3(h, b->ang_vel);
        h = sim_hash_f32(h, b->sleep_time);
    }
    /* temas önbelleği (warm start) sonraki adımları belirler → özete girer */
    h = sim_hash_u32(h, w->cur_count);
    for (u32 m = 0; m < w->cur_count; m++) {
        const Manifold* mf = &w->cur[m];
        h = sim_hash_u32(h, mf->a);
        h = sim_hash_u32(h, mf->b);
        h = sim_hash_u32(h, mf->sub);
        h = sim_hash_u32(h, mf->count);
        for (u32 k = 0; k < mf->count; k++) {
            h = sim_hash_f32(h, mf->p[k].jn);
            h = sim_hash_f32(h, mf->p[k].jt1);
            h = sim_hash_f32(h, mf->p[k].jt2);
            h = sim_hash_vec3(h, mf->p[k].local_a);
        }
    }
    return h;
}

static u64 det_hash(const void* state, u32 state_size, void* user) {
    (void)state_size;
    (void)user;
    const DetState* s = (const DetState*)state;
    u64 h = sim_hash_init();
    h = sim_hash_u32(h, s->tick);
    h = sim_hash_phys(h, &s->phys);
    h = sim_hash_ecs(h, &s->ecs);
    return sim_hash_final(h);
}

static void det_step(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    (void)user;
    DetState* s = (DetState*)state;
    PhysWorld* w = &s->phys;
    for (u32 p = 0; p < player_count && p < SIM_DET_PLAYERS; p++) {
        DetPlayer* pl = (DetPlayer*)ecs_get(&s->ecs, s->player[p], s->cid_player);
        if (!pl) continue;
        Body* b = &w->bodies[pl->body];
        const SimInput* in = &inputs[p];
        Vec3 f = v3((f32)in->move_x * (DET_MOVE_FORCE / 32767.0f), 0.0f, -(f32)in->move_y * (DET_MOVE_FORCE / 32767.0f));
        if (f.x != 0.0f || f.z != 0.0f) phys_apply_force_at(w, pl->body, f, b->pos);
        if (pl->cooldown) pl->cooldown--;
        if ((in->buttons & SIM_BTN_JUMP) && pl->cooldown == 0 && m_absf(b->vel.y) < 0.2f) {
            phys_apply_impulse_at(w, pl->body, v3(0.0f, DET_JUMP, 0.0f), b->pos);
            pl->cooldown = DET_COOLDOWN;
        }
        if ((in->buttons & SIM_BTN_FIRE) && pl->cooldown == 0) {
            /* sırayla bir sandığa, oyuncudan sandığa doğru yatay itki */
            u32 c = s->crate[pl->crate_cursor % DET_CRATES];
            pl->crate_cursor++;
            Vec3 d = v3_sub(w->bodies[c].pos, b->pos);
            d.y = 0.0f;
            f32 len = v3_len(d);
            if (len > 1e-3f) {
                phys_apply_impulse_at(w, c, v3_add(v3_scale(d, DET_KICK / len), v3(0.0f, 1.5f, 0.0f)), w->bodies[c].pos);
                pl->kicks++;
            }
            pl->cooldown = DET_COOLDOWN;
        }
    }
    phys_step(w, dt);
    s->tick++;
}

u32 sim_det_state_size(void) { return DET_STATE_SIZE; }

void sim_det_config(SimConfig* cfg) {
    mem_zero(cfg, sizeof(*cfg));
    cfg->state_size = DET_STATE_SIZE;
    cfg->player_count = SIM_DET_PLAYERS;
    cfg->snapshot_cap = 16u;
    cfg->fixed_dt = SIM_DEFAULT_DT;
    cfg->step_fn = det_step;
    cfg->hash_fn = det_hash;
}

void sim_det_setup(Sim* sim, u32 seed) {
    sim_reset(sim);
    Arena* a = sim_state_arena(sim);
    SimOffset off = sim_state_alloc(sim, sizeof(DetState), 16u);
    DetState* s = SIM_PTR(sim_state(sim), DetState, off);
    phys_world_init(&s->phys, a, DET_MAX_BODIES, DET_MAX_MANIF);
    PhysWorld* w = &s->phys;
    BodyDesc d = phys_desc_plane(v3(0.0f, 1.0f, 0.0f), 0.0f);
    phys_add_body(w, &d);
    /* çevre duvarları (statik kutular) */
    const Vec3 wall_pos[4] = { v3(0, 1, -12), v3(0, 1, 12), v3(-12, 1, 0), v3(12, 1, 0) };
    const Vec3 wall_half[4] = { v3(12, 1, 0.5f), v3(12, 1, 0.5f), v3(0.5f, 1, 12), v3(0.5f, 1, 12) };
    for (u32 i = 0; i < 4; i++) {
        d = phys_desc_box(wall_pos[i], wall_half[i], 0.0f);
        phys_add_body(w, &d);
    }
    ecs_world_init(&s->ecs, a, 16u);
    s->cid_player = ecs_register(&s->ecs, "DetPlayer", sizeof(DetPlayer), _Alignof(DetPlayer), NULL);
    for (u32 p = 0; p < SIM_DET_PLAYERS; p++) {
        d = phys_desc_box(v3(p ? 4.0f : -4.0f, 0.5f, 3.0f), v3(0.5f, 0.5f, 0.5f), 2.0f);
        DetPlayer pl = { phys_add_body(w, &d), 0, 0, p * 5u };
        w->bodies[pl.body].flags |= BODY_NO_SLEEP;
        s->player[p] = ecs_spawn(&s->ecs);
        ecs_set(&s->ecs, s->player[p], s->cid_player, &pl);
    }
    /* sandık yığınları: 3 sütun × 4 kat; tohum sütun konumunu kaydırır (farklı tohum → farklı oyun) */
    Rng r = rng_make((u64)seed * 0x9E3779B97F4A7C15ull + 1u);
    for (u32 i = 0; i < DET_CRATES; i++) {
        u32 col = i / 4u, lvl = i % 4u;
        f32 x = -3.0f + 3.0f * (f32)col + rng_range(&r, -0.3f, 0.3f);
        d = phys_desc_box(v3(x, 0.5f + 1.02f * (f32)lvl, -2.0f), v3(0.5f, 0.5f, 0.5f), 1.0f);
        s->crate[i] = phys_add_body(w, &d);
    }
    for (u32 i = 0; i < DET_SPHERES; i++) {
        d = phys_desc_sphere(v3(rng_range(&r, -6.0f, 6.0f), 2.0f + (f32)i, rng_range(&r, 4.0f, 8.0f)), 0.4f, 0.8f);
        phys_add_body(w, &d);
    }
    s->tick = 0;
}

/* Girdi betiği: 24 tick'lik dilimlerde tohumdan yön; arada zıplama ve atış */
SimInput sim_det_input(u32 player, u32 frame, u32 seed) {
    u32 slice = frame / 24u;
    u32 h = (slice * 0x9E3779B1u) ^ (player * 0x85EBCA77u) ^ seed;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    SimInput in = sim_input_zero();
    in.move_x = (i16)((i32)(h & 0xFFFFu) - 32768 + ((h & 0xFFFFu) == 0u));
    in.move_y = (i16)((i32)((h >> 16) & 0xFFFFu) - 32768 + (((h >> 16) & 0xFFFFu) == 0u));
    if ((frame + player * 7u) % 90u == 0u) in.buttons |= SIM_BTN_JUMP;
    if ((frame + player * 13u) % 120u == 60u) in.buttons |= SIM_BTN_FIRE;
    return in;
}

static b32 f_bad(f32 x) { return !(x == x) || m_absf(x) > 1e30f; }

b32 sim_det_has_nan(const Sim* sim) {
    const DetState* s = (const DetState*)sim_state_const(sim);
    for (u32 i = 0; i < s->phys.count; i++) {
        const Body* b = &s->phys.bodies[i];
        if (f_bad(b->pos.x) || f_bad(b->pos.y) || f_bad(b->pos.z) || f_bad(b->vel.x) || f_bad(b->vel.y) ||
            f_bad(b->vel.z) || f_bad(b->rot.w)) return TRUE;
    }
    return FALSE;
}

static u64 fnv_combine(u64 h, u64 v) {
    for (u32 i = 0; i < 8; i++) {
        h ^= (v >> (i * 8u)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

SimDetResult sim_det_run(Arena* a, u32 total_ticks, u32 seed, u64* out_hashes, u32 max_hashes) {
    SimDetResult r;
    mem_zero(&r, sizeof(r));
    SimConfig cfg;
    sim_det_config(&cfg);
    Sim* sim = ARENA_NEW(a, Sim);
    sim_init(sim, a, &cfg);
    sim_det_setup(sim, seed);
    r.total_ticks = total_ticks;
    r.initial_hash = sim_state_hash(sim);
    r.combined_hash = 0xCBF29CE484222325ull;
    u32 every = total_ticks >= 10u ? total_ticks / 10u : 1u;
    for (u32 t = 0; t < total_ticks; t++) {
        for (u32 p = 0; p < SIM_DET_PLAYERS; p++) sim_submit_input(sim, p, sim->current_frame, sim_det_input(p, t, seed));
        sim_step(sim);
        u64 h = sim_frame_hash(sim, t);
        r.combined_hash = fnv_combine(r.combined_hash, h);
        if (out_hashes && t < max_hashes) out_hashes[t] = h;
        if ((t + 1u) % every == 0u && (t + 1u) / every <= 10u) r.checkpoint_hashes[(t + 1u) / every - 1u] = h;
    }
    r.final_hash = sim_state_hash(sim);
    r.has_nan = sim_det_has_nan(sim);
    return r;
}
