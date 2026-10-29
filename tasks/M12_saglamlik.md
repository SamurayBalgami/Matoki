# M12 — temel sağlamlık

Plan: bellek → render çekirdeği → fizik → Matoki; her adım test + ölçüm. Kararlar D36.

## Faz 1: bellek — bitti
- tests/test_robust.c: arena_try / taşma, 64 KB raster arenasında 5 ara flush ile her piksel tam 200 kez, tablo
  sığmazsa atlama, PIPE_MAX_DRAWS + 25 → 25 atlanan, kare belleği dolu, paralel taşma (sığanlar çizilir).

## Faz 2: render çekirdeği — bitti
- Kesin referanslı fuzz (0 fark / 2,66 M piksel), SIMD = skaler, çöp mesh (nöbetçiler sağlam).
- Hash: 10 görünüm × shader × MSAA değişmedi.

## Faz 3: fizik — bitti
- tests/test_phys_robust.c: NaN izolasyonu bit-bit, bozuk girdi, hız sınırı, 300/50 gövde yığını uyur, 12'lik kule,
  yuvarlanma direnci kesin formül (iki çözücü).

| bench_phys (soft, uyku açık) | önce ms (oturmuş) | sonra | uyanık önce → sonra |
|---|---|---|---|
| yığın 100 | 0,158 | 0,007 | 96 → 0 |
| yığın 500 | 0,924 | 0,533 | 496 → 0 |
| yığın 2000 | 6,99 | 7,17 | 1996 → 1996 (600 adımda hâlâ düşüyor) |

## Faz 4: Matoki — bitti
- tests/test_matoki_fuzz.c: 4800 mutasyon (4296 hata, hepsi konumlu), 3000 jeton çorbası, derinlik 20 000, 3000 satır.