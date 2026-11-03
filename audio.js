// web/audio.js — Ses platform katmanı: ilk kullanıcı etkileşiminde AudioContext + AudioWorklet kurar,
// motorun engine_audio_init(sample_rate) export'unu çağırıp dönen halka tampon adresini worklet'e verir.
// Ses üretimi tamamen C'de (src/audio/); burada yalnızca kurulum var.

export function installAudio(exports, memory) {
  if (typeof exports.engine_audio_init !== "function") return;
  if (!(memory.buffer instanceof SharedArrayBuffer)) {
    console.warn("[ses] SharedArrayBuffer yok, ses kapalı");
    return;
  }
  let started = false;
  const start = async () => {
    if (started) return;
    started = true;
    try {
      const ctx = new AudioContext({ latencyHint: "interactive" });
      await ctx.audioWorklet.addModule("audio_worklet.js");
      const ptr = exports.engine_audio_init(ctx.sampleRate >>> 0);
      const node = new AudioWorkletNode(ctx, "matoki-audio", { outputChannelCount: [2] });
      node.port.postMessage({ buffer: memory.buffer, ptr });
      node.connect(ctx.destination);
      if (ctx.state !== "running") await ctx.resume();
      // ölçüm/araç: halka başlığı = new Uint32Array(memory.buffer, audioRing, 8) (yazma, okuma, kapasite, hız, kesinti)
      globalThis.matoki = Object.assign(globalThis.matoki || {}, { audio: ctx, audioRing: ptr, memory });
    } catch (err) {
      console.warn("[ses] başlatılamadı:", err);
    }
  };
  // Tarayıcılar sesi ancak kullanıcı etkileşiminden sonra başlatır
  window.addEventListener("keydown", start, { once: true });
  window.addEventListener("mousedown", start, { once: true });
}
