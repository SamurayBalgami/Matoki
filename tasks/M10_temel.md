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

## Sıradakiler

1. **Render çekirdeği (4c):** SIMD vertex aşaması + üçgen kurulumu; boyamada raster payını (flat) küçültmek.
2. **Paralel raster:** worker'lar + SharedArrayBuffer (COOP/COEP sunucu hazır), karo başına iş.
3. **Matoki:** uniform'dan türeyen değerleri çizim başına bir kez hesaplama (önsöz fonksiyonu), varyant önbelleği,
   trilinear, tam matris değerleri, vertex shader'ları.
4. Sonra: Matoki ile yazılmış ışık/gölge (M8 Faz 4–5), varlık yükleme.
