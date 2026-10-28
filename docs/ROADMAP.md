# MATOKI — yol haritası

Depo 2026-10-27'de sıfırdan kuruldu. M8 (3D) görev dosyası önceki kilometre taşlarının hazır olduğunu varsayıyor;
eksik olanlar aşağıda öncelik sırasıyla listeleniyor.

| # | Kilometre taşı | Durum |
|---|---|---|
| 1 | Çekirdek: tipler, arena, matematik, günlük, build.py, wasm test koşucusu | ✅ |
| 2 | Raster v0: 24.8, top-left, quad'lar, early-z, perspektif-doğru, yardımcı lane'ler | ✅ (skaler) |
| 3 | 3D boru hattı: kırpma, guard band, culling, kamera, demo sahne 1–2 | ✅ (M8 Faz 1 temeli) |
| 4 | Raster performansı: 64×64 karo binning, wasm SIMD quad yolu, benchmark (`tools/bench.mjs`) | ✅ raster 1.25–2.6× |
| 4b | Shader maliyeti: mip seviye tablosu + SIMD C lit shader | ✅ lit kare 1.7–2.5× |
| 4c | SIMD vertex aşaması + üçgen kurulumu, hızlı fb_clear (artık raster tarafı baskın) | ⏳ |
| 5 | **Matoki** v0: lexer → parser → sema → IR (katlama + CSE) → wasm SIMD codegen, ir_eval, diferansiyel test, JIT | ✅ (`docs/MATOKI.md`) |
| 5b | Matoki: mat3/mat4 ✅, discard ✅, exp/log/pow ✅, önsöz (çizim başına) ✅, skaler quad-sabitler ✅; uniform özelleştirme + varyant önbelleği | ⏳ |
| 6 | M8 Faz 2'nin Matoki kısmı: dFdx/dFdy/fwidth, texel_fetch/bitcast, @mipmap, mip_level, sample_level, @bilinear | ✅ (C ile bit-bit) |
| 7 | PNG (inflate/deflate), JSON, glTF/GLB, OBJ, .mmsh (M8 Faz 3) | ⏳ |
| 8 | Bitmap font + immediate-mode UI | ⏳ |
| 9 | İş sistemi + worker'lar + paralel raster (M6_2) | ✅ (D27; 720p 2.6–6.5×) |
| 10 | M8 Faz 4–7: ışıklandırma/PBR, gölgeler, prosedürel şehir, vertex shader'lar | ⏳ |
| 11 | M9 fizik Faz 1: rijit cisimler, küre/kutu/düzlem, SAT, SAP, warm start'lı sequential impulse, demo sahne 3 | ✅ (`tasks/M9_physics.md`) |
| 12 | M9 Faz 2: soft step (varsayılan), adalar + uyku, fizik benchmark'ı (`tools/bench_phys.mjs`) | ✅ oturmuş sahneler 30–110× |
| 12b | M9 Faz 2b: kısıt grafiği renklendirme + SIMD çözücü (bit-bit eşit) | ⏳ |
| 13 | M9 Faz 3a: raycast, raycast tekerlekli araç, karakter kontrolcüsü, demo sahne 4 (sürülebilir araba) | ✅ |
| 13b | M9 Faz 3b: eklemler, kapsül/dışbükey (GJK/EPA), statik mesh + BVH | ✅ |
| 13c | M9 Faz 4: CCD, mesh iç kenarları, gerçek hull eylemsizliği | ✅ |
| 13d | M9 Faz 5: XPBD yumuşak cisim — kumaş (bayrak) ve jöle, rijitle iki yönlü | ✅ |
| 13e | M9 Faz 6: kısıt grafiği renklendirmesi + SIMD temas çözücüsü (skalerle bit-bit) | ✅ |
| 13f | M9 Faz 7: dinamik dünya BVH'si (geniş faz / raycast / CCD / yumuşak aday), SAP ile bit-bit eşit | ✅ |
| 13g | M9 Faz 8: kumaş öz-çarpışması + rijit ↔ kumaş teması (mermi bayrakta yakalanır) | ✅ |
| 13h | M9 Faz 9: SIMD dar faz, kinematik gövdeler, öz-çarpışma komşu listesi, kumaş yırtılması | ⏳ (M10 sonrası) |
| 14 | M10 Faz 1: tarayıcıda kare profili (`3d.html?prof`) — boyama baskın | ✅ (`tasks/M10_temel.md`) |
| 14b | M10 Faz 2: Matoki eksikleri (mip, bilinear, discard, matris, exp/log/pow); lit.mtk C ile bit-bit | ✅ |
| 14c | M10 Faz 3: Matoki demo varsayılanı (önsöz, skaler quad-sabitler, seviye tabloları); C ile bit-bit | ✅ |
| 14d | M10 Faz 4: paralel raster (9), tek/çok thread bit-bit | ✅ |
| 14e | M10 Faz 5: geometri aşaması paralel/SIMD (4c), fizik iş sistemine | ⏳ |
