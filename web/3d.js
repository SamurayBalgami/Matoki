// web/3d.js — MATOKI 3D demo yükleyicisi (yalnızca platform işi).
// wasm yükleme, giriş olaylarını motora iletme, framebuffer'ı canvas'a kopyalama ve
// istatistik metni. Tüm motor mantığı C'de (src/gfx3d/demo.c).
const W = 640, H = 360;
const canvas = document.getElementById('screen');
const info = document.getElementById('info');
const errBox = document.getElementById('err');
const ctx = canvas.getContext('2d');

let mem = null;
let table = null;
const dec = new TextDecoder();
let logBuf = '';
const env = {
  plat_log(p, n) {
    logBuf += dec.decode(new Uint8Array(mem.buffer, p, n));
    let i;
    while ((i = logBuf.indexOf('\n')) >= 0) { console.log(logBuf.slice(0, i)); logBuf = logBuf.slice(i + 1); }
  },
  plat_now_ms() { return performance.now(); },
  // Matoki JIT: üretilen modülü derle, motorun belleğini paylaştır, "shade"i fonksiyon tablosuna ekle
  plat_jit_instantiate(p, n) {
    try {
      const code = new Uint8Array(mem.buffer, p, n).slice();
      const inst = new WebAssembly.Instance(new WebAssembly.Module(code), { env: { memory: mem } });
      const idx = table.grow(1);
      table.set(idx, inst.exports.shade);
      return idx;
    } catch (e) {
      console.error('plat_jit_instantiate', e);
      return -1;
    }
  },
};

// engine_stats yerleşimi — demo.c'deki ST_* enum'u ile aynı sıra
const ST = ['scene', 'mip', 'cam', 'objIn', 'objCulled', 'triIn', 'triRejected', 'triNear', 'triGuard',
  'triEmitted', 'triBack', 'triDegen', 'triDrawn', 'quads', 'pixels', 'frameUs', 'arenaKb', 'camX', 'camY', 'camZ',
  'rasterMode', 'binItems', 'binFull', 'shaderMode', 'physBodies', 'physContacts', 'physUs', 'physSteps', 'physAwake',
  'playMode', 'carKmh', 'carWheels', 'charGrounded', 'softParticles', 'softUs', 'softContacts'];
const PLAY_NAMES = ['SÜRÜŞ', 'YÜRÜYÜŞ', 'UÇUŞ'];
const SHADER_NAMES = ['ref (skaler)', 'tablo (skaler)', 'SIMD', 'Matoki JIT (mip yok)'];

async function main() {
  const { instance } = await WebAssembly.instantiateStreaming(fetch('engine.wasm'), { env });
  const ex = instance.exports;
  mem = ex.memory;
  table = ex.__indirect_function_table;
  if (!ex.engine_init(W, H)) throw new Error('engine_init başarısız');
  const img = ctx.createImageData(W, H);

  const swallow = new Set(['Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'F1']);
  addEventListener('keydown', (e) => {
    if (swallow.has(e.code)) e.preventDefault();
    if (!e.repeat) ex.engine_key(e.keyCode, 1);
  });
  addEventListener('keyup', (e) => ex.engine_key(e.keyCode, 0));
  addEventListener('blur', () => ex.engine_release_keys());
  canvas.addEventListener('click', () => canvas.requestPointerLock());
  addEventListener('mousemove', (e) => {
    if (document.pointerLockElement === canvas) ex.engine_mouse(e.movementX, e.movementY);
  });
  canvas.addEventListener('wheel', (e) => { e.preventDefault(); ex.engine_wheel(Math.sign(e.deltaY)); }, { passive: false });

  let last = performance.now();
  let acc = 0, frames = 0, renderAcc = 0, fps = 0, renderMs = 0, lastInfo = 0;
  function frame(now) {
    const dt = now - last;
    last = now;
    ex.engine_frame(dt);
    img.data.set(new Uint8Array(mem.buffer, ex.engine_framebuffer(), W * H * 4));
    ctx.putImageData(img, 0, 0);

    const s = new Uint32Array(mem.buffer, ex.engine_stats(), ex.engine_stats_count());
    const st = Object.fromEntries(ST.map((k, i) => [k, s[i]]));
    acc += dt; frames++; renderAcc += st.frameUs / 1000;
    if (acc >= 500) { fps = frames * 1000 / acc; renderMs = renderAcc / frames; acc = 0; frames = 0; renderAcc = 0; }
    if (now - lastInfo > 100) {
      lastInfo = now;
      const i32 = (v) => (v | 0) / 100;
      info.textContent =
        `sahne ${st.scene}   mipmap ${st.mip ? 'AÇIK' : 'kapalı'}   kamera ${['uçuş', 'orbit', 'oyun'][st.cam] || '?'}\n` +
        `${fps.toFixed(1)} FPS   render ${renderMs.toFixed(2)} ms   ${W}×${H}\n` +
        `raster ${st.rasterMode ? 'SIMD' : 'skaler'}   shader ${SHADER_NAMES[st.shaderMode]}\n` +
        `bin ${st.binItems} (tam ${st.binFull})\n` +
        (st.scene >= 3
          ? `fizik: ${st.physBodies} gövde (uyanık ${st.physAwake}), ${st.physContacts} temas, ${(st.physUs / 1000).toFixed(2)} ms (${st.physSteps} adım)\n`
          : '') +
        (st.scene === 4
          ? `mod ${PLAY_NAMES[st.playMode]}   hız ${Math.abs((st.carKmh | 0) / 100).toFixed(0)} km/s   tekerlek yerde ${st.carWheels}/4` +
            (st.playMode === 1 ? `   karakter ${st.charGrounded ? 'yerde' : 'havada'}` : '') + '\n'
          : '') +
        (st.scene === 6
          ? `yumuşak: ${st.softParticles} parçacık, ${st.softContacts} temas, ${(st.softUs / 1000).toFixed(2)} ms\n`
          : '') + '\n' +
        `nesne        ${st.objIn} (elenen ${st.objCulled})\n` +
        `üçgen girdi  ${st.triIn}\n` +
        `  frustum ret  ${st.triRejected}\n` +
        `  yakın kırp.  ${st.triNear}\n` +
        `  guard kırp.  ${st.triGuard}\n` +
        `  raster'a     ${st.triEmitted}\n` +
        `  arka yüz     ${st.triBack}\n` +
        `  boş/dejenere ${st.triDegen}\n` +
        `  çizilen      ${st.triDrawn}\n` +
        `quad ${st.quads}   piksel ${st.pixels}\n` +
        `raster arena tepe ${st.arenaKb} KB\n` +
        `konum ${i32(st.camX).toFixed(1)}, ${i32(st.camY).toFixed(1)}, ${i32(st.camZ).toFixed(1)}`;
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

main().catch((e) => { errBox.textContent = String(e && e.stack || e); console.error(e); });
