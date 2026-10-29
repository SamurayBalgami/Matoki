/* audio/impact.c — fizik olaylarından vuruş sesi (audio/impact.h). Aday seçimi kararlı: eşit hızda düşük indeks
 * önce gelir, böylece aynı simülasyon aynı ses dizisini üretir. Ses tanımı: alçak geçirenli gürültü patlaması;
 * kesim frekansı hızla açılır (sert vuruş parlak), kütlenin küp köküyle düşer (ağır cisim boğuk). */
#include "audio/impact.h"
#include "core/math.h"
#include "core/mem.h"

void impact_init(ImpactAudio* ia, Arena* a, u32 cap) {
    mem_zero(ia, sizeof(*ia));
    ia->last = ARENA_ARRAY(a, f32, cap);
    ia->cap = cap;
    impact_reset(ia);
}

void impact_reset(ImpactAudio* ia) {
    for (u32 i = 0; i < ia->cap; i++) ia->last[i] = -1e9f;
}

void impact_advance(ImpactAudio* ia, f32 dt) { ia->time += dt; }

void synth_sfx_impact(f32 speed, f32 mass, SynthSfx* out) {
    mem_zero(out, sizeof(*out));
    f32 s = speed > 0.0f ? m_minf(speed / 12.0f, 1.0f) : 0.0f;   /* NaN → 0 */
    f32 m = mass > 0.05f ? m_minf(mass, 1000.0f) : 0.05f;
    f32 heavy = 1.0f / m_powf(m, 0.3333f);                         /* 1 kg → 1, 8 kg → 0,5 */
    out->wave = WAVE_NOISE;
    out->freq = 1500.0f + 3000.0f * s;
    out->attack = 0.002f;
    out->decay = 0.05f + 0.05f * s;
    out->sustain = 0.15f;
    out->hold = 0.0f;
    out->release = 0.06f + 0.10f * (1.0f - heavy * 0.5f);
    out->volume = 0.08f + 0.42f * s;
    out->cutoff_hz = m_clampf((300.0f + 2600.0f * s) * heavy, 80.0f, 6000.0f);
    out->cutoff_end_hz = out->cutoff_hz * 0.35f;
    out->resonance = 0.3f;
}

b32 impact_trigger(ImpactAudio* ia, Synth* s, u32 body, f32 speed, f32 mass) {
    if (!(speed >= IMPACT_MIN_SPEED)) return FALSE;
    if (body < ia->cap) {
        if (ia->time - ia->last[body] < IMPACT_COOLDOWN) {
            ia->skipped++;
            return FALSE;
        }
        ia->last[body] = ia->time;
    }
    SynthSfx sfx;
    synth_sfx_impact(speed, mass, &sfx);
    synth_play(s, &sfx);
    ia->played++;
    return TRUE;
}

typedef struct Cand {
    f32 speed, mass;
    u32 body;
} Cand;

#define IMPACT_MAX_CANDS 16

/* en güçlü max aday (azalan hız, eşitlikte ekleme sırası) */
static u32 cand_push(Cand* c, u32 n, u32 max, Cand x) {
    if (n == max && !(x.speed > c[n - 1].speed)) return n;
    u32 i = n < max ? n++ : n - 1;
    while (i > 0 && x.speed > c[i - 1].speed) {
        c[i] = c[i - 1];
        i--;
    }
    c[i] = x;
    return n;
}

static u32 cand_play(ImpactAudio* ia, Synth* s, const Cand* c, u32 n) {
    u32 played = 0;
    for (u32 i = 0; i < n; i++) played += impact_trigger(ia, s, c[i].body, c[i].speed, c[i].mass);
    return played;
}

u32 impact_from_phys(ImpactAudio* ia, Synth* s, const PhysWorld* w, u32 max) {
    if (w->solver != PHYS_SOLVER_SOFT || max == 0) return 0;
    if (max > IMPACT_MAX_CANDS) max = IMPACT_MAX_CANDS;
    Cand c[IMPACT_MAX_CANDS];
    u32 n = 0;
    for (u32 mi = 0; mi < w->cur_count; mi++) {
        const Manifold* m = &w->cur[mi];
        f32 v = 0.0f;
        for (u32 i = 0; i < m->count; i++) v = m_maxf(v, -m->p[i].rel_vel);
        if (!(v >= IMPACT_MIN_SPEED)) continue;
        const Body* A = &w->bodies[m->a];
        const Body* B = &w->bodies[m->b];
        /* ses hafif (daha çok hızlanan) cisimden: ters kütlesi büyük olan */
        b32 use_a = A->inv_mass >= B->inv_mass;
        f32 im = use_a ? A->inv_mass : B->inv_mass;
        if (im <= 0.0f) continue;
        Cand x = { v, 1.0f / im, use_a ? m->a : m->b };
        n = cand_push(c, n, max, x);
    }
    return cand_play(ia, s, c, n);
}

u32 impact_from_dem(ImpactAudio* ia, Synth* s, const DemWorld* d, const f32* v_before, f32 dt, u32 max) {
    if (max == 0) return 0;
    if (max > IMPACT_MAX_CANDS) max = IMPACT_MAX_CANDS;
    Cand c[IMPACT_MAX_CANDS];
    u32 n = 0;
    for (u32 i = 0; i < d->n; i++) {
        const DemBall* b = &d->b[i];
        /* çekimin dt'de katacağı hız çıkarılır; kalan değişim temas kuvvetinden (zemin ya da komşu top) */
        f32 g = (f32)d->g;
        f32 dx = (f32)b->v[0] - v_before[i * 3u];
        f32 dy = (f32)b->v[1] - v_before[i * 3u + 1u] + g * dt;
        f32 dz = (f32)b->v[2] - v_before[i * 3u + 2u];
        f32 dv = m_sqrtf(dx * dx + dy * dy + dz * dz);
        /* Δv ≈ (1 + e)·v_çarpma → yarısı yaklaşık çarpma hızı */
        Cand x = { 0.5f * dv, (f32)b->m, i };
        if (x.speed >= IMPACT_MIN_SPEED) n = cand_push(c, n, max, x);
    }
    return cand_play(ia, s, c, n);
}
