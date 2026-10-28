// tools/bench_quality.mjs — kalite ayarlarının maliyeti: Matoki / HQ × MSAA yok / 4x, tüm thread'ler.
// Kullanım: node tools/bench_quality.mjs [640x360]   (bench görünümleri, kare medyanı ms)
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine, startWorkers } from '../web/mt.mjs';
const [W, H] = (process.argv[2] || '640x360').split('x').map(Number);
const { state, ex, module } = await loadEngine(fs.readFileSync('build/engine.wasm'), () => {});
ex.engine_init(W, H);
const workers = await startWorkers(state, ex, module, os.cpus().length - 1);
const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
const cfg = [['Matoki, MSAA yok', 3, 0], ['HQ, MSAA yok', 4, 0], ['Matoki, MSAA 4x', 3, 1], ['HQ + MSAA 4x', 4, 1]];
function run(v, mode, msaa) {
  ex.engine_set_shader_mode(mode); ex.engine_set_msaa(msaa); ex.engine_bench_view(v);
  for (let i = 0; i < 5; i++) ex.engine_frame(0);
  const t = []; for (let i = 0; i < 15; i++) { const t0 = performance.now(); ex.engine_frame(0); t.push(performance.now() - t0); }
  return med(t);
}
console.log(`${W}x${H}, ${1 + workers.length} thread, kare medyanı ms`);
console.log('| görünüm | ' + cfg.map((c) => c[0]).join(' | ') + ' |');
for (let v = 1; v <= ex.engine_bench_view_count(); v++) {
  const r = cfg.map(() => []);
  for (let k = 0; k < 7; k++) cfg.forEach((c, i) => r[i].push(run(v, c[1], c[2])));
  console.log(`| ${v} | ` + r.map((x) => med(x).toFixed(2)).join(' | ') + ' |');
}
workers.forEach((w) => w.terminate());