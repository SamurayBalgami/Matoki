// tools/bench_res.mjs — çözünürlüğün kare süresine etkisi (D35): görünüm × çözünürlük, HQ shader + MSAA, tüm thread'ler.
// Kullanım: node tools/bench_res.mjs [--frames N] [--threads N] [--res 640x360,1280x720,1920x1080]
// Her görünüm için ısınma karesinden sonra N kare engine_frame(0) ortalaması (ms); tablo markdown.
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine, startWorkers } from '../web/mt.mjs';

const args = process.argv.slice(2);
let frames = 10, threads = os.cpus().length - 1, resList = ['640x360', '1280x720', '1920x1080'];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--threads') threads = +args[++i];
  else if (args[i] === '--res') resList = args[++i].split(',');
}
const { state, ex, module } = await loadEngine(fs.readFileSync(new URL('../build/engine.wasm', import.meta.url)), () => {});
const [w0, h0] = resList[0].split('x').map(Number);
ex.engine_init(w0, h0);
const workers = await startWorkers(state, ex, module, threads);
ex.engine_set_shader_mode(4);
ex.engine_set_msaa(1);
const views = ex.engine_bench_view_count();
const rows = [];
for (const r of resList) {
  const [w, h] = r.split('x').map(Number);
  if (!ex.engine_resize(w, h)) { console.error('resize başarısız', r); continue; }
  const row = [r];
  for (let v = 1; v <= views; v++) {
    ex.engine_bench_view(v);
    ex.engine_frame(0);
    const t0 = performance.now();
    for (let f = 0; f < frames; f++) ex.engine_frame(0);
    row.push(((performance.now() - t0) / frames).toFixed(2));
  }
  rows.push(row);
}
console.log(`| çözünürlük | ${Array.from({ length: views }, (_, i) => `görünüm ${i + 1}`).join(' | ')} |`);
console.log(`|---|${'---|'.repeat(views)}`);
for (const row of rows) console.log(`| ${row.join(' | ')} |`);
console.log(`(${1 + workers.length} thread, HQ + MSAA, ${frames} kare ortalaması, ms)`);
for (const wk of workers) wk.terminate();
