// web/3d.js — MATOKI 3D demo yükleyicisi (yalnızca platform işi).
// wasm yükleme, giriş olaylarını motora iletme, framebuffer'ı canvas'a kopyalama ve
// istatistik metni. Tüm motor mantığı C'de (src/gfx3d/demo.c).
// Çözünürlük: canvas'ın ekrandaki gerçek piksel boyutu (CSS genişliği × devicePixelRatio) × ölçek, en çok 1920×1080.
// ?res=WxH sabitler; H tuşu ölçeği %100 → %75 → %50 gezer (yavaş makinede hız için).
let W = 640, H = 360;
const MAX_PIXELS = 1920 * 1080;
const SCALES = [1, 0.75, 0.5];
let scaleIdx = 0;
let autoScale = true;   // FPS < 25 sürerse ölçek kendiliğinden iner; H'ye basınca kapanır
let slowWindows = 0;
const canvas = document.getElementById('screen');
const info = document.getElementById('info');
const errBox = document.getElementById('err');
const ctx = canvas.getContext('2d');

import { loadEngine, startWorkers } from './mt.mjs';

let mem = null;
let logBuf = '';
function log(text) {
  logBuf += text;
  let i;
  while ((i = logBuf.indexOf('\n')) >= 0) { console.log(logBuf.slice(0, i)); logBuf = logBuf.slice(i + 1); }
}
// ?threads=N: worker sayısı (0 = tek thread); varsayılan çekirdek sayısı - 1
const qs = new URLSearchParams(location.search);
const WANT_WORKERS = qs.has('threads') ? +qs.get('threads') : Math.max(0, (navigator.hardwareConcurrency || 4) - 1);
let threads = 1;
// engine_stats yerleşimi — demo.c'deki ST_* enum'u ile aynı sıra
const ST = ['scene', 'mip', 'cam', 'objIn', 'objCulled', 'triIn', 'triRejected', 'triNear', 'triGuard',
  'triEmitted', 'triBack', 'triDegen', 'triDrawn', 'quads', 'pixels', 'frameUs', 'arenaKb', 'camX', 'camY', 'camZ',
  'rasterMode', 'binItems', 'binFull', 'shaderMode', 'physBodies', 'physContacts', 'physUs', 'physSteps', 'physAwake',
  'playMode', 'carKmh', 'carWheels', 'charGrounded', 'softParticles', 'softUs', 'softContacts',
  'clearUs', 'geoUs', 'flushUs', 'totalUs', 'preludes', 'msaa', 'resolveUs', 'vertMode', 'verts', 'batches', 'vertUs', 'setupUs'];
// ?prof: kareler rAF yerine art arda, sabit 1/60 sn adımla sürülür (vsync ve gizli sekmeden bağımsız);
// her sahnede 60 kare ısınma + 180 kare ölçüm, aşama ortalamaları konsola ve window.__prof'a
const PROF = qs.has('prof');
const PROF_SCENES = [1, 2, 3, 4, 5, 6, 7, 8], PROF_WARM = 60, PROF_MEASURE = 180, PROF_DT = 1000 / 60;
const PLAY_NAMES = ['SÜRÜŞ', 'YÜRÜYÜŞ', 'UÇUŞ'];
const SHADER_NAMES = ['ref (skaler)', 'tablo (skaler)', 'SIMD', 'Matoki JIT', 'Matoki HQ (anizotropik)'];

async function main() {
  if (!crossOriginIsolated) throw new Error('SharedArrayBuffer yok: sayfa COOP/COEP başlıklarıyla sunulmalı (python build.py serve)');
  const { state, ex, module } = await loadEngine(await WebAssembly.compileStreaming(fetch('engine.wasm')), log);
  mem = state.memory;
  const fixed = qs.has('res') ? qs.get('res').split('x').map(Number) : null;
  function targetSize() {
    if (fixed) return fixed;
    const dpr = window.devicePixelRatio || 1;
    let w = Math.round(canvas.clientWidth * dpr * SCALES[scaleIdx]);
    let h = Math.round(w * 9 / 16);
    const k = Math.sqrt(Math.min(1, MAX_PIXELS / (w * h)));
    w = Math.max(160, Math.floor(w * k / 2) * 2);
    h = Math.max(90, Math.floor(h * k / 2) * 2);
    return [w, h];
  }
  [W, H] = targetSize();
  if (!ex.engine_init(W, H)) throw new Error('engine_init başarısız');
  canvas.width = W; canvas.height = H;  // kare döngüsü worker'lar hazır olunca başlar (başaramazsa tek thread devam)
  try {
    threads = 1 + (await startWorkers(state, ex, module, WANT_WORKERS)).length;
  } catch (e) {
    console.error('worker başlatılamadı, tek thread', e);
  }
  let img = ctx.createImageData(W, H);
  function applySize() {
    const [w, h] = targetSize();
    if (w === W && h === H) return;
    if (!ex.engine_resize(w, h)) return;
    W = w; H = h;
    canvas.width = W; canvas.height = H;
    img = ctx.createImageData(W, H);
  }
  addEventListener('resize', applySize);
  const swallow = new Set(['Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'F1']);
  addEventListener('keydown', (e) => {
    if (swallow.has(e.code)) e.preventDefault();
    if (!e.repeat) ex.engine_key(e.keyCode, 1);
    if (e.code === 'KeyH' && !e.repeat) { autoScale = false; scaleIdx = (scaleIdx + 1) % SCALES.length; applySize(); }
  });
  addEventListener('keyup', (e) => ex.engine_key(e.keyCode, 0));
  addEventListener('blur', () => ex.engine_release_keys());
  canvas.addEventListener('click', () => canvas.requestPointerLock());
  addEventListener('mousemove', (e) => {
    if (document.pointerLockElement === canvas) ex.engine_mouse(e.movementX, e.movementY);
  });
  canvas.addEventListener('wheel', (e) => { e.preventDefault(); ex.engine_wheel(Math.sign(e.deltaY)); }, { passive: false });

  let last = performance.now();
  let acc = 0, frames = 0, fps = 0, lastInfo = 0;
  const sum = {}, avg = {};   // aşama süreleri (ms), 500 ms ortalaması
  const prof = { i: -1, k: 0, n: 0, sum: null, rows: [], now: 0 };
  function profNext() {
    prof.i++;
    if (prof.i >= PROF_SCENES.length) {
      window.__prof = prof.rows;
      console.table(prof.rows);
      return;
    }
    const code = 48 + PROF_SCENES[prof.i];
    ex.engine_key(code, 1); ex.engine_key(code, 0);
    prof.k = 0; prof.n = 0; prof.sum = null;
  }
  const profChan = new MessageChannel();
  profChan.port1.onmessage = () => { prof.now += PROF_DT; frame(prof.now); };
  if (PROF) profNext();

  function frame(now) {
    const dt = now - last;
    last = now;
    const w0 = performance.now();
    ex.engine_frame(dt);
    const c0 = performance.now();
    img.data.set(new Uint8Array(mem.buffer, ex.engine_framebuffer(), W * H * 4));
    ctx.putImageData(img, 0, 0);
    const copyMs = performance.now() - c0;
    const wallMs = performance.now() - w0;

    const s = new Uint32Array(mem.buffer, ex.engine_stats(), ex.engine_stats_count());
    const st = Object.fromEntries(ST.map((k, i) => [k, s[i]]));
    const ms = {
      dt, total: st.totalUs / 1000, clear: st.clearUs / 1000, geo: st.geoUs / 1000, flush: st.flushUs / 1000,
      phys: st.physUs / 1000, soft: st.softUs / 1000, copy: copyMs, wall: wallMs, resolve: st.resolveUs / 1000,
      vert: st.vertUs / 1000, setup: st.setupUs / 1000,
    };
    acc += dt; frames++;
    for (const k in ms) sum[k] = (sum[k] || 0) + ms[k];
    if (acc >= 500) {
      fps = frames * 1000 / acc;
      // iki ardışık yavaş pencere (1 s) → bir kademe düşük çözünürlük (profil modunda değil)
      slowWindows = fps < 25 ? slowWindows + 1 : 0;
      if (autoScale && !PROF && !fixed && slowWindows >= 2 && scaleIdx < SCALES.length - 1) { scaleIdx++; applySize(); slowWindows = 0; }
      for (const k in sum) { avg[k] = sum[k] / frames; sum[k] = 0; }
      acc = 0; frames = 0;
    }
    if (PROF && prof.i < PROF_SCENES.length) {
      if (++prof.k > PROF_WARM) {
        if (!prof.sum) prof.sum = { tris: 0, px: 0 };
        for (const k in ms) if (k !== 'dt') prof.sum[k] = (prof.sum[k] || 0) + ms[k];
        prof.sum.tris += st.triDrawn; prof.sum.px += st.pixels;
        prof.n++;
      }
      if (prof.k >= PROF_WARM + PROF_MEASURE) {
        const r = { sahne: st.scene };
        for (const k in prof.sum) r[k] = +(prof.sum[k] / prof.n).toFixed(k === 'tris' || k === 'px' ? 0 : 2);
        r.tavanFps = +(1000 / r.wall).toFixed(1);   // yalnızca motor + kopya: ulaşılabilir en yüksek FPS
        prof.rows.push(r);
        profNext();
      }
    }
    if (now - lastInfo > 100) {
      lastInfo = now;
      const i32 = (v) => (v | 0) / 100;
      const f = (k) => (avg[k] || 0).toFixed(2);
      info.textContent =
        (PROF ? (prof.i < PROF_SCENES.length ? `PROFİL: sahne ${PROF_SCENES[prof.i]} ölçülüyor…\n` : 'PROFİL bitti (konsol / window.__prof)\n') : '') +
        `sahne ${st.scene}   mipmap ${st.mip ? 'AÇIK' : 'kapalı'}   kamera ${['uçuş', 'orbit', 'oyun'][st.cam] || '?'}\n` +
        `${fps.toFixed(1)} FPS   motor ${f('total')} ms   ${W}×${H} (%${Math.round(SCALES[scaleIdx] * 100)})   ${threads} thread\n` +
        `  temizle ${f('clear')}  geometri ${f('geo')}  boyama ${f('flush')}  fizik ${f('phys')}  yumuşak ${f('soft')}  kopya ${f('copy')}\n` +
        `raster ${st.rasterMode ? 'SIMD' : 'skaler'}   shader ${SHADER_NAMES[st.shaderMode]}   MSAA ${st.msaa}x (çözümleme ${f('resolve')} ms)\n` +
        `köşe aşaması ${st.vertMode ? 'Matoki' : 'C'} ${f('vert')} ms   üçgen kurulumu ${f('setup')} ms   ${st.verts} köşe, ${st.batches} parti\n` +
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
    if (PROF && prof.i < PROF_SCENES.length) profChan.port2.postMessage(0);
    else requestAnimationFrame(frame);
  }
  if (PROF) { prof.now = last; profChan.port2.postMessage(0); }
  else requestAnimationFrame(frame);
}

main().catch((e) => { errBox.textContent = String(e && e.stack || e); console.error(e); });
