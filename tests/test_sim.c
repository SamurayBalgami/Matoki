/* tests/test_sim.c — Deterministik simülasyon çekirdeği, geri sarma ve kayıt (M13 faz 5).
 * 1–2: onların durum özeti / ofset modeli / anlık görüntü halkası testleri (aynen). 3–5: onların gecikmeli eş, 10 000
 * kare kayıt ve 8 kare geri sarma testleri, onların fizik sahnesi yerine bizim fizik + ECS sahnesiyle (sim_det.c).
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (tests/test_sim.c), izinle uyarlandı; adlar tools/port_names.py ile
 * çevrildi, statik 48 MB tampon → test_arena, host stdio → log_printf. */
#include "test.h"
#include "core/arena.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "core/sys.h"
#include "sim/sim.h"

/* ============================================================================
 * 1. Hash, Kanonik Float (-0.0f / NaN) ve Ofset Modeli Testleri
 * ============================================================================ */

TEST(sim_hash_canonical_and_desync) {
    u8 buf_a[64];
    u8 buf_b[64];
    for (u32 i = 0u; i < 64u; i++) {
        buf_a[i] = (u8)(i * 7u + 3u);
        buf_b[i] = buf_a[i];
    }

    u64 ha = sim_hash(buf_a, sizeof(buf_a));
    u64 hb = sim_hash(buf_b, sizeof(buf_b));
    CHECK(ha != 0ULL);
    CHECK(ha == hb);

    /* Tek bitlik değişim farklı özet üretmeli (çığ etkisi) */
    buf_b[31] ^= 1u;
    u64 hb_diff = sim_hash(buf_b, sizeof(buf_b));
    CHECK(ha != hb_diff);

    /* -0.0f ve +0.0f aynı kanonik özeti üretmeli */
    u64 h_pz = sim_hash_final(sim_hash_f32(sim_hash_init(), 0.0f));
    u64 h_nz = sim_hash_final(sim_hash_f32(sim_hash_init(), -0.0f));
    CHECK(h_pz == h_nz);

    /* Farklı NaN bit kalıpları aynı kanonik özeti üretmeli */
    u32 nan1_bits = 0x7FC00001u;
    u32 nan2_bits = 0xFFC00123u;
    f32 nan1 = 0.0f, nan2 = 0.0f;
    mem_copy(&nan1, &nan1_bits, sizeof(f32));
    mem_copy(&nan2, &nan2_bits, sizeof(f32));
    u64 h_nan1 = sim_hash_final(sim_hash_f32(sim_hash_init(), nan1));
    u64 h_nan2 = sim_hash_final(sim_hash_f32(sim_hash_init(), nan2));
    CHECK(h_nan1 == h_nan2);
    /* (onların PhysShape birliği denetimi: bizim fizikte birlik yok, özet değerlerden → çıkarıldı) */
}

/* Saf ofset/indeks kullanan durum yapısı (hiç ham işaretçi içermez -> doğrudan state baytları özetlenir) */
typedef struct {
    i32 x, y;
    i32 vx, vy;
    u32 target_idx; /* işaretçi yerine dizi indeksi */
    u32 score;
} OffsetPawn;

typedef struct {
    u32       tick;
    u32       pawn_count;
    SimOffset pawns_off; /* işaretçi yerine durum tabanından bayt ofseti */
} OffsetGameState;

static void offset_game_init(TestCtx* t_, Sim* sim, u32 count) {
    SimOffset hdr_off = sim_state_alloc(sim, sizeof(OffsetGameState), _Alignof(OffsetGameState));
    CHECK(hdr_off == 0u);
    SimOffset arr_off = sim_state_alloc(sim, sizeof(OffsetPawn) * count, _Alignof(OffsetPawn));

    OffsetGameState* gs = SIM_PTR(sim_state(sim), OffsetGameState, hdr_off);
    gs->tick       = 0u;
    gs->pawn_count = count;
    gs->pawns_off  = arr_off;

    OffsetPawn* pawns = SIM_PTR(sim_state(sim), OffsetPawn, gs->pawns_off);
    for (u32 i = 0u; i < count; i++) {
        pawns[i].x          = (i32)(i * 100u);
        pawns[i].y          = (i32)(i * 50u);
        pawns[i].vx         = 0;
        pawns[i].vy         = 0;
        pawns[i].target_idx = (i + 1u) % count;
        pawns[i].score      = 0u;
    }
}

static void offset_game_step(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user) {
    (void)dt;
    (void)user;
    OffsetGameState* gs = (OffsetGameState*)state;
    OffsetPawn* pawns = SIM_PTR(state, OffsetPawn, gs->pawns_off);

    for (u32 i = 0u; i < gs->pawn_count; i++) {
        const SimInput* in = &inputs[i % player_count];
        pawns[i].vx = (pawns[i].vx * 3) / 4 + (i32)in->move_x;
        pawns[i].vy = (pawns[i].vy * 3) / 4 + (i32)in->move_y;
        if (in->buttons & SIM_BTN_JUMP) {
            pawns[i].vy += 20;
        }
        pawns[i].x += pawns[i].vx;
        pawns[i].y += pawns[i].vy;

        /* Hedef indeksi üzerinden etkileşim (işaretçi yok) */
        u32 tid = pawns[i].target_idx;
        i64 dx = (i64)pawns[tid].x - (i64)pawns[i].x;
        i64 dy = (i64)pawns[tid].y - (i64)pawns[i].y;
        if (dx * dx + dy * dy < 10000LL) {
            pawns[i].score += 1u + (in->buttons & 7u);
        }
    }
    gs->tick++;
}

TEST(sim_offset_state_and_snapshot_ring) {
    Arena* a = test_arena();

    SimConfig cfg;
    mem_zero(&cfg, sizeof(cfg));
    cfg.state_size   = 512u;
    cfg.player_count = 2u;
    cfg.snapshot_cap = 16u;
    cfg.fixed_dt     = SIM_DEFAULT_DT;
    cfg.step_fn      = offset_game_step;
    cfg.hash_fn      = NULL; /* ham state bölgesi doğrudan özetlenir */

    Sim* sim_a = ARENA_NEW(a, Sim);
    Sim* sim_b = ARENA_NEW(a, Sim);
    sim_init(sim_a, a, &cfg);
    sim_init(sim_b, a, &cfg);
    CHECK(sim_state(sim_a) != sim_state(sim_b));

    offset_game_init(t_, sim_a, 4u);
    offset_game_init(t_, sim_b, 4u);

    /* Farklı bellek adreslerindeki iki eşin başlangıç ham bellek özeti birebir aynı olmalı */
    CHECK(sim_state_hash(sim_a) == sim_state_hash(sim_b));

    /* 20 kare ilerlet: 16 karelik halka tamponun sarma (wrap-around) davranışını doğrula */
    for (u32 f = 0u; f < 20u; f++) {
        SimInput in0 = { (f & 1u) ? SIM_BTN_JUMP : 0u, (i16)(f * 3u), -5, 0, 0, f };
        SimInput in1 = { (f & 2u) ? SIM_BTN_FIRE : 0u, -7, (i16)(f * 2u), 0, 0, 0u };
        sim_submit_input(sim_a, 0u, f, in0);
        sim_submit_input(sim_a, 1u, f, in1);
        sim_step(sim_a);
    }

    /* Kare 20'deyken kare 4..19 halkada mevcut, kare 0..3 üzerine yazılmış olmalı */
    CHECK(!sim_restore_snapshot(sim_a, 3u));
    u64 hash_after_12 = sim_frame_hash(sim_a, 12u);
    u64 hash_after_19 = sim_frame_hash(sim_a, 19u);
    CHECK(hash_after_12 != 0ULL && hash_after_19 != 0ULL);

    /* Kare 12'ye geri sar + yeniden simüle et -> kare 19 sonu özeti birebir korunmalı */
    CHECK(sim_rollback_to(sim_a, 12u));
    CHECK(sim_a->current_frame == 20u);
    CHECK(sim_frame_hash(sim_a, 12u) == hash_after_12);
    CHECK(sim_state_hash(sim_a) == hash_after_19);
    CHECK(!sim_check_desync(sim_a, 19u, hash_after_19));
    CHECK(sim_check_desync(sim_a, 19u, hash_after_19 ^ 1ULL));
}

/* ============================================================================
 * 2. Sahne: bizim fizik + ECS (sim_det.c). Onların battle_scene_* karşılığı.
 * ============================================================================ */

#define TEST_INPUT_SEED 0x5EEDu

static SimInput make_test_input(u32 player, u32 frame) { return sim_det_input(player, frame, TEST_INPUT_SEED); }

static Sim* det_sim(Arena* a) {
    SimConfig cfg;
    sim_det_config(&cfg);
    Sim* s = ARENA_NEW(a, Sim);
    sim_init(s, a, &cfg);
    return s;
}

TEST(sim_det_scene_moves_and_differs_by_seed) {
    Arena* a = test_arena();
    Sim* s1 = det_sim(a);
    Sim* s2 = det_sim(a);
    sim_det_setup(s1, 1u);
    sim_det_setup(s2, 1u);
    CHECK(sim_state_hash(s1) == sim_state_hash(s2));   /* farklı adres, aynı özet */
    u64 h0 = sim_state_hash(s1);
    sim_det_setup(s2, 2u);
    CHECK(sim_state_hash(s2) != h0);                     /* tohum sandık konumunu kaydırır */
    for (u32 f = 0; f < 120u; f++) {
        for (u32 p = 0; p < SIM_DET_PLAYERS; p++) sim_submit_input(s1, p, f, make_test_input(p, f));
        sim_step(s1);
    }
    CHECK(sim_state_hash(s1) != h0);
    CHECK(!sim_det_has_nan(s1));
    /* durum bölgesine sığdı: arena hiç başarısız olmadı, kullanılan bölge raporlanır */
    CHECK(s1->state_arena.fails == 0u);
    CHECK(s1->state_arena.used <= s1->state_size);
    test_note("durum bölgesi kullanılan (KB)", (f64)s1->state_arena.used / 1024.0, 1);
}

/* ============================================================================
 * 3. İki Eş (Peer) Simülasyon: 1-8 Kare Gecikmeli Girdi + Rollback Testi
 * ============================================================================ */

TEST(sim_peer_rollback_delay_1_to_8) {
    Arena* a = test_arena();
    Sim* peer_a = det_sim(a);
    Sim* peer_b = det_sim(a);

    /* A) Sabit gecikmeler d = 1..8 kare: her karede gecikmeli girdi akışı + rollback */
    for (u32 delay = 1u; delay <= 8u; delay++) {
        sim_det_setup(peer_a, 0x1000u + delay);
        sim_det_setup(peer_b, 0x1000u + delay);
        CHECK(sim_state_hash(peer_a) == sim_state_hash(peer_b));

        const u32 FRAMES = 240u;
        for (u32 f = 0u; f < FRAMES; f++) {
            SimInput in0 = make_test_input(0u, f);
            SimInput in1 = make_test_input(1u, f);

            /* Eş A: tüm girdileri gecikmesiz alır */
            sim_submit_input(peer_a, 0u, f, in0);
            sim_submit_input(peer_a, 1u, f, in1);
            sim_step(peer_a);

            /* Eş B: oyuncu 0 yerel (gecikmesiz), oyuncu 1 ise `delay` kare gecikmeli gelir */
            sim_submit_input(peer_b, 0u, f, in0);
            if (f >= delay) {
                u32 arrived_f = f - delay;
                CHECK(sim_submit_input(peer_b, 1u, arrived_f, make_test_input(1u, arrived_f)));
            }
            sim_step(peer_b);

            /* Gecikmeli girdisi ulaşmış olan tüm karelerde (0 .. f - delay) özetler birebir aynı olmalı */
            if (f >= delay) {
                u32 confirmed_f = f - delay;
                u64 ha = sim_frame_hash(peer_a, confirmed_f);
                u64 hb = sim_frame_hash(peer_b, confirmed_f);
                CHECK(ha != 0ULL);
                CHECK(ha == hb);
                CHECK(!sim_check_desync(peer_b, confirmed_f, ha));
            }
        }

        /* Kuyrukta kalan son `delay` karelik gecikmeli girdileri teslim et ve rollback'i çöz */
        for (u32 rem = FRAMES - delay; rem < FRAMES; rem++) {
            CHECK(sim_submit_input(peer_b, 1u, rem, make_test_input(1u, rem)));
        }
        sim_resolve_rollback(peer_b);

        /* Tüm karelerin özetleri ve son durum özeti birebir aynı olmalı */
        CHECK(peer_b->stats.rollback_count > 0ULL);
        CHECK(peer_b->stats.max_rollback_depth == delay);
        CHECK(sim_state_hash(peer_a) == sim_state_hash(peer_b));
        for (u32 f = FRAMES - 200u; f < FRAMES; f++) {   /* özet halkası son 256 kareyi tutar */
            CHECK(sim_frame_hash(peer_a, f) == sim_frame_hash(peer_b, f));
        }
    }

    /* B) Değişken (jitter: 1..8 kare rastgele gecikme ve sırasız paket varışı) + her karede yakalama */
    sim_det_setup(peer_a, 0x9999u);
    sim_det_setup(peer_b, 0x9999u);

    Rng net_rng = rng_make(0xDEADBEEFULL);
    enum { JITTER_FRAMES = 400 };
    u32 arrival_frame[JITTER_FRAMES];
    b32 delivered[JITTER_FRAMES];
    for (u32 f = 0u; f < JITTER_FRAMES; f++) {
        u32 d = 1u + (rng_u32(&net_rng) % 8u); /* 1..8 kare gecikme */
        arrival_frame[f] = f + d;
        delivered[f] = FALSE;
    }

    for (u32 f = 0u; f < JITTER_FRAMES; f++) {
        SimInput in0 = make_test_input(0u, f);
        SimInput in1 = make_test_input(1u, f);

        sim_submit_input(peer_a, 0u, f, in0);
        sim_submit_input(peer_a, 1u, f, in1);
        sim_step(peer_a);

        sim_submit_input(peer_b, 0u, f, in0);
        /* Bu karede veya daha önce varış zamanı gelmiş (sırasız dahil) paketleri teslim et */
        for (u32 k = 0u; k <= f; k++) {
            if (!delivered[k] && arrival_frame[k] <= f) {
                CHECK(sim_submit_input(peer_b, 1u, k, make_test_input(1u, k)));
                delivered[k] = TRUE;
            }
        }
        sim_step(peer_b);

        /* Tüm k <= min_undelivered - 1 kareleri için iki eşin kare özetleri birebir aynı olmalı */
        u32 sync_limit = f + 1u;
        for (u32 k = 0u; k <= f; k++) {
            if (!delivered[k]) {
                sync_limit = k;
                break;
            }
        }
        for (u32 k = (sync_limit > 16u) ? (sync_limit - 16u) : 0u; k < sync_limit; k++) {
            CHECK(sim_frame_hash(peer_a, k) == sim_frame_hash(peer_b, k));
        }
    }

    /* Kalan uçuş halindeki paketleri teslim et, rollback çöz -> her karede özetler aynı */
    for (u32 k = 0u; k < JITTER_FRAMES; k++) {
        if (!delivered[k]) {
            CHECK(sim_submit_input(peer_b, 1u, k, make_test_input(1u, k)));
            delivered[k] = TRUE;
        }
    }
    sim_resolve_rollback(peer_b);
    CHECK(sim_state_hash(peer_a) == sim_state_hash(peer_b));

    /* C) Saf ofset/indeks modeli (hash_fn == NULL, ham state baytları doğrudan özetlenir) */
    SimConfig off_cfg;
    mem_zero(&off_cfg, sizeof(off_cfg));
    off_cfg.state_size   = 512u;
    off_cfg.player_count = 2u;
    off_cfg.snapshot_cap = 16u;
    off_cfg.fixed_dt     = SIM_DEFAULT_DT;
    off_cfg.step_fn      = offset_game_step;
    off_cfg.hash_fn      = NULL;

    Sim* off_a = ARENA_NEW(a, Sim);
    Sim* off_b = ARENA_NEW(a, Sim);
    sim_init(off_a, a, &off_cfg);
    sim_init(off_b, a, &off_cfg);
    offset_game_init(t_, off_a, 6u);
    offset_game_init(t_, off_b, 6u);

    for (u32 f = 0u; f < 160u; f++) {
        SimInput in0 = make_test_input(0u, f);
        SimInput in1 = make_test_input(1u, f);

        sim_submit_input(off_a, 0u, f, in0);
        sim_submit_input(off_a, 1u, f, in1);
        sim_step(off_a);

        /* Eş B: oyuncu 0 gerçek, oyuncu 1 önce tahminle adım atar */
        sim_submit_input(off_b, 0u, f, in0);
        sim_step(off_b);

        /* Her 1..8 karelik pencerede gecikmeli gerçek girdiler ulaşıp rollback çözülünce özetler eşleşir */
        u32 window = 1u + (f % 8u);
        if ((f + 1u) % window == 0u || f == 159u) {
            u32 start_k = (f + 1u >= 8u) ? (f + 1u - 8u) : 0u;
            for (u32 k = start_k; k <= f; k++) {
                CHECK(sim_submit_input(off_b, 1u, k, make_test_input(1u, k)));
            }
            sim_resolve_rollback(off_b);
            CHECK(sim_state_hash(off_a) == sim_state_hash(off_b));
        }
    }
}

/* ============================================================================
 * 4. 10.000 Kare Fizik + ECS Replay Belirlenimcilik Testi
 * ============================================================================ */

TEST(sim_replay_10000_frames_phys_ecs) {
    Arena* a = test_arena();
    const u32 TOTAL_FRAMES = SIM_DET_DEFAULT_TICKS;
    const u32 SEED         = SIM_DET_DEFAULT_SEED;

    Sim* sim1 = det_sim(a);
    Sim* sim2 = det_sim(a);

    SimReplay rep_orig;
    sim_replay_init(&rep_orig, a, SIM_DET_PLAYERS, TOTAL_FRAMES, SEED, SIM_DEFAULT_DT);

    u64* hashes_run1 = ARENA_ARRAY(a, u64, TOTAL_FRAMES);
    u64* hashes_run2 = ARENA_ARRAY(a, u64, TOTAL_FRAMES);

    /* 1. Koşu: 10.000 kare fizik + ECS simüle et ve girdileri SimReplay'e kaydet */
    sim_det_setup(sim1, SEED);
    sim_attach_replay(sim1, &rep_orig);
    f64 t0 = sys_now_ms();
    for (u32 f = 0u; f < TOTAL_FRAMES; f++) {
        for (u32 p = 0; p < SIM_DET_PLAYERS; p++) sim_submit_input(sim1, p, f, sim_det_input(p, f, SEED));
        sim_step(sim1);
        hashes_run1[f] = sim_state_hash(sim1);
    }
    f64 run_ms = sys_now_ms() - t0;
    sim_attach_replay(sim1, NULL);
    CHECK(rep_orig.frame_count == TOTAL_FRAMES);
    CHECK(!sim_det_has_nan(sim1));

    /* Replay ikili serileştirme + geri yükleme gidiş-dönüş doğrulaması */
    u32 bin_cap = 64u + TOTAL_FRAMES * SIM_DET_PLAYERS * (u32)sizeof(SimInput);
    u8* bin_buf = ARENA_ARRAY(a, u8, bin_cap);
    u32 written = sim_replay_save(&rep_orig, bin_buf, bin_cap);
    CHECK(written > sizeof(SimInput) * TOTAL_FRAMES * SIM_DET_PLAYERS);

    SimReplay rep_loaded;
    CHECK(sim_replay_load(&rep_loaded, a, bin_buf, written));
    CHECK(rep_loaded.frame_count == TOTAL_FRAMES);
    CHECK(rep_loaded.seed == SEED);

    /* 2. Koşu: ikinci Sim örneğinde (farklı bellek adresi) yüklenen kaydı oynat */
    sim_det_setup(sim2, rep_loaded.seed);
    CHECK(sim_replay_run(sim2, &rep_loaded, hashes_run2, TOTAL_FRAMES));
    CHECK(sim2->current_frame == TOTAL_FRAMES);
    CHECK(!sim_det_has_nan(sim2));

    u32 mismatches = 0u;
    u32 distinct_transitions = 0u;
    for (u32 f = 0u; f < TOTAL_FRAMES; f++) {
        if (hashes_run1[f] != hashes_run2[f]) mismatches++;
        if (f > 0u && hashes_run1[f] != hashes_run1[f - 1u]) distinct_transitions++;
    }
    CHECK(mismatches == 0u);
    CHECK(distinct_transitions > 9900u);
    CHECK(sim_state_hash(sim1) == sim_state_hash(sim2));

    /* sim_det_run aynı tohumla aynı son özeti verir (kayıt yolundan bağımsız) */
    SimDetResult r = sim_det_run(a, TOTAL_FRAMES, SEED, NULL, 0);
    CHECK(r.final_hash == sim_state_hash(sim1));
    CHECK(!r.has_nan);
    test_note("10 000 kare (snapshot + adım + özet) ms", run_ms, 1);
}

/* ============================================================================
 * 5. Benchmark: 8 Kare Geri Sarma + Yeniden Simülasyon Maliyeti
 * ============================================================================ */

static f64 sort_median(f64* v, u32 n) {
    for (u32 i = 1u; i < n; i++) {
        f64 key = v[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2u];
}

TEST(sim_benchmark_8_frame_rollback) {
    Arena* a = test_arena();
    Sim* sim = det_sim(a);
    sim_det_setup(sim, 0xBEEF2026u);

    /* Sahneyi ısındırmak ve temaslar oluşturmak için 120 kare ilerlet */
    for (u32 f = 0u; f < 120u; f++) {
        sim_submit_input(sim, 0u, f, make_test_input(0u, f));
        sim_submit_input(sim, 1u, f, make_test_input(1u, f));
        sim_step(sim);
    }

    u32 base_frame = sim->current_frame; /* 120 */
    u32 target_frame = base_frame - 8u;  /* 112 (8 kare geri sarma) */
    u64 expected_hash = sim_state_hash(sim);

    const u32 WARMUP = 20u;
    enum { ITERS = 101 };
    f64 rb_samples[ITERS];
    f64 snap_samples[ITERS];
    f64 hash_samples[ITERS];

    for (u32 i = 0u; i < WARMUP + ITERS; i++) {
        f64 ts0 = sys_now_ms();
        CHECK(sim_restore_snapshot(sim, target_frame));
        f64 ts1 = sys_now_ms();
        sim->current_frame = base_frame;

        f64 t0 = sys_now_ms();
        CHECK(sim_rollback_to(sim, target_frame));
        f64 t1 = sys_now_ms();

        f64 th0 = sys_now_ms();
        u64 h = sim_state_hash(sim);
        f64 th1 = sys_now_ms();
        CHECK(h == expected_hash);

        if (i >= WARMUP) {
            snap_samples[i - WARMUP] = ts1 - ts0;
            rb_samples[i - WARMUP]   = t1 - t0;
            hash_samples[i - WARMUP] = th1 - th0;
        }
    }

    f64 med_rb_ms   = sort_median(rb_samples, ITERS);
    f64 med_snap_ms = sort_median(snap_samples, ITERS);
    f64 med_hash_ms = sort_median(hash_samples, ITERS);
    log_printf("  [bench M13 faz 5] durum=%u KB (kullanılan %u KB) | snapshot geri yukleme: %.4f ms | "
               "1 kare sim: %.4f ms | 8 kare rollback+resim: %.4f ms | hash: %.4f ms",
               sim->state_size / 1024u, (u32)(sim->state_arena.used / 1024u), med_snap_ms, med_rb_ms / 8.0, med_rb_ms,
               med_hash_ms);

    CHECK(sim->current_frame == base_frame);
    CHECK(sim_state_hash(sim) == expected_hash);
}
