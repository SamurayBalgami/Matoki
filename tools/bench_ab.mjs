// tools/bench_ab.mjs — iki motor derlemesini (A = önce, B = sonra) aynı süreçte dönüşümlü ölçer: her görünümde
// R tur × F kare, turlar A/B sırasıyla iç içe → makinedeki gürültü iki tarafa eşit dağılır. Çıktı: görünüm başına
// medyan ms, B/A oranı (turların oran medyanı), en iyi sürelerin oranı (dış yük tek yönlü → daha az gürültü)
// ve framebuffer hash eşitliği. Tek thread (worker yok). Bu makinede A/A gürültüsü ±%2–4 ölçüldü.
// Kullanım: node tools/bench_ab.mjs once.wasm sonra.wasm [--res 640x360] [--rounds 9] [--frames 8]
import fs from 'node:fs';
import { loadEngine } from '../web/mt.mjs';
const args = process.argv.slice(2);
const arg = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const [W, H] = arg('--res', '640x360').split('x').map(Number);
const ROUNDS = +arg('--rounds', 9), FRAMES = +arg('--frames', 8);
async function load(path) {
  const { state, ex } = await loadEngine(fs.readFileSync(path), () => {});
  ex.engine_init(W, H);
  const mem = () => (ex.memory || state.memory).buffer;
  return { ex, mem };
}
const A = await load(args[0]), B = await load(args[1]);
function hash(e) {
  const px = new Uint32Array(e.mem(), e.ex.engine_framebuffer(), W * H);
  let h = 0x811c9dc5;
  for (let i = 0; i < px.length; i++) { h ^= px[i]; h = Math.imul(h, 0x01000193) >>> 0; }
  return h;
}
function time(e, v) {
  e.ex.engine_bench_view(v);
  const t0 = performance.now();
  for (let i = 0; i < FRAMES; i++) e.ex.engine_frame(0);
  return (performance.now() - t0) / FRAMES;
}
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
console.log(`| görünüm | A ms | B ms | B/A | hash |`);
console.log(`|---|---|---|---|---|`);
const ratios = [], minRatios = [];
for (let v = 1; v <= A.ex.engine_bench_view_count(); v++) {
  time(A, v); time(B, v);   // ısınma
  const ta = [], tb = [], r = [];
  for (let k = 0; k < ROUNDS; k++) {
    const a = k & 1 ? (tb.push(time(B, v)), time(A, v)) : time(A, v);
    if (!(k & 1)) tb.push(time(B, v));
    ta.push(a);
    r.push(tb[tb.length - 1] / a);
  }
  A.ex.engine_bench_view(v); A.ex.engine_frame(0);
  B.ex.engine_bench_view(v); B.ex.engine_frame(0);
  const same = hash(A) === hash(B);
  ratios.push(med(r));
  minRatios.push(Math.min(...tb) / Math.min(...ta));
  console.log(`| ${v} | ${med(ta).toFixed(2)} | ${med(tb).toFixed(2)} | ${med(r).toFixed(3)} | ${same ? 'aynı' : 'FARKLI'} |`);
}
console.log(`oranların medyanı (B/A): ${med(ratios).toFixed(3)}   en iyi sürelerin oranı (B/A, medyan): ${med(minRatios).toFixed(3)}`);
process.exit(0);
