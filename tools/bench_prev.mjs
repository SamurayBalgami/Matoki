// tools/bench_prev.mjs — önceki sürüm (build/engine_prev.wasm) ↔ bu sürüm kare süresi, görünümler dönüşümlü,
// tüm thread'ler; son sütun gökyüzü kubbesi kapalı. Kullanım: node tools/bench_prev.mjs
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine, startWorkers } from '../web/mt.mjs';
const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
async function open(path) {
  const e = await loadEngine(fs.readFileSync(path), () => {});
  e.ex.engine_init(640, 360);
  e.workers = await startWorkers(e.state, e.ex, e.module, os.cpus().length - 1);
  return e;
}
function frame(e, v, sky = 1) {
  if (e.ex.engine_set_sky) e.ex.engine_set_sky(sky);
  e.ex.engine_bench_view(v);
  for (let i = 0; i < 4; i++) e.ex.engine_frame(0);
  const t = [];
  for (let i = 0; i < 12; i++) { const t0 = performance.now(); e.ex.engine_frame(0); t.push(performance.now() - t0); }
  return med(t);
}
const a = await open('build/engine_prev.wasm'), b = await open('build/engine.wasm');
console.log('| görünüm | önce ms | sonra ms | oran | sonra, gök yok ms |');
for (let v = 1; v <= 6; v++) {
  const x = [], y = [], z = [];
  for (let k = 0; k < 7; k++) { x.push(frame(a, v)); y.push(frame(b, v)); z.push(frame(b, v, 0)); }
  console.log(`| ${v} | ${med(x).toFixed(2)} | ${med(y).toFixed(2)} | ${(med(y) / med(x)).toFixed(2)} | ${med(z).toFixed(2)} |`);
}
[a, b].forEach((e) => e.workers.forEach((w) => w.terminate()));