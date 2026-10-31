// web/audio_worklet.js — MATOKI ses çıkışı (AudioWorkletProcessor).
// Motorun paylaşılan belleğindeki halka tampondan (src/audio/audio.h AudioRing) doğrudan okur.
// Sözleşme: [0] write_pos, [1] read_pos, [2] capacity, [3] sample_rate, [4] underruns, veri @ +32.
// Mantık yok: yalnızca kopyalama + read_pos'u ilerletme (Atomics).

class MatokiAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.hdr = null;
    this.data = null;
    this.cap = 0;
    this.port.onmessage = (e) => {
      const { buffer, ptr } = e.data;
      this.hdr = new Uint32Array(buffer, ptr, 8);
      this.cap = Atomics.load(this.hdr, 2);
      this.data = new Float32Array(buffer, ptr + 32, this.cap);
    };
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.hdr) {
      for (const ch of out) ch.fill(0);
      return true;
    }
    const w = Atomics.load(this.hdr, 0);
    let r = Atomics.load(this.hdr, 1);
    const avail = (w - r) >>> 0;
    const take = Math.min(avail, n);
    const mask = this.cap - 1;
    const ch0 = out[0];
    for (let i = 0; i < take; i++) ch0[i] = this.data[(r + i) & mask];
    for (let i = take; i < n; i++) ch0[i] = 0;
    for (let c = 1; c < out.length; c++) out[c].set(ch0);
    if (take < n) Atomics.add(this.hdr, 4, n - take);
    Atomics.store(this.hdr, 1, (r + take) >>> 0);
    return true;
  }
}

registerProcessor("matoki-audio", MatokiAudio);
