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