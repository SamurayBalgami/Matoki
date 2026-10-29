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
| `uniform t: texture2d @clamp;` | Doku. `@clamp` verilmezse wrap (genişlik/yükseklik 2'nin kuvveti olmalı). `@mipmap`: `sample` seviyeyi quad türevlerinden seçer. `@bilinear`: seviye içinde çift doğrusal filtre. Öznitelikler birleşebilir. |
| `uniform M: mat4;` / `mat3` | Sütun-öncelikli matris (C `Mat4`/`Mat3` ile aynı yerleşim, 64 / 36 bayt). Yalnızca `M * v` içinde kullanılır. |
| `varying ad: f32 / vecN;` | Rasterizer'dan gelen interpole değer. Ardışık yuvalara yerleşir; `@slot(N)` ile başlangıç yuvası seçilir. Toplam ≤ 16 yuva. |
| `fn ad(p: tip, ...) -> tip { ... }` | Fonksiyon. Her çağrıda satır içine alınır; özyineleme yasak. Gövde `return <ifade>;` ile bitmeli, başka yerde `return` olamaz. |
| `fn frag() -> vec4` | Giriş noktası. Dönen `vec4` (0..1) `clamp → ×255 + 0.5 → kesme` ile RGBA8'e paketlenir (`pack_rgb` ile aynı). |

Uniform yerleşimi derleyiciden sorgulanır: `matoki_find_uniform(p, "ad")->offset`. C struct'ı aynı sırayla
yazılırsa (örnek: `LitUniforms` ↔ `lit.mtk`) aynı uniform bloğu hem C hem Matoki shader'ına verilebilir.

## Tipler ve ifadeler

- Tipler: `f32`, `i32`, `bool`, `vec2`, `vec3`, `vec4`; yalnızca uniform: `texture2d`, `mat3`, `mat4`.
- Matris: `M * v` (`mat4 * vec4`, `mat3 * vec3`) → vektör; sıra `mat4_mul_v4` ile aynı (bit-bit). Matris yerele,
  parametreye atanamaz; `M * M`, `v * M`, transpoz yok (D25).
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
- `discard;`: o anki yolda (iç içe if koşullarının hepsi doğru olan lane'ler) pikseli atar; renk ve derinlik
  yazılmaz. SPMD gereği kod akmaya devam eder; `discard`'dan sonraki atamalar zararsızdır. Fonksiyon içinde de
  geçerlidir (çağrı yerinin yolu sayılır). Raster sözleşmesi: `ShadeQuad.kill` (D24).

## Yerleşikler

| Yerleşik | Not |
|---|---|
| `vec2/3/4(...)` | bileşenleri birleştirir; tek skaler → yayılım |
| `f32(i)`, `i32(x)` | dönüştürme; `i32` doygun keser (NaN → 0) |
| `abs min max clamp` | f32/vecN (ikinci/üçüncü argüman skaler olabilir) veya i32 |
| `floor ceil trunc round fract sqrt` | `round` = en yakın çift |
| `sin cos` | `m_sinf` / `m_cosf` (C) ile **bit-bit aynı** polinom |
| `exp2 log2 exp log pow` | `m_exp2f` / `m_log2f` (C) ile bit-bit aynı, göreli hata ~1e-6. `log2(x<0)` = NaN, `log2(0)` = -inf, `exp2` [-150, 128]'e kenetli; `pow(x, y)` = `exp2(y·log2 x)` (y skaler veya aynı boyut) |
| `mix(a, b, t) step(e, x) smoothstep(e0, e1, x) mod(x, y)` | GLSL anlamı; `mix = a + (b - a) * t` |
| `dot cross length distance normalize reflect` | `normalize` sıfır vektörde NaN verir (GLSL gibi) |
| `select(c: bool, a, b)` | dalsız seçim; her tipte |
| `sample(tex, uv: vec2) -> vec4` | RGBA 0..1; wrap / `@clamp`. Varsayılan en yakın komşu, seviye 0. `@mipmap` → seviye `mip_level`'dan (`ref_sample_tex_mip` ile bit-bit), `@bilinear` → `ref_sample_bilinear` ile bit-bit |
| `sample_level(tex, uv, lod: i32) -> vec4` | verilen seviye (`[0, son]`'a kenetli; negatif → 0), dokunun filtre özniteliğiyle |
| `mip_level(tex, uv) -> i32` | `ref_mip_level` kuralı: `floor(log2(ρ))`, ρ = texel/piksel (quad türevleri); quad içinde sabit |
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

## Vertex aşaması (`fn vert() -> vec4`, D29)

```
uniform model: mat4;           // VertUniforms ile aynı sıra: model, view_proj, normal_mat, time, params...
uniform view_proj: mat4;
uniform normal_mat: mat3;
uniform time: f32;
attribute position: vec3;      // köşe girdisi: yuvalar bildirim sırasıyla (ATTR3D_POS/NRM/UV) ya da @slot(N)
attribute normal: vec3;
attribute texcoord: vec2;
varying wpos: vec3;            // fragment aşamasıyla aynı yuvalar
varying nrm: vec3;
varying uv: vec2;

fn vert() -> vec4 {
    let w = model * vec4(position, 1.0);
    wpos = w.xyz;              // vert'te varying'ler yazılabilir yereldir (0 ile başlar, if'lerde birleşir)
    nrm = normal_mat * normal;
    uv = texcoord;
    return view_proj * vec4(wpos, 1.0);    // klip uzayı konumu
}
```

- Derleme: `MatokiOptions.stage = MATOKI_STAGE_VERT`. Bir dosyada `frag` ve `vert` birlikte olabilir; uniform bloğu
  tektir, bu yüzden demo vertex shader'larını ayrı dosyada tutar (`shaders/std_vert.mtk`, `wave_vert.mtk`).
- Lane'ler 4 bağımsız köşedir: `dFdx/dFdy/fwidth`, `frag_coord`, `mip_level`, `@mipmap` ile `sample` ve
  `discard` derleme hatası. `sample_level` serbest (dokudan yükseklik okuyup köşe kaydırma).
- `attribute` yalnız vert'te okunur; varying'e yalnız vert'te yazılır.
- ABI: `VertexFn(VertexIn*, uniforms, VertexOut*)` — wasm imzası ShadeFn ile aynı; önsöz ve thread önbelleği aynen.
- `std_vert.mtk` C kâhini `vert_std_c` ile bit-bit aynı (test + kare hash'leri).

## Üretilen modül: shade, prelude, shade_fast (D26)

- **Çizim-sabitler önsözde:** yalnızca uniform/doku/sabitten türeyen pahalı değerler `prelude(uniforms)`'da bir
  kez hesaplanıp önbelleğe yazılır; `shade_fast` onları okur. Raster önsözü (önsöz, uniforms) değiştikçe çağırır.
- **Quad-sabitler skaler:** türevler ve onlardan türeyenler (mip seviyesi, seviye boyutları) wasm skaler
  yerellerinde; vektöre gerektiğinde bir kez yayılır.
- `shade` tek başına doğrudur (testler, araçlar); `shade_fast` yalnızca aynı uniform'larla `prelude`'dan sonra.

## Performans (lit.mtk ↔ C SIMD shader, aynı çıktı) — Matoki demo varsayılanı

Kare süresi oranı Matoki / C (eşli ölçüm `node tools/bench.mjs --pair`, iki tur; < 1 Matoki hızlı):

| görünüm | 640×360 | 1280×720 |
|---|---|---|
| 1 başlangıç | 0.974 / 0.942 | 0.919 / 0.978 |
| 2 ızgara içi (mip hep 0) | 1.023 / 1.029 | 1.035 / 1.061 |
| 3 uzaktan | 0.966 / 0.930 | 0.980 / 0.995 |
| 4 mip zemini | 0.918 / 0.890 | 0.951 / 0.987 |

2. görünümde C'nin dallı mip seçimi erken çıkar; Matoki dalsız olduğu için tüm zinciri yapar.

## Henüz olmayanlar

Seviyeler arası (trilinear) filtre, tam matris değerleri (yerel, `M * M`), uniform dizileri, quad-sabit
koşullarda gerçek dallanma (C'nin erken çıkışı gibi), uniform özelleştirme ve varyant önbelleği, vertex
aşamasında kullanıcı tanımlı köşe girdileri (şimdilik standart konum/normal/uv).
