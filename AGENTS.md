# MATOKI — ajan kuralları

GPU kullanmadan tarayıcıda çalışan yazılım render motoru. C11, freestanding, `clang --target=wasm32 -nostdlib`.

## Derleme
- `python build.py` → `build/engine.wasm` (+ `web/engine.wasm`)
- `python build.py test` → testler wasm'a derlenir, `node tests/run.mjs` ile koşar
- `python build.py serve` → `http://localhost:8080/3d.html` (COOP/COEP başlıklı)
- `node tools/bench.mjs build/engine.wasm [--frames N] [--res WxH]` → sabit görünümlerde ms + framebuffer hash
  (performans değişikliğinden önce/sonra çalıştır; hash değişmemeli ya da değişimin nedeni yazılmalı)
- `node tools/bench.mjs --mt` → tek thread ↔ tüm thread'ler; `python build.py test --threads 0` → worker'sız testler
- Motor paylaşılan bellek kullanır (D27): tarayıcıda sayfa COOP/COEP ile sunulmalı (`serve` yapar); JS platform katmanı `web/mt.mjs`
- Gereken araçlar: LLVM (clang + wasm-ld), Node ≥ 22, Python 3. `make` yok, host C derleyicisi gerekmez.

## Kurallar
- `-Wall -Wextra -Werror`. `src/` altında libc başlığı yasak (`<stdint.h>` dahil → `core/types.h`).
- malloc yok; her bellek bir `Arena*`'dan. Tipler `u8..u64 i8..i64 f32 f64 b32 usize`.
- İsimler `modul_fonksiyon()`, tipler `PascalCase`, sabitler `UPPER_SNAKE`. Her dosyanın başında 3-5 satır açıklama.
- `*_wasm.c` dosyaları wasm'a özel kodu taşır (import'lar, ileride SIMD).
- Optimizasyon iddiası = ölçüm (öncesi/sonrası tablo).
- JS yalnızca ince platform katmanıdır: saf ES modülü, npm/bundler yok, motor mantığı JS'e girmez.
- Mimari kararlar `docs/DECISIONS.md`'ye yazılır.
- Testler `tests/*.c`: `TEST(isim) { CHECK(...); CHECK_NEAR(a, b, eps); }`; kayıt tablosunu build.py üretir.

## Yerleşim
- `src/core` tipler, arena, matematik, günlük · `src/raster` rasterizer + shader ABI + C referans shader'lar
- `src/img` doku/mip/prosedürel · `src/gfx3d` 3D boru hattı, kamera, mesh, demo
- `src/matoki` shader dili derleyicisi · `src/jit` JIT yükleme · `src/phys` fizik motoru · `shaders/` .mtk dosyaları
- `web/` tarayıcı sayfaları · `tests/` testler · `tasks/` görev dosyaları ve notları · `docs/` belgeler
