/* audio/audio.h — Ses çıkışı: paylaşılan bellekte tek üretici/tek tüketici halka tampon.
 * C (ana thread) her karede synth'i hedef gecikmeye kadar render edip yazar; JS AudioWorklet
 * aynı SharedArrayBuffer'dan doğrudan okur (kopya/postMessage yok). Konumlar monoton u32 sayaçlardır.
 * JS sözleşmesi: engine_audio_init(sample_rate) → AudioRing* ; worklet read_pos'u Atomics ile ilerletir.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/audio/audio.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef AUDIO_AUDIO_H
#define AUDIO_AUDIO_H

#include "core/types.h"
#include "audio/synth.h"

#define AUDIO_RING_FRAMES 8192u   /* 2'nin kuvveti */
#define AUDIO_LATENCY_S   0.06f   /* hedef doluluk (s): 60 fps'de ~3,6 kare payı; daha uzun duraklamada kesinti */

typedef struct {
    u32 write_pos;      /* yalnız C yazar */
    u32 read_pos;       /* yalnız worklet yazar */
    u32 capacity;       /* AUDIO_RING_FRAMES */
    u32 sample_rate;
    u32 underruns;      /* worklet artırır: tampon boşken istenen kare sayısı */
    u32 _pad[3];
    f32 data[AUDIO_RING_FRAMES];
} AudioRing;

/* JS'ten ayrı alan ofsetleri (platform sözleşmesi) */
#define AUDIO_RING_OFF_WRITE 0
#define AUDIO_RING_OFF_READ  4
#define AUDIO_RING_OFF_CAP   8
#define AUDIO_RING_OFF_RATE  12
#define AUDIO_RING_OFF_UNDER 16
#define AUDIO_RING_OFF_DATA  32

/* JS export'u (testler de çağırır): halkayı sıfırlar, synth'i sample_rate'e ayarlar */
AudioRing* engine_audio_init(u32 sample_rate);

Synth* audio_synth(void);            /* ses yokken de geçerli (44.1 kHz varsayılan, çıktı atılır) */
b32    audio_is_running(void);
/* Tamponu hedef gecikmeye (saniye) kadar doldurur; render edilen kare sayısını döner */
u32    audio_pump(f32 target_latency_s);

/* Test/araç: halkayı belirli bir okuma konumuna ilerletmiş gibi davran */
AudioRing* audio_ring(void);

#endif
