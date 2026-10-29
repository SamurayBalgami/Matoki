# M10 — Temel: render + Matoki önce, fizik üstüne

Fizik (M9) kodun yarısına ulaştı; render ve Matoki geride kaldı. Bu kilometre taşı sırayı düzeltir:
önce ölç, sonra Matoki'yi tamamla, sonra render çekirdeğini hızlandır. Fizik Faz 9 bu sırada bekler.

## Faz 1 — Tarayıcıda kare profili (2026-10-28) ✅

`engine_frame` artık aşama sürelerini yazar (`ST_CLEAR_US`, `ST_GEO_US`, `ST_FLUSH_US`, `ST_TOTAL_US`); demo
paneli 500 ms ortalamalarını gösterir. `3d.html?prof` her sahnede 60 kare ısınma + 180 kare ölçüm yapar ve
kareleri rAF yerine art arda sürer (vsync ve gizli sekmeden bağımsız: ölçülen, ulaşılabilir en yüksek FPS).

Chromium (Claude tarayıcı paneli), i7-12650H, 640×360, tek thread, ms/kare:

| sahne | temizle | geometri | boyama (raster+shader) | fizik | yumuşak | kopya (JS) | toplam | tavan FPS | üçgen | piksel |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 küp + ızgara | 0.16 | 2.36 | 6.36 | – | – | 0.33 | 9.23 | 108 | 2855 | 156k |
| 2 mip zemini | 0.16 | 0.03 | 4.11 | – | – | 0.34 | 4.65 | 215 | 16 | 139k |
| 3 fizik | 0.16 | 0.37 | 4.73 | 0.04 | – | 0.34 | 5.65 | 177 | 918 | 140k |
| 4 araba | 0.16 | 0.17 | 5.06 | 0.03 | – | 0.33 | 5.77 | 173 | 268 | 155k |
| 5 eklemler | 0.17 | 2.24 | 6.08 | 0.59 | – | 0.38 | 9.47 | 106 | 2782 | 134k |
| 6 bayrak + jöle | 0.19 | 0.26 | 3.91 | 0.02 | 3.60 | 0.36 | 8.36 | 120 | 948 | 111k |

Sonuçlar:
- **Boyama baskın** (%50–88). Sahne 2'de ~30 ns/piksel. `flat` shader ölçümü (tools/bench.mjs) raster'ın tek
  başına bunun ~yarısı olduğunu gösteriyor → render çekirdeği (M10 Faz 3) ve paralel raster (worker'lar) en büyük kaldıraç.
- **Geometri** 2800 üçgende 2.3 ms (~0.8 µs/üçgen): vertex aşaması skaler (yol haritası 4c).
- Fizik bu sahnelerde ihmal edilebilir; kumaş (3.6 ms) tek pahalı fizik parçası.
- 720p'de piksel 4 kat → yalnızca boyama ~16–25 ms: tek thread'de 60 FPS sınırda.

## Faz 2 — Matoki: eksik dil özellikleri (2026-10-28) ✅

| özellik | uygulama | doğrulama |
|---|---|---|
| `@mipmap`, `mip_level`, `sample_level` | türevlerden seviye (tamsayı üs kuralı), `Texture.lv_ofs` tablosu (D23), `IR_TEX_WORD` | `ref_sample_tex_mip` / `ref_sample_level` ile bit-bit; 0..8 seviyelerin hepsi, wrap/clamp, kare olmayan |
| lit.mtk mip'li | `select(use_mip != 0, mip_level(tex, uv), 0)` + `sample_level` | `shade_lit_tex` ile 30 000 quad bit-bit (mip açık/kapalı); bench hash'leri C ile aynı |
| `discard` | `ShadeQuad.kill`, SPMD yol maskesi (D24) | kill bitleri ir_eval = JIT; raster (skaler + SIMD) atılan pikselde renk/derinlik yazmaz |
| `mat3`/`mat4` uniform, `M * v` | yalnızca uniform (D25) | `mat4_mul_v4` / `mat3_mul_v3` ile bit-bit |
| `exp2 log2 exp log pow` | polinom + üs alanı; C eşi `m_exp2f`/`m_log2f` | C ile bit-bit (özel değerler dahil); göreli hata ≤ 7.6e-7 |
| `@bilinear` | seviye içinde 4 texel + 3 lerp | `ref_sample_bilinear` ile bit-bit |
| quad-sabit değerler | `IR_DERIV_*` uniform bayrağı; indeks quad-sabitse tek okuma + yayılım | 500 rastgele program farkı (yeni `IR_TEX_WORD` dahil) |

Ölçüm (node, 640×360, en iyi ms, 3 tur; `build/engine_base.wasm` = c4ce591):

| görünüm | flat önce → sonra | lit SIMD önce → sonra | Matoki (mip'li) | C SIMD (aynı ikili) |
|---|---|---|---|---|
| 1 | 4.15 → 4.20 | 7.15 → 7.08 | 7.17 | 7.08 |
| 2 | 4.50 → 4.49 | 7.71 → 7.87 | 8.37 | 7.87 |
| 3 | 3.70 → 3.68 | 5.34 → 5.36 | 5.45 | 5.36 |
| 4 | 1.15 → 1.14 | 3.34 → 3.32 | 3.23 | 3.32 |

- `kill` denetimi raster'da ölçülebilir maliyet getirmedi (±%2, ölçüm gürültüsü); tüm C hash'leri değişmedi.
- Matoki lit artık C SIMD ile aynı görüntüyü veriyor; hız %-3 … +%6. Demo varsayılanı C SIMD kaldı (en hızlı);
  `L` tuşu Matoki'ye geçer.
- Not: bu oturumda ölçüm makinesi yüklüydü (medyanlar ilk ölçümden ~%30–40 yüksek); karşılaştırma aynı koşulda,
  sırayla ve "en iyi" değerle yapıldı.

## Faz 3 — Matoki varsayılan shader (2026-10-28) ✅

Hedef: demo her şeyi Matoki ile çizsin, C SIMD'den yavaş olmasın, çıktı bit-bit aynı kalsın.

Ölçüm araçları (`tools/bench.mjs`):
- `--shade`: sentetik quad'larda shader mikro-ölçümü; `--replay`: bench görünümünün **gerçek** quad'larını
  yakalayıp (`engine_capture_view`) yalnızca shader'ları ölçer; `--pair`: kare ölçümü, C ve Matoki dönüşümlü.
- Makine ölçüm sırasında yüklüydü (başka uygulama ~2 çekirdek): mutlak süreler ±%40 oynadı. Bu yüzden
  karşılaştırmalar **aynı turda ölçülen iki tarafın oranının medyanı** ile yapıldı (dış yük iki tarafa eşit).

Adımlar ve etkileri (oran = Matoki / C SIMD, shader süresi, yakalanmış gerçek quad'lar):

| adım | sonuç |
|---|---|
| `v128.load32_splat`, vektör adres + `load32_lane` | ~%1 (gürültü içinde); tutuldu: daha kısa kod |
| önsöz (preshader), 5 değer | sentetik quad'larda 1.026 → 0.968 |
| teşhis: lit.mtk parçalarını çıkarma | mip seçimi + seviyeye bağlı boyutlar shader'ın ~%30'u (2. görünüm) |
| seviye tabloları (`lv_w/h/fw/fh`) | 2. görünüm 1.24 → 1.10 |
| skaler quad-sabitler (anahtarla A/B, aynı ikili) | 4 görünümde v128'e göre %6–10 hızlı |
| tek komutluk skaler select | etkisiz (±%2); tutuldu: daha kısa kod |

Kare süresi (eşli, iki tur): 640×360 oranlar 1: 0.974/0.942, 2: 1.023/1.029, 3: 0.966/0.930,
4: 0.918/0.890; 1280×720: 1: 0.919/0.978, 2: 1.035/1.061, 3: 0.980/0.995, 4: 0.951/0.987.
Ortalama Matoki %1–4 hızlı; 2. görünümde (mip hep 0) %2–6 yavaş: C'nin dallı mip seçimi erken çıkıyor.
Tüm görünümlerde hash'ler C ile aynı. Demo varsayılanı artık Matoki; tarayıcıda 6 sahne kontrol edildi.

## Faz 4 — Paralel raster (2026-10-28) ✅

Paylaşılan bellek + aynı modülün worker örnekleri + karo başına iş (D27). Platform katmanı `web/mt.mjs`,
`web/worker.mjs` (tarayıcı ve node ortak); `tests/run.mjs` 3 worker'la koşar (`--threads 0` tek thread).

Node, `tools/bench.mjs --mt` (aynı ikili, tek thread ↔ 16 thread dönüşümlü, kare medyanı; hash'ler hep aynı):

| görünüm | 640×360 | 1280×720 (16 thread) | 1280×720 (8 thread) |
|---|---|---|---|
| 1 başlangıç | 3.99 → 2.08 ms (1.9×) | 12.5 → 3.1 ms (4.1×) | 11.3 → 3.3 ms (3.4×) |
| 2 ızgara içi | 4.45 → 1.55 ms (2.9×) | 21.5 → 3.3 ms (6.5×) | 26.3 → 4.3 ms (6.1×) |
| 3 uzaktan | 3.85 → 2.40 ms (1.6×) | 11.1 → 4.0 ms (2.8×) | 10.5 → 4.0 ms (2.6×) |
| 4 mip zemini | 3.52 → 0.75 ms (4.7×) | 8.1 → 1.5 ms (5.5×) | 8.8 → 2.0 ms (4.4×) |

Tarayıcı (`3d.html?prof`, 640×360, motor ms/kare, `?threads=0` ↔ varsayılan 16 thread):
1: 5.95 → 1.98 · 2: 2.59 → 0.45 · 3: 3.21 → 0.76 · 4: 3.19 → 0.68 · 5: 6.68 → 2.22 · 6: 5.44 → 2.71
(boyama ör. sahne 1'de 4.23 → 0.65 ms). Kalan seri kısım: geometri (sahne 1/5'te ~1.2 ms) ve kumaş fiziği.

## Faz 5 — Görüntü kalitesi: anizotropik süzme, yumuşak kumaş ışığı, MSAA 4x (2026-10-29) ✅

İstenen: zemindeki ve bayraktaki piksel karmaşası (aliasing), bayrak kıvrımlarının belli olması, kenar
tırtıkları. Fizik ve mesh yapısına dokunulmadı (bayrak normalleri zaten her kare yeniden hesaplanıyordu).

- `shaders/lit_hq.mtk` (varsayılan, `L` ile değişir): anizotropik + çift doğrusal + mip; Lambert ↔ half-Lambert²
  diffuse, kumaşta arkadan ışık geçirgenliği (D28). Görsel kontrol `tools/shot.mjs` (bench görünümü → PNG;
  görünüm 5 = bayrak yakından): zeminde ufka yakın moiré bandı kalktı, bayrak kıvrımları koyu bantla belli.
- MSAA 4x (varsayılan, `N` ile kapanır): kenarlar yumuşak (küp kenarı 4× büyütülmüş karşılaştırmada açık).
- Testler (`tests/test_msaa.c`): tam kaplamada MSAA = tek örnek, dikey kenarda örnek sayısı (1/4 → 64),
  ortak kenarda boşluk yok, tek/çok thread ve skaler/SIMD bit-bit; 101/101.

Maliyet (`tools/bench_quality.mjs`, 16 thread, kare medyanı ms):

| görünüm | Matoki | HQ | Matoki + MSAA | HQ + MSAA (yeni) | 720p HQ + MSAA |
|---|---|---|---|---|---|
| 1 | 2.05 | 2.80 | 3.42 | 4.12 | 19.5 |
| 2 | 1.26 | 2.12 | 2.96 | 3.75 | 23.2 |
| 3 | 2.66 | 3.15 | 3.96 | 5.34 | 13.4 |
| 4 | 0.78 | 1.88 | 4.91 | 5.07 | 19.1 |
| 5 | 1.09 | 2.03 | 2.39 | 3.59 | 10.7 |

MSAA aşamaları (görünüm 4): boyama 0.53 → 2.23 ms (skaler ilk sürüm 3.12), temizleme 0.29, çözümleme 0.37.
Tarayıcıda 6 sahne 200 FPS (ekran sınırı), motor 2.5–3.9 ms.

## Faz 6 — Matoki vertex shader'ları + paralel geometri (2026-10-29) ✅

**Matoki vert aşaması** (D29): `attribute`, `fn vert() -> vec4`, varying çıktıları, aşama denetimleri.
- `shaders/std_vert.mtk` (varsayılan; `K` ile C kâhinine geçer) — `vert_std_c` ile bit-bit.
- `shaders/wave_vert.mtk` — 4 Gerstner dalgası, analitik normal; `shaders/water.mtk` — Schlick Fresnel, gök
  yansıması, speküler. Sahne 7 (dalgalı göl, 129² köşe, 32 768 üçgen; `7` tuşu).
- Testler: std_vert ↔ C (ir_eval / JIT / önsöz+hızlı, 5000 grup), aşama hataları (8 vaka), varying if-birleşmesi +
  vertex doku okuması, dalga hareketi, rastgele 300 vert programı (3019 ek çıktı) eval = JIT, boru hattında
  Matoki vert = C vert ve tek thread = tüm thread (17 parti). 107/107 (3 worker ve worker'sız).

**Paralel geometri:** ertelenmiş çizim, köşe işleri + üçgen parçaları, raster parça kutuları, iş başına dilim.

`tools/bench_geo.mjs` (önceki commit a4f4f4a = seri geometri ↔ bu sürüm, dönüşümlü, geometri = köşe + kurulum):

| görünüm | 16 thread önce | 16 thread sonra (C vert) | 16 thread sonra (Matoki vert) | 1 thread önce | 1 thread Matoki vert |
|---|---|---|---|---|---|
| 1 | 4.10 | 0.97 | 0.84 | 3.35 | 1.93 |
| 2 | 1.57 | 0.37 | 0.32 | 0.80 | 0.52 |
| 3 | 3.75 | 0.92 | 0.76 | 3.05 | 1.97 |
| 4 | 0.05 | 0.03 | 0.03 | 0.03 | 0.02 |
| 5 | 0.32 | 0.17 | 0.14 | 0.23 | 0.17 |
| 6 göl | — | — | 1.08 | — | 6.44 |

Göl, thread sayısına göre (köşe / kurulum ms): 1: 0.90 / 5.25 · 4: 0.37 / 1.42 · 8: 0.36 / 0.94 · 16: 0.38 / 0.75.
İlk sürümde kurulum 16 thread'de 12.6 ms'ydi (ortak atomik sayaç çekişmesi) → iş başına dilimle düzeldi.
Tüm kare hash'leri (görünüm × shader × MSAA) değişiklikten önceki referansla aynı (`tools/hashes.mjs`).
Tarayıcı: göl 160 FPS (motor 5.75 ms: köşe 0.18, kurulum 0.41), diğer sahneler 200 FPS (ekran sınırı).

## Sıradakiler

1. **MSAA boyama:** kenar sınıflaması için artımlı kenar adımı, örnek tamponu sıkıştırma (tam kaplı pikselde tek renk).
2. **Matoki dili:** tam matris değerleri, uniform dizileri (çoklu ışık), quad-sabit koşullarda gerçek dallanma.
2. **Fizik paralel:** renk grupları (D20) ve kumaş adımları iş sistemine.
3. **Matoki:** quad-sabit koşullarda gerçek dallanma (2. görünüm farkı), varyant önbelleği, trilinear,
   tam matris değerleri, vertex shader'ları; worker'larla birlikte thread başına önsöz önbelleği.
4. Sonra: Matoki ile yazılmış ışık/gölge (M8 Faz 4–5), varlık yükleme.
