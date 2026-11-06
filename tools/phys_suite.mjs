// tools/phys_suite.mjs — fizik doğruluk düzeneği (D59): src/phys/phys_suite.c'deki zor sahneleri koşar, tablo yazar.
// Kullanım: node tools/phys_suite.mjs [engine.wasm] [--only 0,3,5] [--json çıktı.json]
// Ölçüt adları ve geçme eşikleri C'deki tablodan gelir (tek kaynak); bu araç yalnız yazdırır.
import fs from 'node:fs';
import os from 'node:os';
import { loadEngine } from '../web/mt.mjs';

const args = process.argv.slice(2);
let wasmPath = new URL('../build/engine.wasm', import.meta.url), only = null, jsonOut = null;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--only') only = args[++i].split(',').map(Number);
  else if (args[i] === '--json') jsonOut = args[++i];
  else wasmPath = args[i];
}
const { state, ex } = await loadEngine(fs.readFileSync(wasmPath), () => {});
const mem = state.memory;
const cstr = (p) => { const b = new Uint8Array(mem.buffer); let e = p; while (b[e]) e++; return new TextDecoder().decode(b.subarray(p, e)); };
const fmt = (v) => Number.isInteger(v) ? String(v) : Math.abs(v) >= 100 ? v.toFixed(1) : Math.abs(v) >= 1 ? v.toFixed(3) : v.toPrecision(3);

console.log(`CPU: ${os.cpus()[0].model}, node ${process.version}, tek thread, 60 Hz, varsayılan dünya (yumuşak çözücü, 4 alt adım), uyku kapalı\n`);
console.log('| sahne | sonuç | ölçütler | eşik | ms/adım |');
console.log('|---|---|---|---|---|');
const rows = [];
let pass = 0, judged = 0;
for (let id = 0; id < ex.phys_suite_count(); id++) {
  if (only && !only.includes(id)) continue;
  const t0 = performance.now();
  const p = ex.phys_suite_run(id);
  const wall = performance.now() - t0;
  const r = new Float64Array(mem.buffer, p, 6).slice();
  const name = cstr(ex.phys_suite_text(id, 0)), rule = cstr(ex.phys_suite_text(id, 6));
  const metrics = [];
  for (let k = 0; k < 4; k++) {
    const label = cstr(ex.phys_suite_text(id, 2 + k));
    if (label) metrics.push({ label, value: r[1 + k] });
  }
  const info = rule === 'bilgi';
  if (!info) { judged++; pass += r[0] ? 1 : 0; }
  rows.push({ id, name, what: cstr(ex.phys_suite_text(id, 1)), pass: info ? null : !!r[0], metrics, rule, ms_step: r[5], wall_ms: wall });
  console.log(`| ${name} | ${info ? 'bilgi' : r[0] ? 'GEÇTİ' : '**KALDI**'} | ${metrics.map(m => `${m.label}: ${fmt(m.value)}`).join('; ')} | ${rule} | ${r[5].toFixed(3)} |`);
}
console.log(`\n${pass}/${judged} sahne eşiği geçti`);
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(rows, null, 2));
process.exit(0);
