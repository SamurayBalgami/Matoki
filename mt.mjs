// web/mt.mjs — motorun platform katmanı: paylaşılan bellek, import'lar, Matoki JIT, worker'lar.
// Tarayıcıda (crossOriginIsolated: COOP/COEP gerekli) ve node'da aynı kod. Motor mantığı yok:
// iş dağıtımı C'de (core/jobs_wasm.c); worker'lar aynı modülü kendi yığınlarıyla örnekler (worker.mjs).
const isNode = typeof process !== 'undefined' && !!(process.versions && process.versions.node);
const JIT_EXPORTS = ['shade', 'prelude', 'shade_fast'];   // jit_wasm.c bu sırayı varsayar
const dec = new TextDecoder();

// motorun bellek sınırı (build.py --max-memory) ile aynı: 1 GiB = 16384 sayfa
export function createMemory() {
  return new WebAssembly.Memory({ initial: 64, maximum: 16384, shared: true });
}

// paylaşılan bellekten metin (TextDecoder paylaşılan görünümü kabul etmez → kopya)
export function readText(memory, p, n) { return dec.decode(new Uint8Array(memory.buffer, p, n).slice()); }

// Matoki modülünü bu örnek için kur: cache = bu thread'in önsöz önbellek dilimi
export function instantiateJit(memory, bytes, cache) {
  const env = { memory, cache: new WebAssembly.Global({ value: 'i32', mutable: false }, cache) };
  const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env });
  return JIT_EXPORTS.map((k) => inst.exports[k]).filter(Boolean);
}

// ana örneğin import'ları; state = { memory, table } (table örnekleme sonrası doldurulur)
export function makeEnv(state, log) {
  return {
    memory: state.memory,
    plat_log(p, n) { log(readText(state.memory, p, n)); },
    plat_now_ms() { return performance.now(); },
    plat_jit_instantiate(p, n, cache) {
      try {
        const fns = instantiateJit(state.memory, new Uint8Array(state.memory.buffer, p, n).slice(), cache);
        const idx = state.table.grow(fns.length);
        fns.forEach((fn, k) => state.table.set(idx + k, fn));
        return idx;
      } catch (e) {
        log(`plat_jit_instantiate: ${e.message}\n`);
        return -1;
      }
    },
  };
}

// module'ü derleyip ana örneği kurar → { state, ex, module }
export async function loadEngine(source, log) {
  const state = { memory: createMemory(), table: null };
  const module = source instanceof WebAssembly.Module ? source : await WebAssembly.compile(source);
  const instance = await WebAssembly.instantiate(module, { env: makeEnv(state, log) });
  state.table = instance.exports.__indirect_function_table;
  return { state, ex: instance.exports, module };
}

async function spawn() {
  const url = new URL('./worker.mjs', import.meta.url);
  if (isNode) {
    const { Worker } = await import('node:worker_threads');
    const w = new Worker(url);
    return { post: (m) => w.postMessage(m), on: (fn) => w.on('message', fn), onError: (fn) => w.on('error', fn), terminate: () => w.terminate() };
  }
  const w = new Worker(url, { type: 'module' });
  return { post: (m) => w.postMessage(m), on: (fn) => { w.onmessage = (e) => fn(e.data); }, onError: (fn) => { w.onerror = fn; }, terminate: () => w.terminate() };
}

// n worker başlatır; hepsi hazır olunca motora bildirir (o andan sonra raster paralel). → worker listesi
export async function startWorkers(state, ex, module, n) {
  n = Math.max(0, Math.min(n | 0, 15));
  const workers = [], ready = [];
  for (let tid = 1; tid <= n; tid++) {
    const stack = ex.jobs_stack_top(tid);
    if (!stack) break;
    const w = await spawn();
    ready.push(new Promise((res, rej) => {
      w.on((m) => (m === 'ready' ? res() : rej(new Error(String(m)))));
      w.onError((e) => rej(e));
    }));
    w.post({ module, memory: state.memory, tid, stack });
    workers.push(w);
  }
  await Promise.all(ready);
  ex.jobs_set_workers(workers.length);
  return workers;
}
