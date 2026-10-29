/* tests/test_audio.c — Synth, ses başına SVF filtre, Schroeder/Freeverb yankı ve ses halka tamponu testleri:
 * determinizm, [-1,1] sınırı ve NaN yokluğu, zarf süresi, frekans doğruluğu, üstel kayma,
 * filtre frekans yanıtı (200 Hz geçiş / 8 kHz bastırma / 0.99 rezonans kararlılığı),
 * reverb dürtü sönümü ve örnekleme hızı ölçeklemesi, bit-bit geriye uyumluluk ve başarım ölçümü.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (tests/test_audio.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "test.h"
#include "core/math.h"
#include "core/sys.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "audio/synth.h"
#include "audio/audio.h"

static f32* g_buf;    /* 48000·3 örnek, her TEST başında test_arena'dan (statik 1,1 MB ilk belleği aşıyordu) */
static f32* g_buf2;

static SynthSfx tone(SynthWave w, f32 hz, f32 sec) {
    SynthSfx s;
    __builtin_memset(&s, 0, sizeof(s));
    s.wave = w;
    s.freq = hz;
    s.attack = 0.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.hold = sec;
    s.release = 0.001f;
    s.volume = 0.5f;
    return s;
}

static u32 zero_crossings(const f32* x, u32 n) {
    u32 c = 0;
    for (u32 i = 1; i < n; i++) c += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
    return c;
}

TEST(synth_frequency_and_envelope) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s;
    static const SynthWave waves[] = { WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_SINE };
    for (u32 k = 0; k < ARRAY_COUNT(waves); k++) {
        synth_init(&s, 44100);
        SynthSfx t = tone(waves[k], 441.0f, 1.0f);
        synth_play(&s, &t);
        synth_render(&s, g_buf, 44100);
        u32 zc = zero_crossings(g_buf, 44100);
        CHECK(zc >= 878 && zc <= 886);          /* 441 Hz → saniyede 882 geçiş */
        synth_render(&s, g_buf, 441);            /* hold + release bitti */
        CHECK(synth_active_voices(&s) == 0);
    }
    /* Attack: ilk 5 ms'nin tepe genliği, 50 ms sonrakinden belirgin küçük */
    synth_init(&s, 44100);
    SynthSfx a = tone(WAVE_SQUARE, 220.0f, 0.2f);
    a.attack = 0.05f;
    synth_play(&s, &a);
    synth_render(&s, g_buf, 8820);
    f32 early = 0.0f, late = 0.0f;
    for (u32 i = 0; i < 220; i++) early = m_maxf(early, m_absf(g_buf[i]));
    for (u32 i = 4410; i < 8820; i++) late = m_maxf(late, m_absf(g_buf[i]));
    CHECK(early < late * 0.2f);
}

TEST(synth_slide_and_bounds) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s;
    synth_init(&s, 44100);
    SynthSfx t = tone(WAVE_SINE, 200.0f, 1.0f);
    t.freq_end = 800.0f;
    synth_play(&s, &t);
    synth_render(&s, g_buf, 44100);
    u32 zc_first = zero_crossings(g_buf, 4410);
    u32 zc_last  = zero_crossings(g_buf + 39690, 4410);
    CHECK(zc_last > zc_first * 3);               /* 2 oktav yükseldi */

    /* Tüm sesler + gürültü + vibrato tam güçte: çıktı [-1,1], NaN yok */
    synth_init(&s, 48000);
    for (u32 i = 0; i < SYNTH_MAX_VOICES + 4; i++) {
        SynthSfx x = tone((SynthWave)(i % 5), 50.0f + 97.0f * (f32)i, 0.5f);
        x.volume = 1.0f;
        x.vibrato_hz = 6.0f;
        x.vibrato_depth = 0.5f;
        synth_play(&s, &x);
    }
    CHECK(synth_active_voices(&s) == SYNTH_MAX_VOICES);
    synth_render(&s, g_buf, 48000);
    u32 bad = 0;
    for (u32 i = 0; i < 48000; i++) bad += !(g_buf[i] >= -1.0f && g_buf[i] <= 1.0f);
    CHECK(bad == 0);
}

TEST(synth_pattern_and_determinism) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    i8 n[16];
    u32 c = synth_parse_pattern("C4 E4 . - A#3 Bb2 | G9", n, 16);
    CHECK(c == 7);
    CHECK(n[0] == 60 && n[1] == 64 && n[2] == -2 && n[3] == -1);
    CHECK(n[4] == 58 && n[5] == 46 && n[6] == 127);

    static Synth s1, s2;
    SynthSfx inst = tone(WAVE_SQUARE, 0.0f, 0.08f);
    inst.duty = 0.25f;
    for (u32 k = 0; k < 2; k++) {
        Synth* s = k ? &s2 : &s1;
        synth_init(s, 44100);
        synth_song_add_track(s, "C4 E4 G4 C5 . G4 E4 -", &inst);
        synth_song_add_track(s, "C2 - G2 -", &inst);
        synth_song_start(s, 140.0f, 4);
    }
    synth_render(&s1, g_buf, 44100);
    synth_render(&s2, g_buf2, 44100);
    u32 diff = 0;
    f32 energy = 0.0f;
    for (u32 i = 0; i < 44100; i++) {
        diff += (g_buf[i] != g_buf2[i]);
        energy += g_buf[i] * g_buf[i];
    }
    CHECK(diff == 0);
    CHECK(energy > 100.0f);
}

/* Halka tampon: worklet benzeri tüketici ile okunan akış = doğrudan render (sarma dahil) */
TEST(audio_ring_stream_matches_direct) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    AudioRing* ring = engine_audio_init(44100);
    Synth* s = audio_synth();
    SynthSfx t = tone(WAVE_SAW, 330.0f, 2.0f);
    synth_play(s, &t);

    static Synth ref;
    synth_init(&ref, 44100);
    /* engine_audio_init halkayı AUDIO_LATENCY_S kadar ön doldurur (faz 4: açılış kesintisi 15 872 kare → 0). Bu kareler
     * ses çalınmadan önce render edilen sessizlik: tüketici onları okumuş sayılır, karşılaştırma tonla başlar */
    CHECK(ring->write_pos == (u32)(AUDIO_LATENCY_S * 44100.0f));
    for (u32 i = 0; i < ring->write_pos; i++) CHECK(ring->data[i] == 0.0f);
    ring->read_pos = ring->write_pos;
    synth_play(&ref, &t);

    u32 got = 0, mismatches = 0;
    for (u32 frame = 0; frame < 120; frame++) {
        audio_pump(0.07f);                                   /* ~3087 kare hedef */
        u32 fill = ring->write_pos - ring->read_pos;
        CHECK(fill <= ring->capacity);
        u32 take = 735;                                      /* 60 fps'de bir karelik tüketim */
        if (take > fill) take = fill;
        synth_render(&ref, g_buf, take);
        for (u32 i = 0; i < take; i++) {
            f32 v = ring->data[(ring->read_pos + i) & (ring->capacity - 1u)];
            mismatches += (v != g_buf[i]);
        }
        ring->read_pos += take;
        got += take;
    }
    CHECK(got > AUDIO_RING_FRAMES * 4u);   /* birkaç kez sarıldı */
    CHECK(mismatches == 0);
}

static f32 peak_abs_range(const f32* buf, u32 start, u32 end) {
    f32 pk = 0.0f;
    for (u32 i = start; i < end; i++) {
        f32 a = m_absf(buf[i]);
        if (a > pk) pk = a;
    }
    return pk;
}

/* 1 kHz alçak geçiren SVF: 200 Hz sinüs geçer (genlik >= 0.9), 8 kHz sinüs >= 18 dB bastırılır;
 * kesim kayması (cutoff_end_hz) ve şarkı izlerinde filtreli enstrüman çalışır. */
TEST(synth_filter_response_and_slide) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s_dry, s_flt;

    /* 200 Hz sinüs @ 1 kHz kesim */
    synth_init(&s_dry, 44100);
    synth_init(&s_flt, 44100);
    s_dry.master = 1.7f;   /* soft_clip(1.7) >= 0.94 → mutlak genlik de >= 0.9 */
    s_flt.master = 1.7f;
    SynthSfx s200_dry = tone(WAVE_SINE, 200.0f, 0.25f);
    s200_dry.volume = 1.0f;
    SynthSfx s200_flt = s200_dry;
    s200_flt.cutoff_hz = 1000.0f;
    synth_play(&s_dry, &s200_dry);
    synth_play(&s_flt, &s200_flt);
    synth_render(&s_dry, g_buf, 8820);
    synth_render(&s_flt, g_buf2, 8820);
    f32 pk200_dry = peak_abs_range(g_buf, 1000, 8000);
    f32 pk200_flt = peak_abs_range(g_buf2, 1000, 8000);
    CHECK(pk200_flt >= 0.9f);
    CHECK(pk200_flt / pk200_dry >= 0.95f);

    /* 8 kHz sinüs @ 1 kHz kesim: >= 18 dB bastırma (10^(-18/20) = 0.12589) */
    synth_init(&s_dry, 44100);
    synth_init(&s_flt, 44100);
    SynthSfx s8k_dry = tone(WAVE_SINE, 8000.0f, 0.25f);
    SynthSfx s8k_flt = s8k_dry;
    s8k_flt.cutoff_hz = 1000.0f;
    synth_play(&s_dry, &s8k_dry);
    synth_play(&s_flt, &s8k_flt);
    synth_render(&s_dry, g_buf, 8820);
    synth_render(&s_flt, g_buf2, 8820);
    f32 pk8k_dry = peak_abs_range(g_buf, 1000, 8000);
    f32 pk8k_flt = peak_abs_range(g_buf2, 1000, 8000);
    CHECK(pk8k_dry > 0.2f);
    CHECK(pk8k_flt <= pk8k_dry * 0.125f);   /* >= 18.06 dB bastırma (gerçekte ~38 dB) */

    /* Üstel kesim kayması: 200 Hz -> 6000 Hz açılan filtrede 1500 Hz testere sesi açılır */
    synth_init(&s_flt, 44100);
    SynthSfx sweep = tone(WAVE_SAW, 1500.0f, 0.5f);
    sweep.cutoff_hz     = 200.0f;
    sweep.cutoff_end_hz = 6000.0f;
    sweep.resonance     = 0.3f;
    synth_play(&s_flt, &sweep);
    synth_render(&s_flt, g_buf, 22050);
    f32 early_pk = peak_abs_range(g_buf, 500, 3000);
    f32 late_pk  = peak_abs_range(g_buf, 18000, 21500);
    CHECK(late_pk > early_pk * 4.0f);

    /* Şarkı izlerinde filtreli enstrüman */
    synth_init(&s_dry, 44100);
    synth_init(&s_flt, 44100);
    SynthSfx inst_dry = tone(WAVE_SAW, 0.0f, 0.12f);
    SynthSfx inst_flt = inst_dry;
    inst_flt.cutoff_hz     = 500.0f;
    inst_flt.cutoff_end_hz = 180.0f;
    inst_flt.resonance     = 0.4f;
    synth_song_add_track(&s_dry, "A4 C5 E5 A5", &inst_dry);
    synth_song_add_track(&s_flt, "A4 C5 E5 A5", &inst_flt);
    synth_song_start(&s_dry, 120.0f, 4);
    synth_song_start(&s_flt, 120.0f, 4);
    synth_render(&s_dry, g_buf, 22050);
    synth_render(&s_flt, g_buf2, 22050);
    u32 zc_song_dry = zero_crossings(g_buf, 22050);
    u32 zc_song_flt = zero_crossings(g_buf2, 22050);
    f32 diff_e = 0.0f;
    for (u32 i = 0; i < 22050; i++) {
        f32 d = g_buf[i] - g_buf2[i];
        diff_e += d * d;
    }
    CHECK(diff_e > 1.0f);
    CHECK(zc_song_flt <= zc_song_dry);
}

/* Rezonans 0.99'da 10 saniye gürültü girdisiyle kararlılık: NaN/Inf yok, [-1,1] sınırı korunur */
TEST(synth_filter_resonance_stability_10s) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s;
    synth_init(&s, 44100);
    SynthSfx n = tone(WAVE_NOISE, 4000.0f, 10.5f);
    n.volume        = 1.0f;
    n.cutoff_hz     = 1000.0f;
    n.cutoff_end_hz = 3500.0f;
    n.resonance     = 0.99f;
    synth_play(&s, &n);

    u32 bad = 0;
    f32 energy = 0.0f;
    for (u32 sec = 0; sec < 10; sec++) {
        synth_render(&s, g_buf, 44100);
        for (u32 i = 0; i < 44100; i++) {
            f32 v = g_buf[i];
            b32 finite = (v - v) == 0.0f;
            if (!finite || !(v >= -1.0f && v <= 1.0f)) bad++;
            energy += v * v;
        }
    }
    CHECK(bad == 0);
    CHECK(energy > 100.0f);
}

/* mix=0 ve cutoff=0 iken eski çıktıyla bit-bit aynı */
TEST(synth_bit_exact_when_fx_off) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    /* Sahne kodlarındaki gibi konumsal başlatıcı, filtre alanları 0 (kapalı) */
    static const SynthSfx k_legacy_pos = {
        WAVE_SQUARE, 300.0f, 600.0f, 0.25f, 0.002f, 0.05f, 0.4f, 0.03f, 0.06f, 0.30f, 6.0f, 0.4f, 0.0f, 0.0f, 0.0f
    };
    CHECK(k_legacy_pos.cutoff_hz == 0.0f);
    CHECK(k_legacy_pos.cutoff_end_hz == 0.0f);
    CHECK(k_legacy_pos.resonance == 0.0f);

    static Synth s1, s2;
    synth_init(&s1, 44100);
    synth_init(&s2, 44100);
    /* s2'de reverb önce açılıp sonra mix=0 yapılır */
    synth_set_reverb(&s2, 0.5f, 0.8f, 0.4f);
    synth_set_reverb(&s2, 0.0f, 0.8f, 0.4f);

    SynthSfx explicit_zero = k_legacy_pos;
    explicit_zero.cutoff_hz     = 0.0f;
    explicit_zero.cutoff_end_hz = 0.0f;
    explicit_zero.resonance     = 0.0f;

    SynthSfx noise_ev = {
        WAVE_NOISE, 1200.0f, 200.0f, 0.0f, 0.001f, 0.08f, 0.3f, 0.05f, 0.1f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
    };
    SynthSfx saw_ev = {
        WAVE_SAW, 220.0f, 110.0f, 0.0f, 0.005f, 0.1f, 0.5f, 0.1f, 0.15f, 0.35f, 5.0f, 0.3f, 0.0f, 0.0f, 0.0f
    };

    synth_play(&s1, &k_legacy_pos);
    synth_play(&s1, &noise_ev);
    synth_play(&s1, &saw_ev);
    synth_song_add_track(&s1, "C4 E4 G4 B4 . - D4 F4", &k_legacy_pos);
    synth_song_start(&s1, 130.0f, 4);

    synth_play(&s2, &explicit_zero);
    synth_play(&s2, &noise_ev);
    synth_play(&s2, &saw_ev);
    synth_song_add_track(&s2, "C4 E4 G4 B4 . - D4 F4", &explicit_zero);
    synth_song_start(&s2, 130.0f, 4);

    synth_render(&s1, g_buf, 44100);
    synth_render(&s2, g_buf2, 44100);
    CHECK(mem_equal(g_buf, g_buf2, 44100 * sizeof(f32)));
}

/* Reverb: dürtü yanıtı sönümlenir (2 s sonra |y| < 1e-3, room <= 0.9), [-1,1] korunur,
 * örnekleme hızı ölçeklemesi (44.1 / 48 kHz ve aşırı hızlar) ve bozuk girdi güvenliği. */
TEST(synth_reverb_decay_and_scaling) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s;
    const u32 rates[2] = { 44100u, 48000u };
    u32 len_44k[SYNTH_COMB_COUNT];

    for (u32 r = 0; r < 2; r++) {
        u32 sr = rates[r];
        synth_init(&s, sr);
        synth_set_reverb(&s, 0.6f, 0.9f, 0.3f);
        if (r == 0) {
            for (u32 c = 0; c < SYNTH_COMB_COUNT; c++) len_44k[c] = s.rev_comb_len[c];
        } else {
            for (u32 c = 0; c < SYNTH_COMB_COUNT; c++) {
                CHECK(s.rev_comb_len[c] > len_44k[c]);
                CHECK(s.rev_comb_len[c] <= SYNTH_COMB_CAP);
            }
        }

        /* Kısa dürtü (10 ms darbe) çal, 2.5 saniye render et */
        SynthSfx imp = tone(WAVE_SAW, 440.0f, 0.008f);
        imp.volume  = 1.0f;
        imp.release = 0.002f;
        synth_play(&s, &imp);

        u32 total = sr * 2u + sr / 2u;   /* 2.5 s */
        synth_render(&s, g_buf, total);

        /* Dürtü bittikten sonra (40 ms .. 300 ms) belirgin yankı kuyruğu olmalı */
        f32 tail_early = peak_abs_range(g_buf, sr / 25u, sr / 3u);
        /* 2 saniye sonrasında (2.0 s .. 2.5 s) |y| < 1e-3 olmalı */
        f32 tail_2s    = peak_abs_range(g_buf, sr * 2u, total);
        CHECK(tail_early > 0.02f);
        CHECK(tail_2s < 1e-3f);

        u32 bad = 0;
        for (u32 i = 0; i < total; i++) {
            f32 v = g_buf[i];
            if ((v - v) != 0.0f || !(v >= -1.0f && v <= 1.0f)) bad++;
        }
        CHECK(bad == 0);
    }

    /* Aşırı örnekleme hızları ve bozuk/NaN/Inf girdiler tampon kapasitesini aşmamalı */
    synth_init(&s, 1000000u);
    synth_set_reverb(&s, 10.0f, -5.0f, __builtin_nanf(""));
    for (u32 c = 0; c < SYNTH_COMB_COUNT; c++) {
        CHECK(s.rev_comb_len[c] >= 1u && s.rev_comb_len[c] <= SYNTH_COMB_CAP);
    }
    for (u32 a = 0; a < SYNTH_AP_COUNT; a++) {
        CHECK(s.rev_ap_len[a] >= 1u && s.rev_ap_len[a] <= SYNTH_AP_CAP);
    }
    SynthSfx hostile = {
        WAVE_SAW, __builtin_inff(), -100.0f, 2.0f,
        -1.0f, __builtin_nanf(""), 5.0f, 0.01f, -2.0f,
        10.0f, -5.0f, 999.0f,
        __builtin_inff(), __builtin_nanf(""), 50.0f
    };
    synth_play(&s, &hostile);
    synth_render(&s, g_buf, 1024);
    for (u32 i = 0; i < 1024; i++) {
        CHECK((g_buf[i] - g_buf[i]) == 0.0f);
        CHECK(g_buf[i] >= -1.0f && g_buf[i] <= 1.0f);
    }
}

static f64 median_f64(f64* a, u32 n) {
    for (u32 i = 1; i < n; i++) {
        f64 x = a[i];
        u32 j = i;
        while (j > 0 && a[j - 1] > x) { a[j] = a[j - 1]; j--; }
        a[j] = x;
    }
    return a[n / 2];
}

/* Benchmark: 16 aktif ses (SVF filtreli) + reverb, 1 s ses (44100 örnek) render süresi */
TEST(synth_bench_16_voices_reverb) {
    g_buf = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    g_buf2 = ARENA_ARRAY(test_arena(), f32, 48000 * 3);
    static Synth s;
    const u32 WARMUP = 5u;
    const u32 ITERS  = 31u;
    f64 t_dry[31], t_svf_fixed[31], t_svf_slide_rev[31];

    for (u32 it = 0; it < WARMUP + ITERS; it++) {
        synth_init(&s, 44100);
        for (u32 v = 0; v < 16; v++) {
            SynthSfx x = tone((SynthWave)(v % 5), 110.0f + 55.0f * (f32)v, 1.5f);
            x.freq_end = x.freq * 1.2f;
            synth_play(&s, &x);
        }
        f64 t0 = sys_now_ms();
        synth_render(&s, g_buf, 44100);
        f64 dt = sys_now_ms() - t0;
        if (it >= WARMUP) t_dry[it - WARMUP] = dt;
    }

    for (u32 it = 0; it < WARMUP + ITERS; it++) {
        synth_init(&s, 44100);
        synth_set_reverb(&s, 0.35f, 0.8f, 0.3f);
        for (u32 v = 0; v < 16; v++) {
            SynthSfx x = tone((SynthWave)(v % 5), 110.0f + 55.0f * (f32)v, 1.5f);
            x.freq_end  = x.freq * 1.2f;
            x.cutoff_hz = 1800.0f;
            x.resonance = 0.5f;
            synth_play(&s, &x);
        }
        f64 t0 = sys_now_ms();
        synth_render(&s, g_buf, 44100);
        f64 dt = sys_now_ms() - t0;
        if (it >= WARMUP) t_svf_fixed[it - WARMUP] = dt;
    }

    for (u32 it = 0; it < WARMUP + ITERS; it++) {
        synth_init(&s, 44100);
        synth_set_reverb(&s, 0.35f, 0.8f, 0.3f);
        for (u32 v = 0; v < 16; v++) {
            SynthSfx x = tone((SynthWave)(v % 5), 110.0f + 55.0f * (f32)v, 1.5f);
            x.freq_end      = x.freq * 1.2f;
            x.cutoff_hz     = 1800.0f;
            x.cutoff_end_hz = 600.0f;
            x.resonance     = 0.5f;
            synth_play(&s, &x);
        }
        f64 t0 = sys_now_ms();
        synth_render(&s, g_buf, 44100);
        f64 dt = sys_now_ms() - t0;
        if (it >= WARMUP) t_svf_slide_rev[it - WARMUP] = dt;
    }

    f64 med_dry   = median_f64(t_dry, ITERS);
    f64 med_fixed = median_f64(t_svf_fixed, ITERS);
    f64 med_slide = median_f64(t_svf_slide_rev, ITERS);
    log_printf("  [bench M12_B] 16 ses, 1 s @ 44.1 kHz (wasm): "
           "kuru=%.3f ms | SVF(sabit)+reverb=%.3f ms | SVF(kaymali)+reverb=%.3f ms",
           med_dry, med_fixed, med_slide);
    CHECK(med_slide > 0.0 && med_slide < 200.0);
}
