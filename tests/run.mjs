// tests/run.mjs — wasm'a derlenmiş test modülünü node'da koşar.
// Kullanım: node tests/run.mjs build/tests.wasm
// plat_log / plat_now_ms / plat_jit_instantiate import'larını sağlar; çıkış kodu = başarısız test sayısı (>0 → 1).
import fs from 'node:fs';

const bytes = fs.readFileSync(process.argv[2]);
const dec = new TextDecoder();
let mem = null;
let table = null;
const env = {
  plat_log(p, n) { process.stdout.write(dec.decode(new Uint8Array(mem.buffer, p, n))); },
  plat_now_ms() { return performance.now(); },
  // Matoki JIT: modülü derle, belleği paylaştır, "shade"i fonksiyon tablosuna ekle → indeks
  plat_jit_instantiate(p, n) {
    try {
      const code = new Uint8Array(mem.buffer, p, n).slice();
      const inst = new WebAssembly.Instance(new WebAssembly.Module(code), { env: { memory: mem } });
      const idx = table.grow(1);
      table.set(idx, inst.exports.shade);
      return idx;
    } catch (e) {
      process.stdout.write(`plat_jit_instantiate: ${e.message}\n`);
      return -1;
    }
  },
};
const { instance } = await WebAssembly.instantiate(bytes, { env });
mem = instance.exports.memory;
table = instance.exports.__indirect_function_table;
const failed = instance.exports.test_main();
process.exit(failed ? 1 : 0);
