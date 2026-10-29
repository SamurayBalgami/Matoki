/* audio/audio.c — Halka tampon doldurma ve JS'e açılan engine_audio_init export'u.
 * Doluluk = write_pos - read_pos (u32 taşması güvenli). Hedef doluluğa kadar synth render edilir;
 * en fazla capacity kadar ileri yazılır, yani worklet'in okumadığı veri asla ezilmez.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/audio/audio.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "audio/audio.h"
#include "core/math.h"
#include "core/fmt.h"

static AudioRing g_ring;
static Synth     g_synth;
static b32       g_synth_ready;
static b32       g_running;

_Static_assert(__builtin_offsetof(AudioRing, data) == AUDIO_RING_OFF_DATA, "audio ring abi");
_Static_assert(__builtin_offsetof(AudioRing, underruns) == AUDIO_RING_OFF_UNDER, "audio ring abi");

Synth* audio_synth(void) {
    if (!g_synth_ready) {
        synth_init(&g_synth, 44100u);
        g_synth_ready = TRUE;
    }
    return &g_synth;
}

b32 audio_is_running(void) {
    return g_running;
}

AudioRing* audio_ring(void) {
    return &g_ring;
}

EXPORT(engine_audio_init) AudioRing* engine_audio_init(u32 sample_rate) {
    Synth* s = audio_synth();
    if (sample_rate != s->sample_rate) {
        /* Çalan sesleri yeni örnekleme hızına taşımak yerine şarkı durumu korunur, sesler susturulur */
        SynthSong song = s->song;
        f32 master = s->master;
        synth_init(s, sample_rate);
        s->song = song;
        s->master = master;
    }
    __builtin_memset(&g_ring, 0, sizeof(g_ring));
    g_ring.capacity    = AUDIO_RING_FRAMES;
    g_ring.sample_rate = s->sample_rate;
    g_running = TRUE;
    /* ön doldurma: worklet bağlanır bağlanmaz okur; ilk motor karesine dek halka boş kalırsa kesinti sayılırdı */
    audio_pump(AUDIO_LATENCY_S);
    LOG("Ses: %u Hz, halka %u kare", s->sample_rate, AUDIO_RING_FRAMES);
    return &g_ring;
}

u32 audio_pump(f32 target_latency_s) {
    Synth* s = audio_synth();
    if (!g_running) return 0;
    u32 w = g_ring.write_pos;
    u32 r = __atomic_load_n(&g_ring.read_pos, __ATOMIC_ACQUIRE);
    u32 fill = w - r;
    if (fill > g_ring.capacity) fill = g_ring.capacity;   /* worklet'in ileri gittiği durumda bile güvenli */
    u32 target = (u32)(target_latency_s * (f32)s->sample_rate);
    if (target > g_ring.capacity) target = g_ring.capacity;
    if (fill >= target) return 0;
    u32 want = target - fill;
    u32 done = 0;
    while (done < want) {
        u32 idx = (w + done) & (g_ring.capacity - 1u);
        u32 run = MIN(want - done, g_ring.capacity - idx);
        synth_render(s, &g_ring.data[idx], run);
        done += run;
    }
    __atomic_store_n(&g_ring.write_pos, w + done, __ATOMIC_RELEASE);
    return done;
}
