// tools/bench.mjs — rasterizer/boru hattı ölçümü (node; --threads N worker, --mt çok thread karşılaştırması).
// Kullanım: node tools/bench.mjs [engine.wasm] [--frames N] [--res 640x360] [--threads N] [--shade | --pair | --mt]
// Her sabit görünümde (engine_bench_view) ısınma + N kare; kare başına ms (medyan/en iyi)
// ve framebuffer FNV-1a hash'i. Motor engine_set_raster_mode export'u veriyorsa her mod ayrı ölçülür.
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine, startWorkers } from '../web/mt.mjs';

const args = process.argv.slice(2);
let wasmPath = new URL('../build/engine.wasm', import.meta.url);
let frames = 60, W = 640, H = 360;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--res') [W, H] = args[++i].split('x').map(Number);
  else if (args[i] === '--threads') i++;
  else if (!args[i].startsWith('--')) wasmPath = args[i];
}

const ti = args.indexOf('--threads');
const WORKERS = ti >= 0 ? +args[ti + 1] : 0;   // varsayılan tek thread (eski ölçümlerle karşılaştırılabilir)
const { state, ex, module } = await loadEngine(fs.readFileSync(wasmPath), (t) => process.stdout.write(t));
const mem = state.memory;
if (!ex.engine_init(W, H)) throw new Error('engine_init');
const workers = await startWorkers(state, ex, module, args.includes('--mt') && ti < 0 ? os.cpus().length - 1 : WORKERS);
process.on('exit', () => workers.forEach((w) => w.terminate()));
function fnv(u32) {
  let h = 0x811c9dc5;
  for (let i = 0; i < u32.length; i++) { h ^= u32[i]; h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, '0');
}

// demo.c RasterMode sırası
const MODE_NAMES = ['skaler', 'simd'];
const RASTER_MODES = ex.engine_set_raster_mode ? MODE_NAMES.slice(0, ex.engine_raster_mode_count()) : ['önceki (anlık, skaler)'];
console.log(`CPU: ${os.cpus()[0].model}, node ${process.version}, ${W}x${H}, ${frames} kare/ölçüm, wasm ${String(wasmPath).split(/[\\/]/).pop()}`);
// demo.c SHADER_* sırası; lit satırları her shader modu için en hızlı raster moduyla ölçülür
const SHADER_NAMES = ['lit ref', 'lit lut', 'lit simd', 'lit matoki'];
const MATOKI = 3;
const SHADER_MODES = ex.engine_set_shader_mode ? SHADER_NAMES.slice(0, ex.engine_shader_mode_count()) : ['lit'];
const hasMatoki = ex.engine_matoki_ready && ex.engine_matoki_ready();

function measure(v, flat, rasterMode, shaderMode, mip = 1, n = frames) {
  if (ex.engine_set_raster_mode) ex.engine_set_raster_mode(rasterMode);
  if (ex.engine_set_shader_mode) ex.engine_set_shader_mode(shaderMode);
  ex.engine_set_flat(flat);
  ex.engine_bench_view(v);
  if (ex.engine_set_mip) ex.engine_set_mip(mip);
  for (let i = 0; i < 8; i++) ex.engine_frame(0);
  const t = [];
  for (let i = 0; i < n; i++) {
    const t0 = performance.now();
    ex.engine_frame(0);
    t.push(performance.now() - t0);
  }
  t.sort((a, b) => a - b);
  const hash = fnv(new Uint32Array(mem.buffer, ex.engine_framebuffer(), W * H));
  return [t[t.length >> 1].toFixed(3), t[0].toFixed(3), hash];
}

// --shade: yalnızca shader mikro-ölçümü (quad başına ns). Modlar dönüşümlü ölçülür (31 tur × 50 tekrar),
// böylece frekans / çekirdek değişimi hepsine eşit dağılır; en iyi ve medyan.
if (args.includes('--shade')) {
  console.log('| shader | en iyi ns/quad | medyan ns/quad | çıktı hash |');
  console.log('|---|---|---|---|');
  const modes = [];
  for (let s = 0; s < SHADER_MODES.length; s++) if (s !== MATOKI || hasMatoki) modes.push(s);
  const res = modes.map(() => []), hashes = [];
  for (const s of modes) { ex.engine_shade_bench(s, 20); hashes.push((ex.engine_shade_bench_hash() >>> 0).toString(16).padStart(8, '0')); }
  const ROUNDS = 61, SIMD = 2;
  const ratios = modes.map(() => []);
  for (let k = 0; k < ROUNDS; k++) {
    const t = modes.map((s) => ex.engine_shade_bench(s, 40));
    modes.forEach((s, i) => { res[i].push(t[i]); ratios[i].push(t[i] / t[modes.indexOf(SIMD)]); });
  }
  const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
  console.log('(oran = aynı turda C SIMD\'ye göre süre; turların medyanı — dış yükten en az etkilenen ölçü)');
  modes.forEach((s, i) => {
    const r = [...res[i]].sort((a, b) => a - b);
    console.log(`| ${SHADER_MODES[s]} | ${r[0].toFixed(2)} | ${med(r).toFixed(2)} | ${hashes[i]} | oran ${med(ratios[i]).toFixed(3)} |`);
  });
  process.exit(0);
}

// --mt: her görünümde tek thread ↔ tüm thread'ler (dönüşümlü 9 tur), kare medyanı, hızlanma, hash eşitliği
if (args.includes('--mt')) {
  const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
  const n = 1 + workers.length;
  console.log(`| görünüm | 1 thread ms | ${n} thread ms | hızlanma | hash eşit |`);
  console.log('|---|---|---|---|---|');
  const sm = hasMatoki ? MATOKI : 2;
  for (let v = 1; v <= ex.engine_bench_view_count(); v++) {
    const a = [], b = [];
    let ha = '', hb = '';
    for (let k = 0; k < 9; k++) {
      ex.engine_set_threads(1); const x = measure(v, 0, RASTER_MODES.length - 1, sm, 1, 20);
      ex.engine_set_threads(0); const y = measure(v, 0, RASTER_MODES.length - 1, sm, 1, 20);
      a.push(+x[0]); b.push(+y[0]); ha = x[2]; hb = y[2];
    }
    console.log(`| ${v} | ${med(a).toFixed(3)} | ${med(b).toFixed(3)} | ${(med(a) / med(b)).toFixed(2)}× | ${ha === hb ? 'evet' : 'HAYIR'} |`);
  }
  process.exit(0);
}
// --replay: her görünümün gerçek quad'larını yakala, yalnızca shader'ları (C SIMD ↔ Matoki) eşli ölç
if (args.includes('--replay') && hasMatoki) {
  console.log('| görünüm | yakalanan quad | C SIMD ns/quad | Matoki ns/quad | Matoki / C oranı | hash eşit |');
  console.log('|---|---|---|---|---|---|');
  const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
  for (let v = 1; v <= ex.engine_bench_view_count(); v++) {
    const n = ex.engine_capture_view(v);
    ex.engine_replay_bench(2, 2); const hs = ex.engine_shade_bench_hash();
    ex.engine_replay_bench(MATOKI, 2); const hm = ex.engine_shade_bench_hash();
    const pre = ex.engine_replay_preludes();
    const a = [], b = [], r = [], rf = [];
    for (let k = 0; k < 41; k++) {
      const x = ex.engine_replay_bench(2, 5), y = ex.engine_replay_bench(MATOKI, 5);
      const z = ex.engine_replay_bench(SHADER_MODES.length, 5);   // Matoki tam gövde, önsöz yok
      a.push(x); b.push(y); r.push(y / x); rf.push(z / x);
    }
    console.log(`| ${v} | ${n} | ${med(a).toFixed(2)} | ${med(b).toFixed(2)} | ${med(r).toFixed(3)} | ${hs === hm ? 'evet' : 'HAYIR'} | önsöz ${pre}, tam gövde oranı ${med(rf).toFixed(3)} |`);
  }
  process.exit(0);
}

// --pair: her görünümde C SIMD ve Matoki dönüşümlü (15 tur × 20 kare); turdaki medyanların oranı → oranların medyanı.
// Dış yük (başka uygulamalar) iki tarafa eşit dağıldığı için gürültülü makinede tek güvenilir karşılaştırma.
if (args.includes('--pair') && hasMatoki) {
  console.log('| görünüm | C SIMD medyan ms | Matoki medyan ms | Matoki / C oranı | hash eşit |');
  console.log('|---|---|---|---|---|');
  const med = (a) => [...a].sort((x, y) => x - y)[a.length >> 1];
  const bestRaster = RASTER_MODES.length - 1;
  const nv = ex.engine_bench_view_count();
  for (let v = 1; v <= nv; v++) {
    const rs = [], ts = [], tm = [];
    let hs = '', hm = '';
    for (let k = 0; k < 15; k++) {
      const a = measure(v, 0, bestRaster, 2, 1, 20);
      const b = measure(v, 0, bestRaster, MATOKI, 1, 20);
      ts.push(+a[0]); tm.push(+b[0]); rs.push(+b[0] / +a[0]);
      hs = a[2]; hm = b[2];
    }
    // demo.c ST_*: 13 = quad, 14 = piksel, 22 = bin öğesi, 40 = önsöz çağrısı (son Matoki karesi)
    const st = new Uint32Array(mem.buffer, ex.engine_stats(), ex.engine_stats_count());
    console.log(`| ${v} | ${med(ts).toFixed(3)} | ${med(tm).toFixed(3)} | ${med(rs).toFixed(3)} | ${hs === hm ? 'evet' : 'HAYIR'} | quad ${st[13]} bin ${st[21]} önsöz ${st[40]} |`);
  }
  process.exit(0);
}

console.log('| görünüm | shader | raster modu | medyan ms | en iyi ms | hash |');
console.log('|---|---|---|---|---|---|');
const nViews = ex.engine_bench_view_count();
const bestRaster = RASTER_MODES.length - 1;
for (let v = 1; v <= nViews; v++) {
  for (let m = 0; m < RASTER_MODES.length; m++) {
    const [med, best, hash] = measure(v, 1, m, 0);
    console.log(`| ${v} | flat | ${RASTER_MODES[m]} | ${med} | ${best} | ${hash} |`);
  }
  // lit.mtk mip'li ve mip'siz C ile bit-bit aynı: matoki hash'i C satırlarıyla aynı olmalı
  for (let s = 0; s < SHADER_MODES.length; s++) {
    if (s === MATOKI && !hasMatoki) continue;
    const [med, best, hash] = measure(v, 0, bestRaster, s);
    console.log(`| ${v} | ${SHADER_MODES[s]} | ${RASTER_MODES[bestRaster]} | ${med} | ${best} | ${hash} |`);
  }
  if (hasMatoki) {
    for (const s of [2, MATOKI]) {
      const [med, best, hash] = measure(v, 0, bestRaster, s, 0);
      console.log(`| ${v} | ${SHADER_MODES[s]} (mip yok) | ${RASTER_MODES[bestRaster]} | ${med} | ${best} | ${hash} |`);
    }
  }
}
