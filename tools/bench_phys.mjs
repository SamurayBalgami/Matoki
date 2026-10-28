// tools/bench_phys.mjs — fizik motoru ölçümü (node, tek thread).
// Kullanım: node tools/bench_phys.mjs [engine.wasm] [--n 100,500,2000] [--solvers 0,1] [--sleep 0,1]
// Her koşu 600 adım; sahne 0 = düşen yığın, sahne 1 = piramit duvar. src/phys/phys_bench.c ile eşleşir.
import fs from 'node:fs';
import os from 'node:os';

const args = process.argv.slice(2);
let wasmPath = new URL('../build/engine.wasm', import.meta.url);
let ns = [100, 500, 2000], solvers = [0], sleeps = [0], scenes = [0, 1], substeps = 0, iterations = 0, ccd = 0, order = 0, bp = 0, rays = 0;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--n') ns = args[++i].split(',').map(Number);
  else if (args[i] === '--scenes') scenes = args[++i].split(',').map(Number);
  else if (args[i] === '--substeps') substeps = +args[++i];
  else if (args[i] === '--iterations') iterations = +args[++i];
  else if (args[i] === '--solvers') solvers = args[++i].split(',').map(Number);
  else if (args[i] === '--sleep') sleeps = args[++i].split(',').map(Number);
  else if (args[i] === '--ccd') ccd = +args[++i] ? 1 : 2;
  else if (args[i] === '--order') order = +args[++i];   /* 1 sıralı, 2 renk skaler, 3 renk SIMD */
  else if (args[i] === '--bp') bp = args[++i] === 'sap' ? 1 : 2;   /* sap | bvh */
  else if (args[i] === '--rays') rays = +args[++i];      /* koşu sonunda N ışın */
  else wasmPath = args[i];
}

let mem = null, table = null;
const dec = new TextDecoder();
const env = {
  plat_log(p, n) { process.stdout.write(dec.decode(new Uint8Array(mem.buffer, p, n))); },
  plat_now_ms() { return performance.now(); },
  plat_jit_instantiate(p, n) {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(mem.buffer, p, n).slice()), { env: { memory: mem } });
    // shade, prelude, shade_fast (olanlar) ardışık indekslere — jit_wasm.c bu sırayı varsayar
    const fns = ['shade', 'prelude', 'shade_fast'].map((k) => inst.exports[k]).filter(Boolean);
    const idx = table.grow(fns.length);
    fns.forEach((fn, k) => table.set(idx + k, fn));
    return idx;
  },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { env });
const ex = instance.exports;
mem = ex.memory;
table = ex.__indirect_function_table;

const SOLVER = ['SI', 'soft'];
const SCENE = ['yığın', 'piramit', 'duvar', 'şehir'];
console.log(`CPU: ${os.cpus()[0].model}, node ${process.version}, wasm ${String(wasmPath).split(/[\\/]/).pop()}`);
console.log('| sahne | n | çözücü | uyku | ms aktif | ms oturmuş | çarpışma ms | çözücü ms | temas | uyanık | KE (J) | en büyük yer değ. (m) | en büyük girişim (m) | hash | geniş faz ms | ışın ms/1000 |');
console.log('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|');
if (ex.phys_bench_config) ex.phys_bench_config(substeps, iterations, ccd, order, bp);
if (bp) console.log(`(geniş faz: ${bp === 1 ? 'SAP + doğrusal sorgular' : 'BVH'})`);
if (order) console.log(`(çözüm sırası: ${['', 'oluşturulma (sıralı)', 'renk, skaler', 'renk, SIMD'][order]})`);
if (substeps || iterations) console.log(`(alt adım ${substeps || 4}, SI iterasyon ${iterations || 10})`);
if (ccd) console.log(`(CCD ${ccd === 1 ? 'açık' : 'kapalı'})`);
for (const scene of scenes) {
  for (const n of ns) {
    for (const solver of solvers) {
      for (const sleep of sleeps) {
        const p = ex.phys_bench_run(scene, n, solver, sleep);
        const r = new Float64Array(mem.buffer, p, 11).slice();
        let ray = '—';
        if (rays && ex.phys_bench_rays) {
          const q = new Float64Array(mem.buffer, ex.phys_bench_rays(rays), 3);
          ray = `${(q[0] * 1000 / rays).toFixed(3)} (isabet ${q[1]}, Σt ${q[2].toFixed(1)})`;
        }
        const broad = r.length > 10 && !Number.isNaN(r[10]) ? r[10].toFixed(3) : '—';
        console.log(`| ${SCENE[scene]} | ${n} | ${SOLVER[solver]} | ${sleep ? 'açık' : 'kapalı'} | ${r[0].toFixed(3)} | ${r[1].toFixed(3)} | ` +
          `${r[2].toFixed(3)} | ${r[3].toFixed(3)} | ${r[4]} | ${r[5]} | ${r[6].toFixed(4)} | ${r[7].toFixed(3)} | ${r[8].toFixed(4)} | ${(r[9] >>> 0).toString(16)} | ${broad} | ${ray} |`);
      }
    }
  }
}
