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
  // Matoki JIT: modülü derle, belleği paylaştır, export'ları fonksiyon tablosuna ekle → ilk indeks
  plat_jit_instantiate(p, n) {
    try {
      const code = new Uint8Array(mem.buffer, p, n).slice();
      const inst = new WebAssembly.Instance(new WebAssembly.Module(code), { env: { memory: mem } });
      // shade, prelude, shade_fast (olanlar) ardışık indekslere — jit_wasm.c bu sırayı varsayar
      const fns = ['shade', 'prelude', 'shade_fast'].map((k) => inst.exports[k]).filter(Boolean);
      const idx = table.grow(fns.length);
      fns.forEach((fn, k) => table.set(idx + k, fn));
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
