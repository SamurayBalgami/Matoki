// tools/bench.mjs — rasterizer/boru hattı ölçümü (node, tek thread).
// Kullanım: node tools/bench.mjs [engine.wasm] [--frames N] [--res 640x360]
// Her sabit görünümde (engine_bench_view) ısınma + N kare; kare başına ms (medyan/en iyi)
// ve framebuffer FNV-1a hash'i. Motor engine_set_raster_mode export'u veriyorsa her mod ayrı ölçülür.
import fs from 'node:fs';
import os from 'node:os';

const args = process.argv.slice(2);
let wasmPath = new URL('../build/engine.wasm', import.meta.url);
let frames = 60, W = 640, H = 360;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--res') [W, H] = args[++i].split('x').map(Number);
  else wasmPath = args[i];
}

const dec = new TextDecoder();
let mem = null;
let table = null;
const env = {
  plat_log(p, n) { process.stdout.write(dec.decode(new Uint8Array(mem.buffer, p, n))); },
  plat_now_ms() { return performance.now(); },
  plat_jit_instantiate(p, n) {
    const code = new Uint8Array(mem.buffer, p, n).slice();
    const inst = new WebAssembly.Instance(new WebAssembly.Module(code), { env: { memory: mem } });
    const idx = table.grow(1);
    table.set(idx, inst.exports.shade);
    return idx;
  },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { env });
const ex = instance.exports;
mem = ex.memory;
table = ex.__indirect_function_table;
if (!ex.engine_init(W, H)) throw new Error('engine_init');

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

function measure(v, flat, rasterMode, shaderMode, mip = 1) {
  if (ex.engine_set_raster_mode) ex.engine_set_raster_mode(rasterMode);
  if (ex.engine_set_shader_mode) ex.engine_set_shader_mode(shaderMode);
  ex.engine_set_flat(flat);
  ex.engine_bench_view(v);
  if (ex.engine_set_mip) ex.engine_set_mip(mip);
  for (let i = 0; i < 8; i++) ex.engine_frame(0);
  const t = [];
  for (let i = 0; i < frames; i++) {
    const t0 = performance.now();
    ex.engine_frame(0);
    t.push(performance.now() - t0);
  }
  t.sort((a, b) => a - b);
  const hash = fnv(new Uint32Array(mem.buffer, ex.engine_framebuffer(), W * H));
  return [t[t.length >> 1].toFixed(3), t[0].toFixed(3), hash];
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
  for (let s = 0; s < SHADER_MODES.length; s++) {
    if (s === MATOKI) continue;
    const [med, best, hash] = measure(v, 0, bestRaster, s);
    console.log(`| ${v} | ${SHADER_MODES[s]} | ${RASTER_MODES[bestRaster]} | ${med} | ${best} | ${hash} |`);
  }
  // Matoki lit.mtk henüz mip'siz: C SIMD ile mip kapalıyken karşılaştır (hash'ler aynı olmalı)
  if (hasMatoki) {
    for (const s of [2, MATOKI]) {
      const [med, best, hash] = measure(v, 0, bestRaster, s, 0);
      console.log(`| ${v} | ${SHADER_MODES[s]} (mip yok) | ${RASTER_MODES[bestRaster]} | ${med} | ${best} | ${hash} |`);
    }
  }
}
