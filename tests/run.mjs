// tests/run.mjs — wasm'a derlenmiş test modülünü node'da koşar.
// Kullanım: node tests/run.mjs build/tests.wasm [--threads N]   (varsayılan 3 worker)
// Platform katmanı web/mt.mjs (paylaşılan bellek, JIT, worker'lar); çıkış kodu = başarısız test sayısı (>0 → 1).
import fs from 'node:fs';
import { loadEngine, startWorkers } from '../web/mt.mjs';

const args = process.argv.slice(2);
const ti = args.indexOf('--threads');
const threads = ti >= 0 ? +args[ti + 1] : 3;
const { state, ex, module } = await loadEngine(fs.readFileSync(args[0]), (s) => process.stdout.write(s));
const workers = await startWorkers(state, ex, module, threads);
const failed = ex.test_main();
for (const w of workers) w.terminate();
process.exit(failed ? 1 : 0);
