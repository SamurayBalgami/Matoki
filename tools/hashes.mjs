// tools/hashes.mjs — bench görünümlerinin framebuffer hash'leri (görünüm × shader × MSAA); regresyon karşılaştırması.
// Kullanım: node tools/hashes.mjs [--threads N] > dosya
import fs from 'node:fs';
import { loadEngine, startWorkers } from '../web/mt.mjs';
const args = process.argv.slice(2);
const ti = args.indexOf('--threads');
const { state, ex, module } = await loadEngine(fs.readFileSync(new URL('../build/engine.wasm', import.meta.url)), () => {});
ex.engine_init(640, 360);
const workers = await startWorkers(state, ex, module, ti >= 0 ? +args[ti + 1] : 0);
function fnv(u32) { let h = 0x811c9dc5; for (let i = 0; i < u32.length; i++) { h ^= u32[i]; h = Math.imul(h, 0x01000193) >>> 0; } return h.toString(16).padStart(8, '0'); }
for (let v = 1; v <= ex.engine_bench_view_count(); v++) {
  const row = [];
  for (const mode of [2, 3, 4]) for (const msaa of [0, 1]) {
    ex.engine_set_shader_mode(mode); ex.engine_set_msaa(msaa); ex.engine_bench_view(v);
    ex.engine_frame(0); ex.engine_frame(0);
    row.push(`s${mode}m${msaa}=${fnv(new Uint32Array(state.memory.buffer, ex.engine_framebuffer(), 640 * 360))}`);
  }
  console.log(`görünüm ${v}: ${row.join(' ')}`);
}
workers.forEach((w) => w.terminate());