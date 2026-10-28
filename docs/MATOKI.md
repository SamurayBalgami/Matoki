# Matoki shader dili (v0)

Matoki, MATOKI motorunun kendi shader dilidir. Shader **tek bir piksel** için yazılır; derleyici onu
2×2 quad'ın 4 pikseli için **wasm SIMD** koduna çevirir (ISPC tarzı SPMD). Derleyici motorun içinde,
çalışma anında çalışır: kaynak → IR → wasm modülü → JIT → `ShadeFn` işaretçisi.

```
kaynak (.mtk) → lexer → parser (AST) → sema (tipler, isimler, yerleşim) → lower (IR)
             → [sabit katlama + CSE kurulum sırasında] → DCE → codegen (wasm SIMD) → JIT
                                                         └→ ir_eval (skaler kâhin, testler)
```

## Örnek

```
uniform tex: texture2d @clamp;
uniform tint: vec3;
uniform time: f32;

varying wpos: vec3;     // yuva 0..2
varying nrm: vec3;      // yuva 3..5
varying uv: vec2;       // yuva 6..7

fn wave(x: f32) -> f32 { return sin(x * 3.0 + time) * 0.5 + 0.5; }

fn frag() -> vec4 {
    let c = sample(tex, uv);
    var k = wave(wpos.x);
    if (nrm.y > 0.5) { k = k * 2.0; }
    for i in 0..3 { k += 0.1 * f32(i); }
    return vec4(c.rgb * tint * k, 1.0);
}
```

Gerçek bir örnek: [`shaders/lit.mtk`](../shaders/lit.mtk) — C referansı `shade_lit_tex` ile bit-bit aynı çıktı verir.

## Bildirimler

| Sözdizimi | Anlam |
|---|---|
| `uniform ad: tip;` | Tüm quad için sabit. Uniform bloğunda bildirim sırasıyla, 4 bayt hizalı ve boşluksuz yerleşir: `f32`/`i32` 4 bayt, `vecN` 4N bayt, `texture2d` 4 bayt (`Texture*`, wasm32). `bool` uniform yok (`i32` kullanın). |
| `uniform t: texture2d @clamp;` | Doku. `@clamp` verilmezse wrap (genişlik/yükseklik 2'nin kuvveti olmalı). `@mipmap` ve `@bilinear` ayrıştırılır ama henüz hata verir (M8 Faz 2). |
| `varying ad: f32 / vecN;` | Rasterizer'dan gelen interpole değer. Ardışık yuvalara yerleşir; `@slot(N)` ile başlangıç yuvası seçilir. Toplam ≤ 16 yuva. |
| `fn ad(p: tip, ...) -> tip { ... }` | Fonksiyon. Her çağrıda satır içine alınır; özyineleme yasak. Gövde `return <ifade>;` ile bitmeli, başka yerde `return` olamaz. |
| `fn frag() -> vec4` | Giriş noktası. Dönen `vec4` (0..1) `clamp → ×255 + 0.5 → kesme` ile RGBA8'e paketlenir (`pack_rgb` ile aynı). |

Uniform yerleşimi derleyiciden sorgulanır: `matoki_find_uniform(p, "ad")->offset`. C struct'ı aynı sırayla
yazılırsa (örnek: `LitUniforms` ↔ `lit.mtk`) aynı uniform bloğu hem C hem Matoki shader'ına verilebilir.

## Tipler ve ifadeler

- Tipler: `f32`, `i32`, `bool`, `vec2`, `vec3`, `vec4`, `texture2d` (yalnızca uniform).
- Literaller: `1.5`, `2e-3`, `7`, `0xFF`, `true`. Tamsayı literali f32 beklenen yerde f32'ye döner
  (`x * 2` geçerli); başka örtük dönüşüm yok (`f32(i)`, `i32(x)` kullanın).
- Yereller: `let x = e;` (değişmez), `var x = e;` / `var x: vec3;` (sıfırla başlar). Gölgeleme serbest.
- Aritmetik `+ - * /`: f32/vecN bileşen bazlı; vektör ⊕ skaler yayılır. `i32` için `+ - *` (bölme yok).
  `%` yok, `mod(x, y)` kullanın.
- Karşılaştırma `< <= > >= == !=`: iki aynı tip skaler → `bool`. Mantık: `&& || !` (kısa devre YOK:
  SPMD'de iki taraf da hesaplanır). Bit: `& | ^ << >> ~` (i32; `>>` aritmetik).
- Swizzle: `v.xyz`, `c.rgba`, `v.zx`; atamada da (`c.yz = vec2(1.0, 0.5);`, tekrarsız).
- Atama: `=`, `+=`, `-=`, `*=`, `/=`.

## Kontrol akışı (SPMD)

- `if c { } else if c2 { } else { }`: koşul `bool`. Quad'ın lane'leri farklı dallara gidebilir. İki dal da
  çalıştırılır, değişen yereller `select` ile birleştirilir. Yan etki olmadığı için bu, sonucu değiştirmez.
- `for i in A..B { }`: A ve B tamsayı sabiti, en fazla 1024 tur. Derleme anında tamamen açılır; `i` bir `i32` sabitidir.
- Satır içi çağrı + açılan döngü toplamı 4096 ile sınırlı ("shader çok büyük" hatası).

## Yerleşikler

| Yerleşik | Not |
|---|---|
| `vec2/3/4(...)` | bileşenleri birleştirir; tek skaler → yayılım |
| `f32(i)`, `i32(x)` | dönüştürme; `i32` doygun keser (NaN → 0) |
| `abs min max clamp` | f32/vecN (ikinci/üçüncü argüman skaler olabilir) veya i32 |
| `floor ceil trunc round fract sqrt` | `round` = en yakın çift |
| `sin cos` | `m_sinf` / `m_cosf` (C) ile **bit-bit aynı** polinom |
| `mix(a, b, t) step(e, x) smoothstep(e0, e1, x) mod(x, y)` | GLSL anlamı; `mix = a + (b - a) * t` |
| `dot cross length distance normalize reflect` | `normalize` sıfır vektörde NaN verir (GLSL gibi) |
| `select(c: bool, a, b)` | dalsız seçim; her tipte |
| `sample(tex, uv: vec2) -> vec4` | en yakın komşu, seviye 0, wrap / `@clamp`; RGBA 0..1 |
| `texel_fetch(tex, x: i32, y: i32) -> i32` | ham texel (0xAABBGGRR); koordinatlar kenetlenir |
| `tex_size(tex) -> vec2` | (genişlik, yükseklik) |
| `bitcast_f32(i32)`, `bitcast_i32(f32)` | bit düzeyinde yeniden yorum (gölge haritası derinliği vb.) |
| `dFdx(x) dFdy(x) fwidth(x)` | kaba quad türevi: `v[lane1] - v[lane0]`, `v[lane2] - v[lane0]`; uniform → 0 |
| `frag_coord() -> vec3` | (x + 0.5, y + 0.5, z) |

## IR ve doğruluk

IR (`src/matoki/ir.h`) düz SSA'dır: her değer 4 lane, tipi `F32` veya `I32` (bool = tam maske). Semantik
wasm SIMD ile birebir tanımlıdır (`fmin`/`fmax` NaN yayar, −0 < +0; `f2i` doygun; kaydırma `& 31`).
`ir_emit` sabitleri `ir_eval_lane` ile katlar, yani katlama sonucu çalışma anındakiyle bit-bit aynıdır. CSE de
kurulum sırasında yapılır.

- **Kâhin:** `ir_eval` IR'ı skaler C ile çalıştırır. `tests/test_matoki_back.c` 500 rastgele programı katlama
  açık ve kapalı olarak kurar, sonra JIT çıktısıyla karşılaştırır.
- **Tek belirsizlik NaN yük/işaret bitleridir:** wasm bunları sabitlemez. Test, float→int bitcast öncesinde
  NaN'ı kanonikleştirir. `select`'in F32 maskesi tam olmalıdır (lower yalnızca bunu üretir).

## Henüz olmayanlar

`@mipmap` / `@bilinear` / `sample_level` (M8 Faz 2), `mat3`/`mat4` uniform'ları ve `mat * vec`, `discard`,
`exp`/`log`/`pow`, uniform dizileri, uniform değerlerin skaler hesaplanması (şu an her şey v128),
uniform özelleştirme ve varyant önbelleği, vertex shader'ları.
