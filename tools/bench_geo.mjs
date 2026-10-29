// tools/bench_geo.mjs — geometri öncesi/sonrası: build/engine_prev.wasm (önceki sürüm) ↔ build/engine.wasm,
// aynı thread sayısı, görünümler dönüşümlü. Kullanım: node tools/bench_geo.mjs [worker sayısı]
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine, startWorkers } from '../web/mt.mjs';
const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
async function open(path, threads) {
  const e = await loadEngine(fs.readFileSync(path), () => {});
  e.ex.engine_init(640, 360);
  e.workers = await startWorkers(e.state, e.ex, e.module, threads);
  return e;
}
function measure(e, v, isNew, vertMode) {
  const ex = e.ex;
  if (isNew) ex.engine_set_vert_mode(vertMode);
  ex.engine_bench_view(v);
  for (let i = 0; i < 4; i++) ex.engine_frame(0);
  const geo = [], tot = [];
  for (let i = 0; i < 15; i++) {
    const t0 = performance.now(); ex.engine_frame(0); tot.push(performance.now() - t0);
    const s = new Uint32Array(e.state.memory.buffer, ex.engine_stats(), ex.engine_stats_count());
    geo.push((isNew ? s[46] + s[47] : s[37]) / 1000);
  }
  return [med(geo), med(tot)];
}
const threads = +(process.argv[2] || (os.cpus().length - 1));
const prev = await open('build/engine_prev.wasm', threads), cur = await open('build/engine.wasm', threads);
console.log(`${threads + 1} thread, 640x360, HQ + MSAA, ms (medyan, 5 tur dönüşümlü)`);
console.log('| görünüm | önce geometri | sonra geometri (C vert) | sonra geometri (Matoki vert) | önce kare | sonra kare |');
for (let v = 1; v <= 5; v++) {
  const r = { pg: [], pt: [], cg: [], mg: [], mt: [] };
  for (let k = 0; k < 5; k++) {
    const a = measure(prev, v, false); r.pg.push(a[0]); r.pt.push(a[1]);
    const b = measure(cur, v, true, 0); r.cg.push(b[0]);
    const c = measure(cur, v, true, 1); r.mg.push(c[0]); r.mt.push(c[1]);
  }
  console.log(`| ${v} | ${med(r.pg).toFixed(2)} | ${med(r.cg).toFixed(2)} | ${med(r.mg).toFixed(2)} | ${med(r.pt).toFixed(2)} | ${med(r.mt).toFixed(2)} |`);
}
for (const v of [6]) {
  const g = [], t = [];
  for (let k = 0; k < 5; k++) { const c = measure(cur, v, true, 1); g.push(c[0]); t.push(c[1]); }
  const s = new Uint32Array(cur.state.memory.buffer, cur.ex.engine_stats(), cur.ex.engine_stats_count());
  console.log(`| 6 (göl, ${s[44]} köşe) | — | — | ${med(g).toFixed(2)} | — | ${med(t).toFixed(2)} |`);
}
[prev, cur].forEach((e) => e.workers.forEach((w) => w.terminate()));