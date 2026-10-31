// tools/check_demo.mjs — demo düzeyi gerileme denetimleri (C testleri motorun global demo durumunu sürmez).
// engine.wasm'ı yükler, dışa açık fonksiyonlarla girdi verir, istatistiklerden sonucu okur. Çıkış kodu 1 = başarısız.
// Kullanım: node tools/check_demo.mjs [build/engine.wasm]
import fs from 'node:fs';
import { loadEngine } from '../web/mt.mjs';

const path = process.argv[2] || new URL('../build/engine.wasm', import.meta.url);
const { state, ex } = await loadEngine(fs.readFileSync(path), () => {});
const W = 320, H = 180;
ex.engine_init(W, H);
const mem = () => (ex.memory || state.memory).buffer;
const stats = () => new Uint32Array(mem(), ex.engine_stats(), ex.engine_stats_count());
// demo.c enum sonu: ... ST_HUD_US, ST_ANIM_US, ST_AUDIO_US, ST_AUDIO_VOICES, ST_IMPACTS, ST_AUDIO_UNDERRUNS, ST_SHADOW_US
const ST_HUD_US = ex.engine_stats_count() - 7, ST_SHADOW_US = ex.engine_stats_count() - 1;
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails++; };
const frame = () => ex.engine_frame(16.7);
const tap = (code) => { ex.engine_key(code, 1); frame(); ex.engine_key(code, 0); frame(); };
const type = (ch) => { ex.engine_key(ch.toUpperCase().charCodeAt(0), 1); ex.engine_char(ch.codePointAt(0)); frame(); ex.engine_key(ch.toUpperCase().charCodeAt(0), 0); frame(); };

// 1. editör açıkken yazılan 'u' / 'b' paneli / gölgeyi değiştirmez
ex.engine_set_hud(1);
ex.engine_set_shadow(1);
tap(51);   // sahne 3 (gölgeli)
frame();
const hud0 = stats()[ST_HUD_US] > 0, sh0 = stats()[ST_SHADOW_US] > 0;
check(hud0 && sh0, 'başlangıç: panel ve gölge açık');
tap(113);   // F2
check(ex.engine_ui_active() === 1, 'F2 editörü açar');
for (const ch of 'ub') type(ch);   // tek basış: çift sayıda basış açma kapamayı gizlerdi
frame();
check(stats()[ST_HUD_US] > 0, "editörde 'u' yazmak paneli kapatmaz");
check(stats()[ST_SHADOW_US] > 0, "editörde 'b' yazmak gölgeyi kapatmaz");
tap(113);
check(ex.engine_ui_active() === 0, 'F2 editörü kapatır');
tap(85);   // U editör kapalıyken çalışır
check(stats()[ST_HUD_US] === 0, 'editör kapalıyken U paneli kapatır');

// 2. canlı derleme uniform denetimi: ad + tip + ofset (yanlış tipte aynı ofset ya da yapı dışı uniform reddedilir)
const lit = fs.readFileSync(new URL('../shaders/lit.mtk', import.meta.url), 'utf8');
const recompile = (src) => {
  const bytes = new TextEncoder().encode(src);
  const p = ex.engine_scratch(bytes.length);
  new Uint8Array(mem(), p, bytes.length).set(bytes);
  return ex.engine_matoki_recompile(bytes.length);
};
check(recompile(lit) === 1, 'lit.mtk olduğu gibi derlenir');
const afterTint = (extra) => lit.replace('uniform tint: vec3;', 'uniform tint: vec3;\n' + extra);
check(recompile(afterTint('uniform wrap: f32;\nuniform translucency: f32;')) === 1, 'LitUniforms sırasındaki ek uniform kabul');
check(recompile(afterTint('uniform wrap: vec4;')) === 0, 'aynı ofsette yanlış tip reddedilir');
check(recompile(afterTint('uniform extra: vec4;')) === 0, 'LitUniforms dışı uniform reddedilir');
check(recompile(lit.replace('uniform use_mip: i32;', 'uniform use_mip: f32;')) === 0, 'i32 ↔ f32 tip değişimi reddedilir');

// 3. fizik ↔ görüntü ara değerlemesi (D39): 144 Hz karelerle aynı girdi, ara değer açık ve kapalı → fizik durumu
// aynı kalır (son kare dt = 0 ve ara değer kapalı çizilir: çizilen = fiziğin kendi pozu). Açıkken çizilen kare farklı
// (ara poz gerçekten yazılıyor). Sahne 3 tuşla yeniden kurulur, yani iki koşu aynı başlangıçtan.
const fbHash = () => {
  const px = new Uint32Array(mem(), ex.engine_framebuffer(), W * H);
  let h = 2166136261;
  for (let i = 0; i < px.length; i++) h = Math.imul(h ^ px[i], 16777619) >>> 0;
  return h;
};
const physRun = (interp) => {
  tap(52); tap(51);   // sahne 4 → 3: yeniden kurulum
  ex.engine_set_interp(interp);
  for (let f = 0; f < 200; f++) ex.engine_frame(1000 / 144);
  ex.engine_frame(0);
  const shown = fbHash();
  ex.engine_set_interp(0);
  ex.engine_frame(0);
  return { shown, state: fbHash() };
};
ex.engine_set_hud(0);   // panel ölçüm sürelerini yazar: kare hash'ini oynatır
const ipOn = physRun(1), ipOff = physRun(0);
check(ipOn.state === ipOff.state, 'ara değerleme fizik durumunu değiştirmez (144 Hz, 200 kare)');
check(ipOn.shown !== ipOn.state, 'ara değer açıkken çizilen poz ara pozdur');
ex.engine_set_interp(1);

// 4. yavaş karede fizik gerçek zamanda kalır (adım sınırı zaman bütçesi; önceden 4 adım/kare → 8 fps'te %53 hız)
// ve bütçeyi aşmaz: fizik ms ≤ max(4, dt/2) + bir adım. demo.c stats sırası: ST_PHYS_US = 26, ST_PHYS_STEPS = 27.
for (const [scene, dtms] of [[5, 125], [6, 125], [3, 250]]) {
  tap(48 + scene);
  let steps = 0, worst = 0;
  for (let f = 0; f < 30; f++) {
    ex.engine_frame(dtms);
    const s = stats();
    steps += s[27];
    const perStep = s[27] ? s[26] / 1000 / s[27] : 0;
    worst = Math.max(worst, s[26] / 1000 - Math.max(4, dtms / 2) - perStep);
  }
  const speed = steps * (1000 / 60) / (30 * dtms);
  check(speed > 0.99, `sahne ${scene}, ${Math.round(1000 / dtms)} fps: fizik hızı ${speed.toFixed(3)} (gerçek zaman)`);
  check(worst <= 0, `sahne ${scene}: fizik süresi bütçe içinde`);
}

console.log(fails ? `${fails} denetim başarısız` : 'tüm demo denetimleri geçti');
process.exit(fails ? 1 : 0);
