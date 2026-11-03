// web/worker.mjs — iş sistemi worker'ı (tarayıcı Worker ya da node worker_threads).
// Motor modülünü paylaşılan bellekle, kendi yığınıyla örnekler; sonra sonsuza dek: iş bekle →
// yeni Matoki modüllerini (jit kayıt defteri) aynı tablo indekslerine kendi önbellek dilimiyle kur → çalıştır.
import { instantiateJit, readText } from './mt.mjs';

const isNode = typeof process !== 'undefined' && !!(process.versions && process.versions.node);
let port;
if (isNode) port = (await import('node:worker_threads')).parentPort;
else port = self;

function run({ module, memory, tid, stack }) {
  const env = {
    memory,
    plat_log(p, n) { console.log(`[worker ${tid}] ${readText(memory, p, n)}`); },
    plat_now_ms() { return performance.now(); },
    plat_jit_instantiate() { return -1; },   // JIT yalnızca ana thread'de
  };
  const ex = new WebAssembly.Instance(module, { env }).exports;
  ex.__stack_pointer.value = stack;
  const table = ex.__indirect_function_table;
  port.postMessage('ready');
  let installed = 0;
  for (;;) {
    ex.jobs_worker_wait(tid);
    const count = ex.jit_count();
    while (installed < count) {
      // JitEntry: bytes, len, table_index, cache, cache_stride (u32, jit_wasm.c)
      const d = new DataView(memory.buffer, ex.jit_entry(installed), 20);
      const bytes = new Uint8Array(memory.buffer, d.getUint32(0, true), d.getUint32(4, true)).slice();
      const idx = d.getUint32(8, true);
      const fns = instantiateJit(memory, bytes, d.getUint32(12, true) + tid * d.getUint32(16, true));
      if (table.length < idx + fns.length) table.grow(idx + fns.length - table.length);
      fns.forEach((fn, k) => table.set(idx + k, fn));
      installed++;
    }
    ex.jobs_worker_run(tid);
  }
}

const onMessage = (m) => {
  try {
    run(m);
  } catch (e) {
    port.postMessage(`worker hatası: ${e && e.message}`);
  }
};
if (isNode) port.on('message', onMessage);
else port.onmessage = (e) => onMessage(e.data);
