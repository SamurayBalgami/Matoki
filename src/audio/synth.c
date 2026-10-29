/* audio/synth.c — Osilatörler, zarf, ses tahsisi, ses başına TPT SVF alçak geçiren filtre,
 * Schroeder/Freeverb yankı (reverb), karıştırma ve müzik sıralayıcısı.
 * Kare/testere dalgalarındaki süreksizlikler polyBLEP ile yumuşatılır (aliasing azalır).
 * Karışım master ile ölçeklenip rasyonel tanh yaklaşımıyla yumuşak kırpılır.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/audio/synth.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "audio/synth.h"
#include "core/math.h"

/* 44.1 kHz için aralarında asal-yakın Freeverb/Schroeder gecikme uzunlukları (örnek) */
static const u32 k_comb_base[SYNTH_COMB_COUNT] = { 1116u, 1188u, 1277u, 1356u };
static const u32 k_ap_base[SYNTH_AP_COUNT]     = { 556u, 441u };

/* NaN / ±Inf güvenli aralık kıstırma ve sonluluk kontrolü */
static inline b32 is_finite_f32(f32 x) {
    return (x - x) == 0.0f;
}

static inline f32 clamp_safe(f32 x, f32 lo, f32 hi) {
    if (!is_finite_f32(x) || !(x >= lo)) return lo;
    if (!(x <= hi)) return hi;
    return x;
}

static void reverb_init_lengths(Synth* s) {
    u32 sr = s->sample_rate ? s->sample_rate : 44100u;
    for (u32 c = 0; c < SYNTH_COMB_COUNT; c++) {
        u64 scaled = ((u64)k_comb_base[c] * (u64)sr + 22050u) / 44100u;
        if (scaled < 1u) scaled = 1u;
        if (scaled > (u64)SYNTH_COMB_CAP) scaled = (u64)SYNTH_COMB_CAP;
        s->rev_comb_len[c] = (u32)scaled;
        if (s->rev_comb_pos[c] >= s->rev_comb_len[c]) s->rev_comb_pos[c] = 0u;
    }
    for (u32 a = 0; a < SYNTH_AP_COUNT; a++) {
        u64 scaled = ((u64)k_ap_base[a] * (u64)sr + 22050u) / 44100u;
        if (scaled < 1u) scaled = 1u;
        if (scaled > (u64)SYNTH_AP_CAP) scaled = (u64)SYNTH_AP_CAP;
        s->rev_ap_len[a] = (u32)scaled;
        if (s->rev_ap_pos[a] >= s->rev_ap_len[a]) s->rev_ap_pos[a] = 0u;
    }
}

void synth_init(Synth* s, u32 sample_rate) {
    if (!s) return;
    __builtin_memset(s, 0, sizeof(*s));
    s->sample_rate = sample_rate ? sample_rate : 44100u;
    s->master      = 0.6f;
    s->song.volume = 1.0f;
    reverb_init_lengths(s);
}

void synth_set_reverb(Synth* s, f32 mix, f32 room, f32 damp) {
    if (!s) return;
    reverb_init_lengths(s);
    s->rev_mix      = clamp_safe(mix, 0.0f, 1.0f);
    s->rev_room     = clamp_safe(room, 0.0f, 1.0f);
    s->rev_damp     = clamp_safe(damp, 0.0f, 1.0f) * 0.4f;
    s->rev_feedback = 0.60f + 0.28f * s->rev_room;
}

f32 synth_midi_to_hz(i32 midi) {
    return 440.0f * m_exp2f((f32)(midi - 69) / 12.0f);
}

i32 synth_play(Synth* s, const SynthSfx* x) {
    if (!s || !x) return -1;
    i32 slot = -1;
    u32 oldest = 0xFFFFFFFFu;
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = (i32)i; break; }
        if (s->voices[i].id < oldest) { oldest = s->voices[i].id; slot = (i32)i; }
    }
    SynthVoice* v = &s->voices[slot];
    f32 sr = (f32)(s->sample_rate ? s->sample_rate : 44100u);
    f32 freq     = is_finite_f32(x->freq)     ? m_maxf(x->freq, 0.0f)     : 0.0f;
    f32 freq_end = is_finite_f32(x->freq_end) ? m_maxf(x->freq_end, 0.0f) : 0.0f;
    v->active    = TRUE;
    v->wave      = x->wave;
    v->phase     = 0.0f;
    v->inc       = freq / sr;
    v->duty      = (x->duty > 0.0f && x->duty < 1.0f) ? x->duty : 0.5f;
    v->attack    = is_finite_f32(x->attack)  ? m_maxf(x->attack, 0.0f)  : 0.0f;
    v->decay     = is_finite_f32(x->decay)   ? m_maxf(x->decay, 0.0f)   : 0.0f;
    v->sustain   = clamp_safe(x->sustain, 0.0f, 1.0f);
    v->hold      = is_finite_f32(x->hold)    ? m_maxf(x->hold, 0.0f)    : 0.0f;
    v->release   = is_finite_f32(x->release) ? m_maxf(x->release, 0.001f) : 0.001f;
    v->t         = 0.0f;
    v->dt        = 1.0f / sr;
    v->volume    = clamp_safe(x->volume, 0.0f, 1.0f);
    v->vib_phase = 0.0f;
    v->vib_inc   = (is_finite_f32(x->vibrato_hz) ? m_maxf(x->vibrato_hz, 0.0f) : 0.0f) / sr;
    v->vib_depth = clamp_safe(x->vibrato_depth, -48.0f, 48.0f);
    v->lfsr      = 0x7FFFu;
    v->noise_out = 1.0f;
    v->id        = s->next_id++;
    f32 dur = v->attack + v->decay + v->hold + v->release;
    v->inc_mul = 1.0f;
    if (freq_end > 0.0f && freq > 0.0f && dur > 0.0f) {
        v->inc_mul = m_exp2f(m_log2f(freq_end / freq) / (dur * sr));
    }
    /* Ses başına TPT SVF alçak geçiren filtre (Zavalishin / Cytomic) */
    v->flt_g     = 0.0f;
    v->flt_g_mul = 1.0f;
    v->flt_k     = 0.0f;
    v->flt_a1    = 0.0f;
    v->flt_a2    = 0.0f;
    v->flt_a3    = 0.0f;
    v->flt_s1    = 0.0f;
    v->flt_s2    = 0.0f;
    if (x->cutoff_hz > 0.0f) {
        f32 max_fc = 0.49f * sr;
        f32 min_fc = m_minf(5.0f, max_fc);
        f32 c0     = clamp_safe(x->cutoff_hz, min_fc, max_fc);
        f32 res    = clamp_safe(x->resonance, 0.0f, 0.99f);
        f32 g0     = m_tanf(M_PI_F * c0 / sr);
        f32 k      = 1.41421356f * (1.0f - res);
        v->flt_g   = g0;
        v->flt_k   = k;
        v->flt_a1  = 1.0f / (1.0f + g0 * (g0 + k));
        v->flt_a2  = g0 * v->flt_a1;
        v->flt_a3  = g0 * v->flt_a2;
        if (x->cutoff_end_hz > 0.0f && dur > 0.0f) {
            f32 c1 = clamp_safe(x->cutoff_end_hz, min_fc, max_fc);
            f32 g1 = m_tanf(M_PI_F * c1 / sr);
            if (g1 > 0.0f && g0 > 0.0f) {
                v->flt_g_mul = m_exp2f(m_log2f(g1 / g0) / (dur * sr));
            }
        }
    }
    return slot;
}

void synth_stop_all(Synth* s) {
    if (!s) return;
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) s->voices[i].active = FALSE;
}

u32 synth_active_voices(const Synth* s) {
    if (!s) return 0;
    u32 n = 0;
    for (u32 i = 0; i < SYNTH_MAX_VOICES; i++) n += s->voices[i].active ? 1u : 0u;
    return n;
}

static inline f32 env_level(const SynthVoice* v, b32* done) {
    f32 t = v->t;
    if (t < v->attack) return t / v->attack;
    t -= v->attack;
    if (t < v->decay) return 1.0f - (1.0f - v->sustain) * (t / v->decay);
    t -= v->decay;
    if (t < v->hold) return v->sustain;
    t -= v->hold;
    if (t < v->release) return v->sustain * (1.0f - t / v->release);
    *done = TRUE;
    return 0.0f;
}

static inline f32 poly_blep(f32 t, f32 dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline f32 voice_sample(SynthVoice* v) {
    f32 inc = v->inc;
    if (v->vib_depth != 0.0f) {
        inc *= m_exp2f(v->vib_depth * (1.0f / 12.0f) * m_sinf(v->vib_phase * M_TAU_F));
        v->vib_phase += v->vib_inc;
        if (v->vib_phase >= 1.0f) v->vib_phase -= 1.0f;
    }
    inc = m_minf(inc, 0.5f);   /* Nyquist üstüne çıkma */
    f32 p = v->phase, o;
    switch (v->wave) {
    case WAVE_SQUARE: {
        o = (p < v->duty) ? 1.0f : -1.0f;
        o += poly_blep(p, inc);
        f32 p2 = p + 1.0f - v->duty;
        if (p2 >= 1.0f) p2 -= 1.0f;
        o -= poly_blep(p2, inc);
    } break;
    case WAVE_TRIANGLE:
        o = 1.0f - 4.0f * m_absf(p - 0.5f);
        break;
    case WAVE_SAW:
        o = 2.0f * p - 1.0f - poly_blep(p, inc);
        break;
    case WAVE_SINE:
        o = m_sinf(p * M_TAU_F);
        break;
    case WAVE_NOISE:
    default:
        o = v->noise_out;
        break;
    }
    p += inc;
    if (p >= 1.0f) {
        p -= m_floorf(p);
        if (v->wave == WAVE_NOISE) {
            u32 bit = (v->lfsr ^ (v->lfsr >> 1)) & 1u;
            v->lfsr = (v->lfsr >> 1) | (bit << 14);
            v->noise_out = (v->lfsr & 1u) ? 1.0f : -1.0f;
        }
    }
    v->phase = p;
    v->inc *= v->inc_mul;
    if (v->flt_g > 0.0f) {
        f32 v3 = o - v->flt_s2;
        f32 v1 = v->flt_a1 * v->flt_s1 + v->flt_a2 * v3;
        f32 v2 = v->flt_s2 + v->flt_a2 * v->flt_s1 + v->flt_a3 * v3;
        v->flt_s1 = 2.0f * v1 - v->flt_s1;
        v->flt_s2 = 2.0f * v2 - v->flt_s2;
        if (v->flt_g_mul != 1.0f) {
            f32 g = v->flt_g * v->flt_g_mul;
            v->flt_g  = g;
            v->flt_a1 = 1.0f / (1.0f + g * (g + v->flt_k));
            v->flt_a2 = g * v->flt_a1;
            v->flt_a3 = g * v->flt_a2;
        }
        o = v2;
    }
    return o;
}

static inline f32 reverb_sample(Synth* s, f32 x) {
    f32 xin = x * 0.25f + 1e-20f;   /* denormal (alt-normal) çökmesini dallanmasız önler */
    f32 acc = 0.0f;
    f32 fb  = s->rev_feedback;
    f32 d2  = s->rev_damp;
    f32 d1  = 1.0f - d2;
    for (u32 c = 0; c < SYNTH_COMB_COUNT; c++) {
        u32 pos = s->rev_comb_pos[c];
        u32 len = s->rev_comb_len[c];
        f32 y   = s->rev_comb_buf[c][pos];
        f32 st  = y * d1 + s->rev_comb_store[c] * d2;
        s->rev_comb_store[c]    = st;
        s->rev_comb_buf[c][pos] = xin + st * fb;
        if (++pos >= len) pos = 0u;
        s->rev_comb_pos[c] = pos;
        acc += y;
    }
    for (u32 a = 0; a < SYNTH_AP_COUNT; a++) {
        u32 pos = s->rev_ap_pos[a];
        u32 len = s->rev_ap_len[a];
        f32 bo  = s->rev_ap_buf[a][pos];
        f32 v   = acc + 0.5f * bo;
        s->rev_ap_buf[a][pos] = v;
        acc = bo - 0.5f * v;
        if (++pos >= len) pos = 0u;
        s->rev_ap_pos[a] = pos;
    }
    return acc;
}

static void song_step(Synth* s) {
    SynthSong* g = &s->song;
    for (u32 k = 0; k < g->track_count; k++) {
        SynthTrack* tr = &g->tracks[k];
        if (tr->length == 0) continue;
        i32 n = tr->notes[g->step % tr->length];
        if (n < 0) continue;
        SynthSfx sfx = tr->instrument;
        f32 ratio = (sfx.freq_end > 0.0f && sfx.freq > 0.0f) ? sfx.freq_end / sfx.freq : 0.0f;
        sfx.freq     = synth_midi_to_hz(n);
        sfx.freq_end = ratio > 0.0f ? sfx.freq * ratio : 0.0f;
        sfx.volume  *= g->volume;
        synth_play(s, &sfx);
    }
    g->step++;
}

static inline f32 soft_clip(f32 x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    return m_clampf(x * (27.0f + x * x) / (27.0f + 9.0f * x * x), -1.0f, 1.0f);   /* yuvarlama taşmasına karşı */
}

void synth_render(Synth* s, f32* out, u32 frames) {
    if (!s || !out) return;
    if (s->rev_mix > 0.0f && s->rev_comb_len[0] == 0u) reverb_init_lengths(s);
    f32 dt = 1.0f / (f32)(s->sample_rate ? s->sample_rate : 44100u);
    for (u32 i = 0; i < frames; i++) {
        if (s->song.playing) {
            s->song.acc -= dt;
            if (s->song.acc <= 0.0f) {
                s->song.acc += s->song.step_seconds;
                song_step(s);
            }
        }
        f32 mix = 0.0f;
        for (u32 k = 0; k < SYNTH_MAX_VOICES; k++) {
            SynthVoice* v = &s->voices[k];
            if (!v->active) continue;
            b32 done = FALSE;
            f32 e = env_level(v, &done);
            if (done) { v->active = FALSE; continue; }
            mix += voice_sample(v) * e * v->volume;
            v->t += v->dt;
        }
        if (s->rev_mix > 0.0f) {
            f32 wet = reverb_sample(s, mix);
            mix = mix * (1.0f - s->rev_mix) + wet * s->rev_mix;
        }
        out[i] = soft_clip(mix * s->master);
    }
    s->frames_rendered += frames;
}

static i32 note_base(char c) {
    switch (c) {
    case 'C': return 0;  case 'D': return 2;  case 'E': return 4;  case 'F': return 5;
    case 'G': return 7;  case 'A': return 9;  case 'B': return 11;
    default:  return -1;
    }
}

u32 synth_parse_pattern(const char* text, i8* out, u32 cap) {
    if (!text || !out || cap == 0u) return 0u;
    u32 n = 0;
    const char* p = text;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '|') p++;
        if (!*p) break;
        if (*p == '.') { out[n++] = -2; p++; continue; }
        if (*p == '-') { out[n++] = -1; p++; continue; }
        i32 b = note_base(*p);
        if (b < 0) { p++; continue; }   /* tanınmayan karakter: atla */
        p++;
        if (*p == '#') { b++; p++; }
        else if (*p == 'b') { b--; p++; }
        i32 oct = 4;
        if (*p >= '0' && *p <= '9') { oct = *p - '0'; p++; }
        i32 midi = (oct + 1) * 12 + b;
        out[n++] = (i8)CLAMP(midi, 0, 127);
    }
    return n;
}

void synth_song_start(Synth* s, f32 bpm, u32 steps_per_beat) {
    if (!s) return;
    f32 safe_bpm = clamp_safe(bpm, 1.0f, 1000.0f);
    s->song.step_seconds = 60.0f / (safe_bpm * (f32)(steps_per_beat ? steps_per_beat : 1u));
    s->song.acc          = 0.0f;
    s->song.step         = 0;
    s->song.playing      = TRUE;
}

void synth_song_stop(Synth* s) {
    if (!s) return;
    s->song.playing = FALSE;
}

b32 synth_song_add_track(Synth* s, const char* pattern, const SynthSfx* instrument) {
    if (!s || !pattern || !instrument) return FALSE;
    SynthSong* g = &s->song;
    if (g->track_count >= SYNTH_MAX_TRACKS) return FALSE;
    SynthTrack* tr = &g->tracks[g->track_count++];
    tr->length     = synth_parse_pattern(pattern, tr->notes, SYNTH_MAX_STEPS);
    tr->instrument = *instrument;
    return TRUE;
}

void synth_sfx_sea_wind(f32 app_wind_speed, f32 app_angle_rad, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    f32 spd_n = clamp_safe(app_wind_speed / 22.0f, 0.0f, 1.0f);
    f32 ang_n = 0.5f + 0.5f * m_cosf(app_angle_rad);
    out->wave          = WAVE_NOISE;
    out->freq          = 1800.0f + spd_n * 3200.0f;
    out->attack        = 0.08f;
    out->decay         = 0.10f;
    out->sustain       = 0.85f;
    out->hold          = 0.14f;
    out->release       = 0.16f;
    out->volume        = 0.04f + spd_n * 0.16f;
    out->cutoff_hz     = 220.0f + spd_n * 780.0f + ang_n * 260.0f;
    out->cutoff_end_hz = out->cutoff_hz * 0.92f;
    out->resonance     = 0.35f + spd_n * 0.35f;
}

void synth_sfx_hull_water(f32 boat_speed, b32 is_surfing, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    f32 spd_n = clamp_safe(boat_speed / 12.0f, 0.0f, 1.0f);
    out->wave          = WAVE_NOISE;
    out->freq          = is_surfing ? 4200.0f : 2600.0f;
    out->freq_end      = is_surfing ? 5200.0f : 2200.0f;
    out->attack        = 0.04f;
    out->decay         = 0.08f;
    out->sustain       = 0.75f;
    out->hold          = 0.10f;
    out->release       = 0.12f;
    out->volume        = spd_n * (is_surfing ? 0.18f : 0.10f);
    out->cutoff_hz     = 380.0f + spd_n * (is_surfing ? 1400.0f : 720.0f);
    out->cutoff_end_hz = out->cutoff_hz * (is_surfing ? 1.18f : 0.88f);
    out->resonance     = is_surfing ? 0.55f : 0.25f;
}

void synth_sfx_sail_flap(f32 luffing, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    f32 lf = clamp_safe(luffing, 0.0f, 1.0f);
    out->wave          = WAVE_NOISE;
    out->freq          = 950.0f + lf * 600.0f;
    out->freq_end      = 420.0f;
    out->attack        = 0.01f;
    out->decay         = 0.05f;
    out->sustain       = 0.30f;
    out->hold          = 0.02f;
    out->release       = 0.06f;
    out->volume        = 0.08f + lf * 0.18f;
    out->cutoff_hz     = 320.0f + lf * 280.0f;
    out->cutoff_end_hz = 140.0f;
    out->resonance     = 0.45f;
}

void synth_sfx_rope_creak(f32 heel_norm, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    f32 hn = clamp_safe(heel_norm, 0.0f, 1.0f);
    out->wave          = WAVE_SAW;
    out->freq          = 110.0f + hn * 65.0f;
    out->freq_end      = 78.0f + hn * 30.0f;
    out->attack        = 0.02f;
    out->decay         = 0.07f;
    out->sustain       = 0.40f;
    out->hold          = 0.04f;
    out->release       = 0.08f;
    out->volume        = 0.06f + hn * 0.12f;
    out->vibrato_hz    = 18.0f;
    out->vibrato_depth = 1.5f;
    out->cutoff_hz     = 280.0f;
    out->resonance     = 0.60f;
}

void synth_sfx_thunder(f32 intensity, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    f32 k = clamp_safe(intensity, 0.1f, 1.0f);
    out->wave          = WAVE_NOISE;
    out->freq          = 620.0f + k * 480.0f;
    out->freq_end      = 140.0f;
    out->attack        = 0.02f;
    out->decay         = 0.35f;
    out->sustain       = 0.55f;
    out->hold          = 0.45f;
    out->release       = 0.95f;
    out->volume        = 0.22f + k * 0.25f;
    out->cutoff_hz     = 260.0f + k * 220.0f;
    out->cutoff_end_hz = 65.0f;
    out->resonance     = 0.50f;
}

void synth_sfx_buoy_bell(b32 major_chime, SynthSfx* out) {
    if (!out) return;
    __builtin_memset(out, 0, sizeof(*out));
    out->wave          = WAVE_TRIANGLE;
    out->freq          = major_chime ? 659.25f : 523.25f; /* E5 veya C5 */
    out->freq_end      = out->freq * 0.996f;
    out->attack        = 0.008f;
    out->decay         = 0.18f;
    out->sustain       = 0.45f;
    out->hold          = 0.12f;
    out->release       = 0.55f;
    out->volume        = 0.24f;
    out->vibrato_hz    = 5.5f;
    out->vibrato_depth = 0.15f;
    out->cutoff_hz     = 1800.0f;
    out->cutoff_end_hz = 620.0f;
    out->resonance     = 0.35f;
}
