// tools/bench_hud.mjs — motor içi panelin (gfx3d/hud, M13 faz 2) maliyeti: aynı görünümde panel kapalı ↔ açık kare süresi
// ve panelin kendi çizim süresi (ST_HUD_US). Kullanım: node tools/bench_hud.mjs [--res WxH] [--frames N] [--threads N]
import fs from 'node:fs';
import { loadEngine, startWorkers } from '../web/mt.mjs';
const args = process.argv.slice(2);
const arg = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const [W, H] = arg('--res', '1280x720').split('x').map(Number);
const FRAMES = +arg('--frames', 60);
const { state, ex, module } = await loadEngine(fs.readFileSync(new URL('../build/engine.wasm', import.meta.url)), () => {});
ex.engine_init(W, H);
await startWorkers(state, ex, module, +arg('--threads', 0));
const n = ex.engine_stats_count();
const stats = () => new Uint32Array(ex.memory ? ex.memory.buffer : state.memory.buffer, ex.engine_stats(), n);
const ST_TOTAL_US = 39, ST_HUD_US = n - 1;
function run(hud) {
  ex.engine_set_hud(hud);
  ex.engine_bench_view(1);
  for (let i = 0; i < 10; i++) ex.engine_frame(16.7);
  let tot = 0, hudUs = 0;
  const t0 = performance.now();
  for (let i = 0; i < FRAMES; i++) { ex.engine_frame(16.7); const s = stats(); tot += s[ST_TOTAL_US]; hudUs += s[ST_HUD_US]; }
  return { wall: (performance.now() - t0) / FRAMES, total: tot / FRAMES / 1000, hud: hudUs / FRAMES / 1000 };
}
const off = run(0), on = run(1), off2 = run(0);
console.log(`${W}x${H}, ${FRAMES} kare, görünüm 1`);
console.log(`panel kapalı: duvar ${off.wall.toFixed(2)} ms, motor ${off.total.toFixed(2)} ms (tekrar: ${off2.wall.toFixed(2)} ms)`);
console.log(`panel açık  : duvar ${on.wall.toFixed(2)} ms, motor ${on.total.toFixed(2)} ms, panel çizimi ${on.hud.toFixed(3)} ms`);
process.exit(0);
