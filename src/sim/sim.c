/* sim/sim.c — Deterministik simülasyon döngüsü, durum anlık görüntüsü (snapshot), rollback ve replay.
 * Durum anlık görüntüleri tek bir Arena bölgesinin düz memcpy'si olarak halka tamponda saklanır.
 * Gecikmeli gerçek girdi tahminden farklı çıktığında en eski kirli kareye geri sarılıp yeniden simüle edilir.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/sim/sim.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "sim/sim.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"

#define HASH_PRIME1 0x9E3779B185EBCA87ULL
#define HASH_PRIME2 0xC2B2AE3D27D4EB4FULL
#define HASH_PRIME3 0x165667B19E3779F9ULL
#define HASH_PRIME4 0x85EBCA77C2B2AE63ULL
#define HASH_PRIME5 0x27D4EB2F165667C5ULL

static inline u64 rotl64(u64 x, u32 r) {
    return (x << r) | (x >> (64u - r));
}

u64 sim_hash_init(void) {
    return SIM_HASH_SEED;
}

u64 sim_hash_update(u64 h, const void* data, usize len) {
    if (!data || len == 0u) return h;
    const u8* p = (const u8*)data;
    const u8* end = p + len;

    while (p + 8 <= end) {
        u64 k = 0ULL;
        __builtin_memcpy(&k, p, sizeof(u64));
        k *= HASH_PRIME2;
        k = rotl64(k, 31u);
        k *= HASH_PRIME1;
        h ^= k;
        h = rotl64(h, 27u) * HASH_PRIME1 + HASH_PRIME4;
        p += 8;
    }

    if (p + 4 <= end) {
        u32 k32 = 0u;
        __builtin_memcpy(&k32, p, sizeof(u32));
        h ^= (u64)k32 * HASH_PRIME1;
        h = rotl64(h, 23u) * HASH_PRIME2 + HASH_PRIME3;
        p += 4;
    }

    while (p < end) {
        h ^= (u64)(*p) * HASH_PRIME5;
        h = rotl64(h, 11u) * HASH_PRIME1;
        p++;
    }

    return h;
}

u64 sim_hash_final(u64 h) {
    h ^= h >> 33u;
    h *= HASH_PRIME2;
    h ^= h >> 29u;
    h *= HASH_PRIME3;
    h ^= h >> 32u;
    return h;
}

u64 sim_hash(const void* data, usize len) {
    return sim_hash_final(sim_hash_update(sim_hash_init(), data, len));
}

u64 sim_hash_u32(u64 h, u32 v) {
    return sim_hash_update(h, &v, sizeof(u32));
}

u64 sim_hash_f32(u64 h, f32 v) {
    u32 bits = 0u;
    __builtin_memcpy(&bits, &v, sizeof(u32));
    /* -0.0f ve +0.0f aynı değere kanonikleştirilir; tüm NaN bit kalıpları tek kanonik qNaN'a indirgenir */
    if ((bits & 0x7FFFFFFFu) == 0u) {
        bits = 0u;
    } else if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u) {
        bits = 0x7FC00000u;
    }
    return sim_hash_u32(h, bits);
}

u64 sim_hash_vec3(u64 h, Vec3 v) {
    h = sim_hash_f32(h, v.x);
    h = sim_hash_f32(h, v.y);
    h = sim_hash_f32(h, v.z);
    return h;
}

u64 sim_hash_ecs(u64 h, const EcsWorld* w) {
    if (!w) return h;
    h = sim_hash_u32(h, w->ent_cap);
    h = sim_hash_u32(h, w->ent_high_water);
    h = sim_hash_u32(h, w->alive_count);
    h = sim_hash_u32(h, w->free_count);
    h = sim_hash_u32(h, w->comp_count);

    if (w->ent_high_water > 0u) {
        h = sim_hash_update(h, w->ent_handle, sizeof(EcsEntity) * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_gen,    sizeof(u16)       * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_mask,   sizeof(u32)       * (usize)w->ent_high_water);
        h = sim_hash_update(h, w->ent_flags,  sizeof(u8)        * (usize)w->ent_high_water);
    }
    if (w->free_count > 0u) {
        h = sim_hash_update(h, w->free_list, sizeof(u32) * (usize)w->free_count);
    }

    for (u32 c = 0u; c < w->comp_count; c++) {
        const EcsStore* s = &w->stores[c];
        h = sim_hash_u32(h, s->size);
        h = sim_hash_u32(h, s->stride);
        h = sim_hash_u32(h, s->count);
        if (s->count > 0u) {
            h = sim_hash_update(h, s->dense_ent, sizeof(EcsEntity) * (usize)s->count);
            if (s->stride > 0u) {
                h = sim_hash_update(h, s->data, (usize)s->count * (usize)s->stride);
            }
        }
    }

    return h;
}

static void sim_clear_rings(Sim* sim) {
    for (u32 p = 0u; p < SIM_MAX_PLAYERS; p++) {
        for (u32 i = 0u; i < SIM_INPUT_RING; i++) {
            sim->input_ring[p][i].frame     = U32_MAX;
            sim->input_ring[p][i].confirmed = FALSE;
            sim->input_ring[p][i].input     = sim_input_zero();
        }
    }
    for (u32 i = 0u; i < SIM_INPUT_RING; i++) {
        sim->frame_hashes[i]    = 0ULL;
        sim->frame_hash_nums[i] = U32_MAX;
    }
    for (u32 i = 0u; i < sim->snapshot_cap; i++) {
        sim->snap_ring[i].frame      = U32_MAX;
        sim->snap_ring[i].arena_used = 0u;
    }
}

void sim_init(Sim* sim, Arena* a, const SimConfig* cfg) {
    ASSERT(sim != NULL);
    ASSERT(a != NULL);
    ASSERT(cfg != NULL);
    ASSERT(cfg->state_size > 0u);

    __builtin_memset(sim, 0, sizeof(*sim));

    u32 player_count = (cfg->player_count == 0u) ? 2u : cfg->player_count;
    ASSERT(player_count <= SIM_MAX_PLAYERS);

    u32 snap_cap = (cfg->snapshot_cap == 0u) ? SIM_DEFAULT_SNAPSHOT_RING : cfg->snapshot_cap;
    ASSERT(snap_cap >= 2u && snap_cap <= SIM_MAX_SNAPSHOT_RING && IS_POW2(snap_cap));

    u32 aligned_state = ALIGN_UP(cfg->state_size, 16u);

    sim->state          = (u8*)arena_alloc(a, aligned_state, 16u);
    sim->state_size     = aligned_state;
    arena_init(&sim->state_arena, sim->state, aligned_state);

    sim->player_count   = player_count;
    sim->fixed_dt       = (cfg->fixed_dt > 0.0f) ? cfg->fixed_dt : SIM_DEFAULT_DT;
    sim->accum          = 0.0f;
    sim->current_frame  = 0u;
    sim->rollback_frame = U32_MAX;

    sim->step_fn        = cfg->step_fn;
    sim->hash_fn        = cfg->hash_fn;
    sim->user           = cfg->user;

    sim->snapshot_cap   = snap_cap;
    sim->snapshot_mask  = snap_cap - 1u;
    sim->snap_ring      = ARENA_ARRAY(a, SimSnapshotSlot, snap_cap);
    sim->snap_pool      = (u8*)arena_alloc(a, (usize)snap_cap * (usize)aligned_state, 16u);

    for (u32 i = 0u; i < snap_cap; i++) {
        sim->snap_ring[i].frame      = U32_MAX;
        sim->snap_ring[i].arena_used = 0u;
        sim->snap_ring[i].data       = sim->snap_pool + (usize)i * (usize)aligned_state;
    }

    sim_clear_rings(sim);
}

void sim_reset(Sim* sim) {
    if (!sim) return;
    __builtin_memset(sim->state, 0, sim->state_size);
    arena_reset(&sim->state_arena, 0);
    sim->accum          = 0.0f;
    sim->current_frame  = 0u;
    sim->rollback_frame = U32_MAX;
    __builtin_memset(&sim->stats, 0, sizeof(sim->stats));
    sim_clear_rings(sim);
}

void* sim_state(Sim* sim) {
    return sim ? sim->state : NULL;
}

const void* sim_state_const(const Sim* sim) {
    return sim ? sim->state : NULL;
}

Arena* sim_state_arena(Sim* sim) {
    return sim ? &sim->state_arena : NULL;
}

SimOffset sim_state_alloc(Sim* sim, u32 size, u32 align) {
    if (!sim || size == 0u) return SIM_NULL_OFFSET;
    if (align == 0u) align = 4u;
    void* ptr = arena_alloc(&sim->state_arena, size, align);
    return sim_ptr_offset(sim->state, ptr);
}

void sim_save_snapshot(Sim* sim, u32 frame) {
    if (!sim) return;
    u32 idx = frame & sim->snapshot_mask;
    SimSnapshotSlot* slot = &sim->snap_ring[idx];
    slot->frame      = frame;
    slot->arena_used = sim->state_arena.used;
    /* Yalnız durum arenasının kullanılan kısmı (bizim ek, faz 5): arenanın ötesi hiç yazılmamış ya da geri yüklemeden
     * sonra yeniden ayrılınca sıfırlanır. Arena hiç kullanılmadıysa (durum doğrudan yazılıyor) tüm bölge. */
    __builtin_memcpy(slot->data, sim->state, slot->arena_used ? slot->arena_used : sim->state_size);
}

b32 sim_restore_snapshot(Sim* sim, u32 frame) {
    if (!sim) return FALSE;
    u32 idx = frame & sim->snapshot_mask;
    const SimSnapshotSlot* slot = &sim->snap_ring[idx];
    if (slot->frame != frame) return FALSE;

    __builtin_memcpy(sim->state, slot->data, slot->arena_used ? slot->arena_used : sim->state_size);
    sim->state_arena.used = slot->arena_used;
    sim->current_frame    = frame;
    return TRUE;
}

b32 sim_submit_input(Sim* sim, u32 player, u32 frame, SimInput input) {
    if (!sim || player >= sim->player_count) return FALSE;

    if (frame < sim->current_frame) {
        u32 age = sim->current_frame - frame;
        if (age > sim->snapshot_cap) {
            return FALSE;
        }
        SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
        b32 changed = (slot->frame != frame) || !sim_input_eq(slot->input, input);
        slot->frame     = frame;
        slot->confirmed = TRUE;
        slot->input     = input;

        if (changed) {
            sim->stats.mispredicted_inputs++;
            if (frame < sim->rollback_frame) {
                sim->rollback_frame = frame;
            }
        }
        return TRUE;
    }

    SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
    slot->frame     = frame;
    slot->confirmed = TRUE;
    slot->input     = input;
    return TRUE;
}

SimInput sim_get_input(const Sim* sim, u32 player, u32 frame, b32* out_confirmed) {
    if (!sim || player >= sim->player_count) {
        if (out_confirmed) *out_confirmed = FALSE;
        return sim_input_zero();
    }

    const SimInputSlot* slot = &sim->input_ring[player][frame & (SIM_INPUT_RING - 1u)];
    if (slot->frame == frame && slot->confirmed) {
        if (out_confirmed) *out_confirmed = TRUE;
        return slot->input;
    }

    if (out_confirmed) *out_confirmed = FALSE;

    /* Geriye doğru bilinen en son girdiyi (tahmin veya gerçek) bul ve tekrarla */
    for (u32 back = 1u; back < SIM_INPUT_RING && back <= frame; back++) {
        u32 pf = frame - back;
        const SimInputSlot* prev = &sim->input_ring[player][pf & (SIM_INPUT_RING - 1u)];
        if (prev->frame == pf) {
            return prev->input;
        }
    }
    return sim_input_zero();
}

static void sim_step_single(Sim* sim, b32 is_resim) {
    u32 f = sim->current_frame;

    /* Kare başındaki durumu anlık görüntü halkasına kopyala */
    sim_save_snapshot(sim, f);

    SimInput frame_inputs[SIM_MAX_PLAYERS];
    for (u32 p = 0u; p < sim->player_count; p++) {
        SimInputSlot* slot = &sim->input_ring[p][f & (SIM_INPUT_RING - 1u)];
        if (slot->frame == f && slot->confirmed) {
            frame_inputs[p] = slot->input;
        } else {
            /* Tahmini girdi: bir önceki karenin girdisini tekrarla (f == 0 ise sıfır girdi) */
            SimInput pred = sim_input_zero();
            if (f > 0u) {
                const SimInputSlot* prev = &sim->input_ring[p][(f - 1u) & (SIM_INPUT_RING - 1u)];
                if (prev->frame == f - 1u) {
                    pred = prev->input;
                }
            }
            slot->frame     = f;
            slot->confirmed = FALSE;
            slot->input     = pred;
            frame_inputs[p] = pred;
            if (!is_resim) {
                sim->stats.predicted_inputs++;
            }
        }
    }

    if (sim->replay_rec) {
        sim_replay_record(sim->replay_rec, f, frame_inputs, sim->player_count);
    }

    if (sim->step_fn) {
        sim->step_fn(sim->state, frame_inputs, sim->player_count, sim->fixed_dt, sim->user);
    }

    sim->current_frame = f + 1u;

    u64 h = sim_state_hash(sim);
    u32 h_idx = f & (SIM_INPUT_RING - 1u);
    sim->frame_hashes[h_idx]    = h;
    sim->frame_hash_nums[h_idx] = f;
}

u32 sim_resolve_rollback(Sim* sim) {
    if (!sim || sim->rollback_frame == U32_MAX) return 0u;

    u32 target    = sim->rollback_frame;
    u32 end_frame = sim->current_frame;
    sim->rollback_frame = U32_MAX;

    if (target >= end_frame) return 0u;
    if (!sim_restore_snapshot(sim, target)) return 0u;

    u32 depth = end_frame - target;
    for (u32 f = target; f < end_frame; f++) {
        sim_step_single(sim, TRUE);
    }

    sim->stats.rollback_count++;
    sim->stats.resim_frames += depth;
    if (depth > sim->stats.max_rollback_depth) {
        sim->stats.max_rollback_depth = depth;
    }
    return depth;
}

b32 sim_rollback_to(Sim* sim, u32 target_frame) {
    if (!sim || target_frame >= sim->current_frame) return FALSE;
    if (sim->rollback_frame == U32_MAX || target_frame < sim->rollback_frame) {
        sim->rollback_frame = target_frame;
    }
    return sim_resolve_rollback(sim) > 0u;
}

void sim_step(Sim* sim) {
    if (!sim) return;
    if (sim->rollback_frame != U32_MAX) {
        sim_resolve_rollback(sim);
    }
    sim_step_single(sim, FALSE);
    sim->stats.total_frames++;
}

u32 sim_update(Sim* sim, f32 dt) {
    if (!sim || dt <= 0.0f) return 0u;
    sim->accum += dt;
    u32 steps = 0u;
    while (sim->accum >= sim->fixed_dt && steps < SIM_MAX_FRAME_STEPS) {
        sim_step(sim);
        sim->accum -= sim->fixed_dt;
        steps++;
    }
    if (sim->accum >= sim->fixed_dt) {
        sim->accum = 0.0f;
    }
    return steps;
}

u64 sim_state_hash(const Sim* sim) {
    if (!sim) return 0ULL;
    if (sim->hash_fn) {
        return sim->hash_fn(sim->state, sim->state_size, sim->user);
    }
    return sim_hash(sim->state, sim->state_size);
}

u64 sim_frame_hash(const Sim* sim, u32 frame) {
    if (!sim) return 0ULL;
    u32 idx = frame & (SIM_INPUT_RING - 1u);
    if (sim->frame_hash_nums[idx] != frame) return 0ULL;
    return sim->frame_hashes[idx];
}

b32 sim_check_desync(const Sim* sim, u32 frame, u64 remote_hash) {
    if (!sim) return FALSE;
    u32 idx = frame & (SIM_INPUT_RING - 1u);
    if (sim->frame_hash_nums[idx] != frame) return FALSE;
    return sim->frame_hashes[idx] != remote_hash;
}

/* ----------------------------------------------------------------------------
 * Replay (Girdi Kaydı ve Oynatma)
 * ---------------------------------------------------------------------------- */

void sim_replay_init(SimReplay* rep, Arena* a, u32 player_count,
                     u32 max_frames, u32 seed, f32 fixed_dt) {
    ASSERT(rep != NULL);
    ASSERT(a != NULL);
    if (player_count == 0u) player_count = 2u;
    ASSERT(player_count <= SIM_MAX_PLAYERS);

    __builtin_memset(rep, 0, sizeof(*rep));
    rep->seed         = seed;
    rep->player_count = player_count;
    rep->frame_count  = 0u;
    rep->max_frames   = max_frames;
    rep->fixed_dt     = (fixed_dt > 0.0f) ? fixed_dt : SIM_DEFAULT_DT;
    if (max_frames > 0u) {
        rep->inputs = ARENA_ARRAY(a, SimInput, (usize)max_frames * (usize)player_count);
    }
}

void sim_replay_clear(SimReplay* rep) {
    if (!rep) return;
    if (rep->inputs && rep->frame_count > 0u) {
        __builtin_memset(rep->inputs, 0,
                         sizeof(SimInput) * (usize)rep->frame_count * (usize)rep->player_count);
    }
    rep->frame_count = 0u;
}

void sim_attach_replay(Sim* sim, SimReplay* rep) {
    if (!sim) return;
    sim->replay_rec = rep;
}

b32 sim_replay_record(SimReplay* rep, u32 frame, const SimInput* inputs, u32 player_count) {
    if (!rep || !inputs || frame >= rep->max_frames) return FALSE;
    u32 count = MIN(player_count, rep->player_count);
    SimInput* dst = rep->inputs + (usize)frame * (usize)rep->player_count;
    for (u32 p = 0u; p < count; p++) {
        dst[p] = inputs[p];
    }
    for (u32 p = count; p < rep->player_count; p++) {
        dst[p] = sim_input_zero();
    }
    if (frame + 1u > rep->frame_count) {
        rep->frame_count = frame + 1u;
    }
    return TRUE;
}

const SimInput* sim_replay_get(const SimReplay* rep, u32 frame) {
    if (!rep || !rep->inputs || frame >= rep->frame_count) return NULL;
    return rep->inputs + (usize)frame * (usize)rep->player_count;
}

typedef struct {
    u32 magic;
    u32 version;
    u32 seed;
    u32 player_count;
    u32 frame_count;
    u32 fixed_dt_bits;
} SimReplayHeader;

_Static_assert(sizeof(SimReplayHeader) == 24, "SimReplayHeader 24 byte olmali");

u32 sim_replay_save(const SimReplay* rep, u8* buf, u32 cap) {
    if (!rep) return 0u;
    usize payload_bytes = sizeof(SimInput) * (usize)rep->frame_count * (usize)rep->player_count;
    usize total_bytes   = sizeof(SimReplayHeader) + payload_bytes;
    if (!buf || (usize)cap < total_bytes) return 0u;

    SimReplayHeader hdr;
    hdr.magic        = SIM_REPLAY_MAGIC;
    hdr.version      = SIM_REPLAY_VERSION;
    hdr.seed         = rep->seed;
    hdr.player_count = rep->player_count;
    hdr.frame_count  = rep->frame_count;
    __builtin_memcpy(&hdr.fixed_dt_bits, &rep->fixed_dt, sizeof(u32));

    __builtin_memcpy(buf, &hdr, sizeof(SimReplayHeader));
    if (payload_bytes > 0u) {
        __builtin_memcpy(buf + sizeof(SimReplayHeader), rep->inputs, payload_bytes);
    }
    return (u32)total_bytes;
}

b32 sim_replay_load(SimReplay* rep, Arena* a, const u8* buf, u32 len) {
    if (!rep || !a || !buf || len < sizeof(SimReplayHeader)) return FALSE;

    SimReplayHeader hdr;
    __builtin_memcpy(&hdr, buf, sizeof(SimReplayHeader));
    if (hdr.magic != SIM_REPLAY_MAGIC || hdr.version != SIM_REPLAY_VERSION) return FALSE;
    if (hdr.player_count == 0u || hdr.player_count > SIM_MAX_PLAYERS) return FALSE;

    usize payload_bytes = sizeof(SimInput) * (usize)hdr.frame_count * (usize)hdr.player_count;
    if ((usize)len < sizeof(SimReplayHeader) + payload_bytes) return FALSE;

    f32 fixed_dt = SIM_DEFAULT_DT;
    __builtin_memcpy(&fixed_dt, &hdr.fixed_dt_bits, sizeof(f32));

    sim_replay_init(rep, a, hdr.player_count, hdr.frame_count, hdr.seed, fixed_dt);
    rep->frame_count = hdr.frame_count;
    if (payload_bytes > 0u) {
        __builtin_memcpy(rep->inputs, buf + sizeof(SimReplayHeader), payload_bytes);
    }
    return TRUE;
}

b32 sim_replay_run(Sim* sim, const SimReplay* rep, u64* out_hashes, u32 max_hashes) {
    if (!sim || !rep || rep->player_count != sim->player_count) return FALSE;

    SimReplay* prev_rec = sim->replay_rec;
    sim->replay_rec = NULL;

    for (u32 f = 0u; f < rep->frame_count; f++) {
        const SimInput* frame_in = sim_replay_get(rep, f);
        if (!frame_in) {
            sim->replay_rec = prev_rec;
            return FALSE;
        }
        for (u32 p = 0u; p < rep->player_count; p++) {
            sim_submit_input(sim, p, sim->current_frame, frame_in[p]);
        }
        sim_step(sim);
        if (out_hashes && f < max_hashes) {
            out_hashes[f] = sim_state_hash(sim);
        }
    }

    sim->replay_rec = prev_rec;
    return TRUE;
}
