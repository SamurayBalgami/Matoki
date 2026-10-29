# M11 — fizik doğrulama ve dalgalar

## Faz 1: üst üste bırakılan toplar (sahne 8) — bitti
- tests/test_phys_drop.c: olay tabanlı kesin 1B çözüm (aynı ivmeli toplar arası göreli hareket doğrusal) ↔ motor.
- Düzeltmeler D31. 240 Hz sonuç (ilk zemin çarpmasından sonraki 1,5 s'deki en yüksek nokta, m):

| durum | top | kesin | motor |
|---|---|---|---|
| eşit (2,2,2 → 1,1,1 kg) | üst | 2,439 | 2,434 |
| ağır üstte (1,2,4) | üst | 2,205 | 2,196 |
| ağır altta (4,2,1) | üst | 7,283 | 7,255 |

- Sıçrama oranı (en üstteki): ağır altta 2,26 > eşit 0,71 > ağır üstte 0,63. Eşit kütlede oran yukarı doğru artar.

## Faz 2: istatistiksel okyanus (sahne 7) — bitti
- src/phys/ocean.c, tests/test_ocean.c, D33.

## Faz 3: gezegen çekimi ve serbest düşme denklemleri — bitti
- D32, tests/test_phys_fall.c: sabit g (t, v, d, ortalama hız), değişken g (kesin hız, Kepler süresi),
  kısa mesafe yaklaşımı √(2GMd/r²) yalnızca d ≪ r'de doğru (d = r'de %41 fazla), enerji sapması kuramsal sınırda.
## Faz 4: değen yığınlar — Hertz DEM (D34) — bitti
Sıralı darbe modeli (Faz 1) eşit kütlede topları kendi yüksekliğine döndürüyordu. DEM, e = 0,9, en üstteki topun
sekme / bırakılış oranı (yüzeyden):

| e | eşit (2,2,2) | ağır üstte (1,2,4) | ağır üstte (1,4,16) | ağır altta (4,2,1) |
|---|---|---|---|---|
| 0,95 | 1,26 | 1,26 | 1,01 | 2,67 |
| 0,90 | 1,18 | 1,19 | 0,99 | 2,41 |
| 0,80 | 1,07 | 1,10 | 0,98 | 1,97 |
| 0,70 | 0,99 | 1,03 | 0,97 | 1,62 |

Eşit yığında alttaki 0,19–0,25; ağır üstteyken alttaki hafif 0,08–0,16.

## Faz 5: çözünürlük (D35) — bitti
- 16 thread, HQ + MSAA, ms/kare (tools/bench_res.mjs):

| çözünürlük | g1 | g2 | g3 | g4 | g5 | g6 | g7 |
|---|---|---|---|---|---|---|---|
| 640×360 | 5,85 | 6,53 | 5,78 | 4,25 | 3,76 | 7,23 | 3,95 |
| 1280×720 | 19,08 | 23,30 | 13,46 | 17,04 | 13,46 | 22,58 | 14,20 |
| 1920×1080 | 42,00 | 52,26 | 29,58 | 37,79 | 30,54 | 46,72 | 32,53 |

## Faz 6: tam yeniden test — bitti
- 126 test (worker'lı ve worker'sız). Hash: HEAD'e göre yalnızca görünüm 5 (direk malzemesi) ve 7 (DEM) değişti.
- Ölçüm görünümleri 8–10 eklendi (sahne 3, 4, 5); tüm sahneler 960×540'ta gözle ve tarayıcıda `?prof` ile koşuldu
  (8 sahne, konsol hatası yok, 676×380'de tavan 118–207 FPS).
- Sahne 8: toplar 2 mm aralıklı (D34); DEM kesin olay çözümüyle doğrulandı, alt adımda yakınsadı.