// tools/shot.mjs — bench görünümünün karesini PNG'ye yazar (görsel karşılaştırma için).
// Kullanım: node tools/shot.mjs <görünüm> <shader modu> <çıktı.png> [--frames N] [--res WxH] [--threads N] [--msaa 0|1]
// frames > 0: her biri 1/60 sn ilerletilir (fizik/rüzgâr canlanır); 0: sabit kare.
import fs from 'node:fs';
import zlib from 'node:zlib';
import { loadEngine, startWorkers } from '../web/mt.mjs';

const args = process.argv.slice(2);
const [view, mode, out] = [+args[0], +args[1], args[2]];
let frames = 0, W = 640, H = 360, threads = 0, msaa = 1;
for (let i = 3; i < args.length; i++) {
  if (args[i] === '--frames') frames = +args[++i];
  else if (args[i] === '--res') [W, H] = args[++i].split('x').map(Number);
  else if (args[i] === '--threads') threads = +args[++i];
  else if (args[i] === '--msaa') msaa = +args[++i];
}
const { state, ex, module } = await loadEngine(fs.readFileSync(new URL('../build/engine.wasm', import.meta.url)), () => {});
ex.engine_init(W, H);
const workers = await startWorkers(state, ex, module, threads);
ex.engine_set_shader_mode(mode);
ex.engine_set_msaa(msaa);
ex.engine_bench_view(view);
ex.engine_frame(0);
for (let i = 0; i < frames; i++) ex.engine_frame(1000 / 60);
const px = new Uint8Array(state.memory.buffer, ex.engine_framebuffer(), W * H * 4).slice();

// PNG: RGBA8, filtre yok
const crcTable = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
const chunk = (type, data) => {
  const t = Buffer.from(type), len = Buffer.alloc(4), c = Buffer.alloc(4);
  len.writeUInt32BE(data.length); c.writeUInt32BE(crc(Buffer.concat([t, data])));
  return Buffer.concat([len, t, data, c]);
};
const raw = Buffer.alloc((W * 4 + 1) * H);
for (let y = 0; y < H; y++) { raw[y * (W * 4 + 1)] = 0; Buffer.from(px.buffer, y * W * 4, W * 4).copy(raw, y * (W * 4 + 1) + 1); }
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 6;
fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
  chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
for (const w of workers) w.terminate();
