/* audio/impact.h — çarpışma sesi: fizik olaylarından (yaklaşma hızı, kütle) kısa vuruş sesi üretir (M13 faz 4).
 * Cisim başına bekleme süresi, aynı çarpışmanın spekülatif ve gerçek temas adımlarında iki kez çalmasını önler.
 * Yalnızca ses çıkışına yazar; simülasyona hiçbir şey dönmez → fizik durum hash'leri ve determinizm etkilenmez.
 * Rijit cisimler: soft çözücünün adım başı normal yaklaşma hızı (ContactPoint.rel_vel); DEM: hız sıçraması. */
#ifndef AUDIO_IMPACT_H
#define AUDIO_IMPACT_H
#include "core/arena.h"
#include "audio/synth.h"
#include "phys/phys.h"
#include "phys/dem.h"

#define IMPACT_MIN_SPEED 1.2f    /* m/s: altı ses çıkarmaz (duran / kayan temaslar) */
#define IMPACT_COOLDOWN  0.12f   /* s: cisim başına */

typedef struct ImpactAudio {
    f32* last;          /* cisim başına son ses zamanı */
    u32  cap;
    f32  time;
    u32  played;        /* toplam çalınan */
    u32  skipped;       /* beklemede olduğu için atlanan */
} ImpactAudio;

void impact_init(ImpactAudio* ia, Arena* a, u32 cap);
void impact_reset(ImpactAudio* ia);   /* sahne değişiminde: bekleme tablosu sıfırlanır */
void impact_advance(ImpactAudio* ia, f32 dt);

/* hız (m/s) ve kütleden (kg) vuruş: hız sesi ve parlaklığı, kütle perdeyi belirler (ağır → pes) */
void synth_sfx_impact(f32 speed, f32 mass, SynthSfx* out);
/* eşik altında ya da cisim beklemedeyse FALSE; body >= cap ise bekleme yok */
b32  impact_trigger(ImpactAudio* ia, Synth* s, u32 body, f32 speed, f32 mass);

/* Bu adımın rijit cisim çarpışmaları: manifold başına en büyük yaklaşma hızı, en güçlü max tanesi çalınır.
 * Soft çözücü gerekir (SI çözücüsü rel_vel yazmaz → 0 döner). Dönüş = çalınan. */
u32  impact_from_phys(ImpactAudio* ia, Synth* s, const PhysWorld* w, u32 max);
/* DEM adımı öncesi hızlar (3n f32) → sonrası: çekim dışı hız değişimi |Δv − g·dt| darbe sayılır */
u32  impact_from_dem(ImpactAudio* ia, Synth* s, const DemWorld* d, const f32* v_before, f32 dt, u32 max);

#endif
