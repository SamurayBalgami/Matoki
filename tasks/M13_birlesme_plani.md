# M13 — Xassemblianist/MatokiEngine sistemlerini bu depoya alma planı

Durum: FAZ 0, 1, 2 TAMAM (2026-10-29) → sıradaki faz 3 (ECS + animasyon + glTF). Kaynak: https://github.com/Xassemblianist/MatokiEngine (0a7b782).
Kararlar: ana depo bu depo; iki depo da aynı ekibin yönetiminde (izin sorunu yok); WebGPU en son, isteğe bağlı arka uç;
CPU simülasyon / GPU çizim (docs/DECISIONS.md D37).

## Durum tespiti (ölçüm)
- Ortak git geçmişi yok (onlar 83 commit / bizim 9). Aynı yoldaki 55 dosyanın hiçbiri aynı değil → `git merge`
  değil, **seçerek taşıma**: her alt sistem bizim kurallara (adlandırma, arena, `build.py`, `tests/test.h`) uyarlanır.
- Onların `src/` ~48 bin satır. Çekirdek tipleri aynı düzende (`Vec3`, `Mat4` sütun öncelikli), ama adlar farklı:

| onların | bizim |
|---|---|
| `vec3_add`, `vec3()`, `m_sin`, `m_sqrt`, `m_max` | `v3_add`, `v3()`, `m_sinf`, `m_sqrtf`, `m_maxf` |
| `arena_push` (225 yerde), `arena_pop_to`, `arena_sub` | `arena_alloc` / `ARENA_ARRAY`, `arena_reset`, — |
| `thread/job.h` (`job_parallel_for`) | `core/jobs.h` (`jobs_parallel_for`, belirlenimci) |
| `mtk_compile_ir`, `mtk_compile_wasm`, `g3d_*` | `matoki_compile`, `matoki_jit`, `pipe_*` |

- Lisans dosyası yok; iki depo da aynı ekibin yönetiminde, taşıma izinli. Her taşınan dosyanın başına betik
  kaynak notu yazar: "Kaynak: Xassemblianist/MatokiEngine@0a7b782 (yol), izinle uyarlandı".

## Bağımlılık haritası (onların src/)
| alt sistem | satır | bağımlı olduğu | zorluk |
|---|---|---|---|
| audio (sentez + halka tampon + AudioWorklet) | 551 | core, plat, thread | düşük |
| gfx2d | 484 | core, raster | düşük |
| font (TrueType atlas, bitmap font) | 2 424 | core, raster | düşük |
| asset (JSON, base64, glTF/GLB) | 1 814 | anim, core, gfx3d, img, plat, raster | orta |
| anim (iskelet, LBS kaplama, klip) | 951 | core, gfx3d, scene | orta |
| scene (ECS, dönüşüm, JSON kayıt) | 1 214 | asset, core | düşük |
| sim (rollback / replay) | 709 | core, phys, scene | orta (bizim phys API) |
| ui (immediate-mode UI, canlı Matoki editörü) | 1 892 | core, matoki, plat, raster | orta (Matoki API farklı) |
| mbin / masm (wasm ikili üretici, WAT birleştirici, çözücü) | 5 869 | core | orta |
| thread (MPMC kuyruk, TLS, JIT eşitleme) | 912 | bench, core, jit, masm, mbin, plat | yüksek (bizim jobs ile çakışır) |
| gpu (WebGPU RHI, komut akışı) + gpu.js | 1 458 + 28 KB JS | core, gfx3d, plat, raster, thread | yüksek; **onlarda da çalışmıyor** (M15_C: siyah tuval) |
| app (oyunlar, demolar) | 5 781 | her şey | alınmaz; demolar bizim demo.c üzerinde yeniden |

## Kararlar (önce verilmeli → docs/DECISIONS.md D37)
1. **İzin + lisans:** arkadaştan yazılı izin; iki depo için ortak bir lisans önerisi (ör. MIT) — kullanıcı ve arkadaş karar verir.
2. **WebGPU:** bu depo "GPU kullanmadan yazılım render" (AGENTS.md). WebGPU ikinci arka uç olarak alınacaksa proje
   tanımı değişir. Öneri: yazılım render ana yol kalır, WebGPU isteğe bağlı arka uç; onlarda çalışır hale gelmeden alınmaz.
3. **Tek iş sistemi:** bizim `core/jobs` (belirlenimci, bit-bit testli) kalır; onların `thread/` kullanımları buna çevrilir.
4. **Adlandırma:** bizim adlar kalır; taşıma bir eşleme tablosuyla betikle yeniden adlandırılır (uyum başlığı değil →
   iki ad kümesi depoda kalıcı olmasın). Anlam farkı olan çiftler (ör. `mat4_perspective` derinlik aralığı, `arena_push`
   sıfırlıyor mu) taşımadan önce testle doğrulanır.
5. **Derleme:** `build.py` kalır; onların `make check` kural denetimi `build.py check` olarak eklenebilir.

## Fazlar (her faz: taşı → yeniden adlandır → `-Werror` derle → onların testlerini `tests/` çatısına taşı → ölç → commit)
| faz | içerik | ~satır | çıktı / kabul |
|---|---|---|---|
| 0 ✓ | D37, yeniden adlandırma betiği (`tools/port_names.py`) + eşleme tablosu, çekirdek ekleri | — | 6 dosya (synth, json, base64, noise, ttf, gfx2d/anim) `-Werror` temiz; 148 dosyanın 112'si eksiksiz çevriliyor |
| 1 ✓ | çekirdek: `fmt_format` + `LOG/PANIC/ASSERT` (19+7+7 kullanım), `mat3_2d`, `mat3_mul_point`, `mat4_inverse`, sabit nokta, profiler; sonra img ekleri (bulanıklaştırma, yeniden boyutlandırma, normal haritası), `mesh_compute_normals`, asset/json + base64, gfx2d | ~3 000 | `port_names.py --check` 148/148; testler geçer; img ekleri bizim mip/sRGB ile aynı sonuç |
| 2 ✓ | raster'a alfa karışımı (sprite / metin için; batch2d şimdilik opak + kesik), font (TrueType atlas + bitmap) → sayfadaki HTML istatistik paneli motor içi metne | ~2 400 | metin kare hash testi; karışım SIMD = skaler |
| 3 | scene (ECS) + anim (iskelet/LBS) + glTF/GLB yükleyici → sahne 9: kaplamalı glTF modeli | ~4 000 | glTF test modeli yüklenir, kaplama bilinen pozla karşılaştırılır |
| 4 | audio (sentez + AudioWorklet), fizik olaylarına ses (çarpışma şiddeti → ses) | ~700 + JS | halka tampon testleri; sahne 8 topları ses çıkarır |
| 5 | sim (rollback/replay) bizim phys + DEM üzerine | ~700 | aynı girdi → aynı durum hash'i, geri sarma testi |
| 6 | ui (immediate-mode) + canlı Matoki editörü bizim `matoki_compile` / `engine_matoki_recompile` ile | ~1 900 | editörde hata konumlu gösterilir, derleme motoru düşürmez (D36) |
| 7 | seçerek özellik: gölge haritası (`g3d_set_shadow`), ertelenmiş çizim, JIT WAT raster çekirdekleri — önce/sonra ölçümle | değişken | yalnız ölçülen kazanç varsa alınır |
| 8a | WebGPU: C'de komut akışı (`DrawRec` → komut tamponu) + ince `web/gpu.mjs`; yoksa yazılım render'a dönüş | — | düz renkli küp iki yolda aynı (piksel farkı < eşik) |
| 8b | Matoki → WGSL hedefi; lit / lit_hq / water / sky iki arka uçta | — | 10 görünüm × shader görüntü farkı testi |
| 8c | GPU'da doku, MSAA, gölge, okyanus FFT görüntüsü; CPU/GPU boru hattı örtüşmesi; sayfada CPU ms / GPU ms / kare ms | — | 1080p önce/sonra ölçüm tablosu (bugün 30–52 ms) |

Her faz ayrı commit (ve istenirse ayrı oturum / dal); hash'ler (`tools/hashes.mjs`) mevcut görünümlerde değişmemeli.

## Faz 1 notları (2026-10-29)
- Çekirdek: `core/printf.c` (fmt_format, LOG/PANIC/ASSERT; `log_printf` satır sonu ekler, `sys_panic` artık
  `_Noreturn`), `mat4_inverse`, `mat3_identity/mul/2d/mul_point`, `core/simd.h`, `core/fixed.h`, `core/profiler.*`
  (yalnız ana thread; onlarınki thread başına tablo tutuyordu, bizim worker'lar ayrı modül örneği).
- `mat3_mul` işaretçi alır (bizim Mat3 sözleşmesi); fizik eklemlerindeki yerel kopya kaldırıldı, toplama sırası aynı
  (0'dan başlar) → fizik testleri ve kare hash'leri değişmedi.
- Taşınan: `asset/json`, `asset/base64`, `img/filter` (onların image.c'sinden; mip ve doku kurulumu alınmadı, bizimki
  sRGB-doğru), `gfx2d/anim`, `gfx2d/camera2d`, `gfx2d/tilemap`, `gfx2d/batch2d` (bizim ertelenmiş raster'a yeniden
  yazıldı; durum üçgen başına saklandığı için doku değişiminde flush gerekmez), `mesh_compute_normals`.
- **Onların kodunda bulunan hata:** tilemap'te sol/üst kenar toleranssız, sağ/alt kenar `-eps` ile karoya çevriliyordu;
  genişliği karo katını eps'ten az aşan kutu sağ duvara yaslanınca soldaki katı karoya giriyordu. Bizim RNG'yle koşan
  fuzz testi buldu (onların PCG dizisi bu durumu üretmemiş). Dört kenar aynı toleransa çekildi + gerileme testi.
- **Bilinen sınır:** JSON sayıları doğru yuvarlanmıyor (mantis × 1e22 adımları). Python'la üretilmiş 412 sayıda 301 tam,
  en çok 3 ULP (onların 2000 sayılık ölçümü ≤ 2 ULP). glTF verisi f32'ye indiği için görünmez; gerekirse Eisel–Lemire.
- Onların testleri host libc kullanıyordu (stdio, popen, strlen): `test_printf`, `test_json`, `test_gfx2d` wasm'a
  uyarlandı (Python karşılaştırması → gömülü kesin bit tablosu); `test_m13_port` yeni (filtre özellikleri, base64 RFC 4648,
  sprite piksel çıktısı, profiler, mat4_inverse).
- Ölçüm: 167/167 test (worker'lı ve worker'sız), 10 görünüm kare hash'i 1T ve 16T'de değişmedi, `port_names.py --check`
  148/148, engine.wasm 394 061 → 393 353 bayt (yeni modüller henüz demoda kullanılmıyor; fark `_Noreturn sys_panic`).

## Faz 2 notları (2026-10-29)
- Raster: `BlendMode` NONE / ALPHA / PREMUL / ADD, `raster_set_blend`; tamsayı formül (`div255`), 4 yazma yolunda
  aynı. Test `test_blend`: 3 mod × 256 alfa bağımsız f64 referansla 0 fark; 400 üst üste yarı saydam üçgen
  skaler / SIMD × tek örnek / MSAA × 1T / tüm thread'ler → 0 uyuşmazlık. batch2d artık `B2D_ALPHA`, `B2D_ADD`.
- Font: `font/ttf`, `font/raster` (glif), `font/atlas`, `font/text`, gömülü Matoki Sans. `build.py test` artık
  `tests/assets/*`'ı gömer (test_font.ttf). `test_font` (5 test, DejaVu sistem testi dosya sistemi gerektirdiği için
  çıkarıldı), `test_text` (5 test; PPM → kare hash'i 543713094 sabitlendi). 1000 karakter @ 16 px: 0,29 ms.
- Motor içi panel `gfx3d/hud`: 7 satır (fps, kare / geometri / raster / çözümleme ms, üçgen, piksel, fizik, bellek),
  U tuşu, sayfa varsayılan açar (`?nohud` kapalı başlatır). Maliyet (`tools/bench_hud.mjs`, 16 thread):
  640×360 0,60 ms · 1280×720 0,72 ms · 1920×1080 1,16 ms. Atlas ayrı arenada (`hud_mem`).
- **Ölçüm (tek thread, 640×360, 10 görünüm, `tools/bench_ab.mjs` dönüşümlü A/B, 11 tur):**

| karşılaştırma | oranların medyanı | en iyi sürelerin oranı |
|---|---|---|
| A/A (aynı dosya, gürültü tabanı) | 1,005 / 0,981 / 1,007 | 1,022 |
| önce → yalnız raster karışımı | 1,015 – 1,017 | 1,009 – 1,044 |
| önce → faz 2 tamamı | 1,011 – 1,050 | 1,039 – 1,046 |
| faz 2 → önce (ters sıra) | 0,971 – 0,973 | 0,971 – 0,975 |

  Sonuç: karışımsız sahnelerde ≈ %2,5–3 yavaşlık, yarısı raster (gürültü tabanında), yarısı font / panel kodunun
  eklenmesi (panel kapalıyken bile; atlas'ı ayrı arenaya almak farkı kapatmadı → kod yerleşimi). Kare hash'leri 10
  görünümde 1T ve 16T'de değişmedi. Denenen ve etkisiz çıkanlar: `t->blend`'i döngü dışına almak, karışımı
  `noinline` yapmak (çıktı bayt bayt aynıydı).

## Riskler
- Anlam farkları sessiz hata üretebilir (aynı adlı ama farklı davranan fonksiyonlar) → faz 0'da çift çift test.
- `thread/` ↔ `core/jobs` çevirisi belirlenimciliği bozabilir → 1T ↔ 16T hash testi her fazda.
- Onların kodu ileride değişir → taşınan sürümün commit'i (0a7b782) dosya başına not edilir; sonraki senkron elle.
