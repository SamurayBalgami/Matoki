/* tests/test_impact.c — M13 faz 4: onların testsiz eklenmiş deniz/hava ses yardımcıları (2ac467a, "yarım iş") ve
 * bizim çarpışma sesi köprüsü (audio/impact). Yardımcılar uç / hatalı girdide (NaN, ±sonsuz, negatif) sonlu, [-1, 1]
 * içinde kalmalı ve zarf süresi bitince susmalı. Köprü: düşen kutu zemine çarpınca bir kez ses, duran kutu sessiz,
 * fizik durum hash'i ses açık/kapalı aynı, DEM topu zemine çarpınca ses. */
#include "test.h"
#include "core/math.h"
#include "audio/synth.h"
#include "audio/impact.h"

#define OUT_CAP (48000u * 4u)
static f32* g_out;   /* OUT_CAP örnek, test_arena'dan */

static f32 f32_bits(u32 b) {
    f32 f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

/* sfx'i tek başına çal: [-1, 1] dışı ya da sayısal olmayan örnek sayısı, enerji, bitti mi */
static void render_sfx(const SynthSfx* sfx, u32* bad, f32* energy, b32* ended) {
    static Synth s;
    synth_init(&s, 48000);
    synth_play(&s, sfx);
    f32 dur = sfx->attack + sfx->decay + sfx->hold + sfx->release;
    u32 n = (u32)((dur + 0.05f) * 48000.0f);
    if (n > OUT_CAP) n = OUT_CAP;
    synth_render(&s, g_out, n);
    *bad = 0;
    *energy = 0.0f;
    for (u32 i = 0; i < n; i++) {
        *bad += !(g_out[i] >= -1.0f && g_out[i] <= 1.0f);
        *energy += g_out[i] * g_out[i];
    }
    *ended = synth_active_voices(&s) == 0;
}

TEST(synth_sea_sfx_helpers_bounded) {
    g_out = ARENA_ARRAY(test_arena(), f32, OUT_CAP);
    const f32 vals[] = { 0.0f, 0.5f, 1.0f, 7.0f, 30.0f, -3.0f, 1e9f, f32_bits(0x7fc00000u), f32_bits(0x7f800000u) };
    u32 bad_total = 0, not_ended = 0, silent = 0, cases = 0;
    for (u32 i = 0; i < ARRAY_COUNT(vals); i++) {
        SynthSfx x[7];
        synth_sfx_sea_wind(vals[i], vals[(i + 3) % ARRAY_COUNT(vals)], &x[0]);
        synth_sfx_hull_water(vals[i], (i & 1) != 0, &x[1]);
        synth_sfx_sail_flap(vals[i], &x[2]);
        synth_sfx_rope_creak(vals[i], &x[3]);
        synth_sfx_thunder(vals[i], &x[4]);
        synth_sfx_buoy_bell((i & 1) != 0, &x[5]);
        synth_sfx_impact(vals[i], vals[(i + 1) % ARRAY_COUNT(vals)], &x[6]);
        for (u32 k = 0; k < 7; k++) {
            u32 bad;
            f32 e;
            b32 ended;
            render_sfx(&x[k], &bad, &e, &ended);
            bad_total += bad;
            not_ended += !ended;
            /* gövde sesi hızla orantılı (0 → sessiz); diğerleri her girdide duyulur */
            if (k != 1 && k != 6 && !(e > 1e-3f)) silent++;
            cases++;
        }
    }
    CHECK(cases == 63);
    CHECK(bad_total == 0);
    CHECK(not_ended == 0);
    CHECK(silent == 0);
}

TEST(impact_sfx_scales_with_speed_and_mass) {
    SynthSfx soft, hard, heavy;
    synth_sfx_impact(2.0f, 1.0f, &soft);
    synth_sfx_impact(10.0f, 1.0f, &hard);
    synth_sfx_impact(10.0f, 8.0f, &heavy);
    CHECK(hard.volume > soft.volume);
    CHECK(hard.cutoff_hz > soft.cutoff_hz);
    CHECK(heavy.cutoff_hz < hard.cutoff_hz * 0.55f);   /* 8 kg: küp kök → yarı */
}

static void drop_world(PhysWorld* w, Arena* a, f32 height) {
    phys_world_init(w, a, 16, 64);
    BodyDesc pd = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &pd);
    BodyDesc bd = phys_desc_box(v3(0, height, 0), v3(0.5f, 0.5f, 0.5f), 2.0f);
    phys_add_body(w, &bd);
}

TEST(impact_from_phys_once_per_drop_and_no_feedback) {
    Arena* a = test_arena();
    static Synth s;
    synth_init(&s, 48000);
    PhysWorld w, ref;
    drop_world(&w, a, 3.0f);
    drop_world(&ref, a, 3.0f);
    ImpactAudio ia;
    impact_init(&ia, a, 16);
    u32 first = 0, total = 0;
    for (u32 k = 0; k < 120; k++) {   /* 2 s: 3 m düşüş ≈ 0,7 s, çarpma ≈ 7 m/s, sonra durur */
        phys_step(&w, 1.0f / 60.0f);
        phys_step(&ref, 1.0f / 60.0f);
        impact_advance(&ia, 1.0f / 60.0f);
        u32 p = impact_from_phys(&ia, &s, &w, 4);
        if (p && !first) first = k;
        total += p;
    }
    CHECK(total >= 1 && total <= 2);           /* ilk çarpma (+ en çok bir küçük sekme) */
    CHECK(first >= 35 && first <= 50);         /* t ≈ √(2·2,5/9,81) ≈ 0,71 s → adım ≈ 43 */
    CHECK(phys_state_hash(&w) == phys_state_hash(&ref));   /* ses simülasyona dokunmaz */

    /* yerde duran kutu: 2 s boyunca ses yok */
    u32 rest = 0;
    for (u32 k = 0; k < 120; k++) {
        phys_step(&w, 1.0f / 60.0f);
        impact_advance(&ia, 1.0f / 60.0f);
        rest += impact_from_phys(&ia, &s, &w, 4);
    }
    CHECK(rest == 0);
}

TEST(impact_from_dem_ball_hits_ground) {
    Arena* a = test_arena();
    static Synth s;
    synth_init(&s, 48000);
    DemWorld d;
    dem_init(&d, a, 4, 5.0e7, 0.45, 0.9);
    dem_add(&d, 0.0, 2.0, 0.0, 0.2, 1.0);
    ImpactAudio ia;
    impact_init(&ia, a, 4);
    f32 vb[3];
    u32 total = 0, first = 0;
    for (u32 k = 0; k < 60; k++) {   /* 1 s: 1,8 m düşüş ≈ 0,61 s */
        for (u32 j = 0; j < 3; j++) vb[j] = (f32)d.b[0].v[j];
        dem_step(&d, 1.0 / 60.0);
        impact_advance(&ia, 1.0f / 60.0f);
        u32 p = impact_from_dem(&ia, &s, &d, vb, 1.0f / 60.0f, 4);
        if (p && !first) first = k;
        total += p;
    }
    CHECK(total == 1);
    CHECK(first >= 33 && first <= 40);
}
