# MATOKI — mimari kararlar

## D1. Derleme ve test altyapısı (2026-10-27)
Windows'ta `make` ve host C derleyicisi/libc yok. `Makefile` yerine `build.py` (yalnızca Python standart kütüphanesi).
Testler motorla aynı bayraklarla **wasm32'ye** derlenir ve node'da koşar; böylece test edilen kod gönderilen kodla
birebir aynıdır. Bedeli: host'ta ASan/UBSan yok. Fuzz testleri gerektiğinde Linux/WSL'de host derlemesi eklenecek.

## D2. Derinlik konvansiyonu
WebGPU/D3D tarzı: sağ el dünya, kamera −Z'ye bakar, klip derinliği 0 (yakın) .. 1 (uzak), `DEPTH_LESS`,
temizleme 1.0. Reverse-Z (f32 derinlikte hassasiyet avantajı) ölçülmeden açılmayacak; ölçüm Faz 1'e açık iş.

## D3. Ön yüz ve sarım
Ön yüz = NDC'de saat yönünün tersi (CCW). Raster, y aşağı ekran uzayında işaretli alanı hesaplar; ön yüzler negatif
alan verir. Kenar fonksiyonları pozitif alan beklediğinden raster gerektiğinde köşe 1 ile 2'yi yer değiştirir.

## D4. Varying perspektif sözleşmesi
`RasterVert.var[]` HAM değerdir. Raster `var*inv_w` ve `inv_w`'yi ekran uzayında doğrusal interpole edip piksel
başına böler. `z` (NDC) ekran uzayında doğrusaldır, bölünmez. Öznitelik düzlemleri yakalanmamış (float) köşe
konumlarıyla kurulur; kaplama 24.8 yakalanmış konumlarla. Kaplama kararı ile interpolasyon birbirinden bağımsızdır.

## D5. Yardımcı lane'ler
Kapsanmayan quad lane'lerinin varying'leri aynı düzlem denklemiyle dışdeğerlenir (§M8 2.2). Kaba türevler
(`v[1]-v[0]`, `v[2]-v[0]`) her quad'da geçerlidir. Bedeli: kısmi quad'larda kullanılmayan lane'ler de hesaplanır.

## D6. Kırpma stratejisi
- Yakın düzlem (`z >= 0`): her zaman gerçek Sutherland–Hodgman kırpması.
- x/y: ±8192 piksellik guard band. Yalnızca bandı aşan üçgenler kırpılır. 24.8'de 8192 px = 2^21; raster koordinat
  sınırı 2^22 px, kenar fonksiyonu i64.
- Uzak düzlem: tamamen dışarıdakiler outcode ile atılır, kısmi olanları derinlik testi keser.
- Kesişim noktası, kenarın iki ucu sözlüksel sıraya (x, y, z, w) konarak hesaplanır. Ortak kenarı ters yönde
  gezen iki üçgen bit-bit aynı noktayı üretir → çatlak yok (test: `clip_crack_free_shared_edge`).

## D8. Ertelenmiş çizim ve 64×64 karo bin'leme (2026-10-27)
Raster API'si `raster_begin(r, arena) → raster_triangle → raster_flush` oldu. `raster_triangle` üçgeni kurar
(`TriSetup`, raster durumunun kopyası dahil) ve karolara bin'ler. Flush karoları satır sırasıyla, karo içinde üçgenleri
gönderim sırasıyla işler; bu yüzden sonuç anlık çizimle birebir aynıdır (hash ile doğrulandı). Shader'ın uniform
işaretçileri flush'a kadar geçerli kalmalı. Raster arenasını yalnızca raster kullanır (pipeline'ın kare arenası ayrıdır,
çünkü pipeline kendi arenasını çizim başına mark/reset yapar). Arena dolarsa ara flush yapılır.
Gerekçe: karolar bağımsız → paralel raster (M6_2) için zemin; tam kaplanan karolarda kenar testi atlanır.
Tek thread'de bin'leme yükü skaler yolda %9–21 (ölçüm: tasks/M8_1_3d.md).

## D9. SIMD quad yolu bit-bit skalere eşit
`raster_tile_simd` (wasm SIMD) ve `raster_tile_scalar` aynı kayan nokta işlemlerini aynı sırayla yapar:
`c + a*fx + b*fy`, `1/w`, `düzlem * w`. wasm'da FMA olmadığı için sonuç bit-bit aynıdır. Kenar fonksiyonları i64
kaldı (i64x2): 24.8 ve guard band ile değerler i32'ye sığmıyor, hassasiyet düşürmek top-left beraberliklerini bozardı.
Skaler yol referans/kâhin olarak korunur; testler iki yolu bit-bit karşılaştırır.

## D10. Mip seviye tablosu uniform'larda, Texture ABI'si sabit (2026-10-27) — D23 ile değişti
`Texture` 32 bayt ve ofsetleri codegen sözleşmesi olduğu için seviye ofsetleri oraya eklenmedi. Bunun yerine
`TexLevel[TEX_MAX_LEVELS]` tablosu shader uniform bloğunda tutulur (`tex_level_table`). Doku değişince tablo yeniden
hesaplanmalıdır (`lit_uniforms_prepare`). Matoki codegen'i ileride aynı tabloyu uniform özelleştirmesiyle
sabit olarak gömebilir. Referans `ref_sample_level` döngülü hâliyle kâhin olarak kalır.

## D11. C shader'larının SIMD eşleri bit-bit aynı olmalı
SIMD shader, skaler eşinin işlemlerini aynı sırayla yapar. `m_maxf(a,b)` ≡ `f32x4.pmax(b,a)`,
`m_clampf` ≡ iki `bitselect`, `m_f2i_sat` ≡ `i32x4.trunc_sat_f32x4`. Böylece A/B ölçümü görüntüyü değiştirmez ve
framebuffer hash'leri regresyon testi olarak kullanılabilir. Matoki codegen'i de aynı ilkeye tabidir.

## D12. Matoki v0 mimarisi (2026-10-27)
- **Düz IR:** kontrol akışı lower'da kaldırılır (if → select, for → açma, çağrı → satır içi). Codegen ve
  ir_eval tek geçişli ve dallanmasız kalır; diferansiyel test kolaylaşır. Bedeli: iki dal da çalışır,
  büyük döngüler kod boyutunu büyütür (bütçe 4096 açılım).
- **Skalerleştirme:** vektörler bileşenlere bölünür; IR'da yalnızca F32/I32 (bool = tam maske).
- **Katlama + CSE kurulumda:** katlama `ir_eval_lane` ile yapılır → katlanmış ve katlanmamış program bit-bit
  aynıdır (test her iki yolu da JIT'ler). Ayrı bir optimizasyon geçişi yok; DCE codegen öncesi.
- **Codegen:** her canlı değer bir v128 yereli; V8 yazmaç ayırmayı yapar. Quad-sabit değerler skaler
  yerellerde (D26).
- **JIT:** motor `__indirect_function_table`'ı dışa aktarır (`--export-table --growable-table`); JS modülü
  `env.memory` ile örnekleyip `shade`i tabloya ekler, dönen indeks C'de `ShadeFn`. Modül imzası
  `(i32, i32, i32) -> ()` = `ShadeFn`, böylece `call_indirect` tip denetimi geçer.
- **Uniform yerleşimi:** bildirim sırası, 4 bayt hizalı ve boşluksuz. C struct'ları aynı sırayla yazılırsa
  aynı blok paylaşılır; motor JIT'ten sonra ofsetleri `offsetof` ile doğrular.
- **Sayı ayrıştırma:** f64 ara değerle (≤ 19 anlamlı basamak). Her durumda doğru yuvarlama garanti değil;
  JSON (Faz 3) için Eisel-Lemire gerekecek.

## D13. NaN belirsizliği ve select
wasm, aritmetikten çıkan NaN'ın işaret ve yük bitlerini belirlemez (V8 `f32x4.min`'den işaret biti 1 olan NaN
döndürdü). Matoki çıktısı paketlenmiş renk olduğundan (NaN → 0) bu görünmez. Diferansiyel test, bit düzeyinde
gözlem öncesinde NaN'ı kanonikleştirir. `IR_SELECT`'in F32 maskesi tam olmalıdır, aksi halde NaN bitleri sızar.

## D14. Fizik motoru C'de, Fortran yok (2026-10-27)
Fortran araç zincirleri (gfortran: wasm hedefi yok; Flang: wasm32 deneysel, çalışma kütüphanesi libc ister;
LFortran: dilin bir alt kümesi) projenin libc'siz / dış koda bağımlı olmayan kuralıyla çakışır. Fortran'ın
takma ad (aliasing) ve dizi avantajı, wasm SIMD ile elle yazılan C'de zaten mevcut. İki dilli bir motorda bit-bit
doğrulama (D9, D11) da sürdürülemez. Fizik `src/phys/` altında C11 ile yazılır. Hazır bir bilimsel Fortran kodu
gerekirse yalnızca çevrim dışı araç olarak, çıktısı veri olarak kullanılır.

## D15. Çözücü ön hesabı iki geçişli
Restitüsyon ve bias için yaklaşma hızı, hiçbir warm start itkisi uygulanmadan önce, tüm temaslar için ölçülür;
warm start ikinci geçiştedir. Tek geçişte önceki manifoldların itkileri sahte yaklaşma hızı üretir ve yığınlara
enerji pompalar (M9 notları, hata 1). Kutu-kutu SAT'ta referans yüz histerezisle seçilir
(`sep_b > 0,95·sep_a + 0,005`).

## D16. Varsayılan fizik çözücüsü: soft step (2026-10-27)
Box2D v3 tarzı alt adımlı soft step (4 alt adım, 30 Hz / ζ = 10 temas yumuşaklığı, relax geçişi), Faz 1'in
sequential impulse + Baumgarte çözücüsünün yerine varsayılan oldu. Ölçüm (M9 Faz 2 notları): eşit maliyette
31 katlı duvar ayakta kalıyor (SI'da çöküyor), 2000 kutuluk yığın oturuyor (KE 250 J → 0,63 J), girişim daha az.
SI karşılaştırma ve regresyon için korunuyor; testler iki çözücüyle koşuyor.
Uyku/adalar: birleşim-bul, küçük indeks kök (belirleyici), 0,5 s eşik süresi; uyuyan-uyuyan çiftler geniş fazda
atlanır. SIMD çözücü için kısıt grafiği renklendirmesi gerekiyor (skalerle bit-bit eşitlik ilkesi, D9/D11).

## D17. Şekil çarpışması "çekirdek + yarıçap", eklemler yumuşak (2026-10-27)
Her dışbükey şekil bir çekirdek (nokta / doğru parçası / çokyüzlü) ve yarıçaptan oluşur; GJK çekirdek
mesafesini, EPA yalnızca çekirdekler kesişince derinliği verir. Böylece küre / kapsül temasları çoğunlukla ucuz
GJK yolunda kalır. Çokyüzlü manifoldları referans / olay yüzü kırpmasıyla üretilir; yüz normali yalnızca o
eksendeki girişim ≤ EPA derinliği + 0,005 + %2 ise kabul edilir (yanlış yüz seçimini önler). Kutu-kutu SAT yolu
korunur (hız ve eski hash'ler). Hull eylemsizliği Faz 3b'de AABB kutusundandı; Faz 4'te gerçek tensör (D18).
Statik mesh: BVH (orta nokta, yaprak ≤ 4), üçgen başına ayrı manifold. Eklemler temaslardan önce çözülür;
soft step'te 60 Hz / ζ = 2 yumuşaklık (temaslardan sert, zincirde birkaç cm uzama kabul), SI'da Baumgarte.

## D18. CCD, mesh iç kenarları, asal eksenli eylemsizlik (2026-10-27)
- Eylemsizlik gövdede köşegen kalır, asal çerçevede: hull için Q = `hull->inertia_axes` (Jacobi), dünya tensörü
  (R·Q)·diag·(R·Q)ᵀ. Tam 3×3 yerel tensör saklamak yerine bu: çözücü ve kinetik enerji kodu aynı kalır.
- CCD Box2D v3 gibi "adım sonu geri çekme": hızlı gövde (yer değ. + dönüş·max_extent > 0,5·min_extent)
  statiklere karşı, `BODY_BULLET` dinamiklere de karşı süpürülür; TOI pozuna konur, hız korunur, adımın kalanı
  harcanır (alt adım yok — basit ve belirleyici). Başta değiyorsa merkez küresiyle yeniden; adım sonu sığsa
  geri çekilmez (eş düzlemli komşularda takılmayı önler). Hedef boşluk 0,25·PHYS_MARGIN → sonraki adımın
  spekülatif teması kesin bulunur.
- Mesh iç kenarı: önceden hesaplanan aktif kenar bayrakları (dihedral ~5° eşiği, içbükey pasif). Pasif
  kenarda / köşede biten sapmış normal yüz normaline çevrilir (Jolt "active edges" fikri). Komşuluk köşe
  indeksiyle: mesh'ler köşe paylaşarak verilmeli.

## D19. Yumuşak cisimler XPBD, rijit çözücüden ayrı (2026-10-27)
- XPBD "küçük adımlar" (10 alt adım × 1 tur): uyum fiziksel birimde (α = 1/k), sonuç alt adım sayısından büyük
  ölçüde bağımsız (statik denge e = m·g·α tam). Rijit çözücüye (soft step, impuls) katılmaz: `SoftWorld` rijit
  adımdan sonra ayrı adımlanır. Basit ve belirleyici; bedeli rijit tepkinin itkiyle bir adım gecikmeli olması.
- Rijit gövde pozu adım başı → sonu arası alt adımlara ara değerlenir (`Body.pos0/rot0`, CCD ile ortak).
- Çarpışma parçacık-yüzey (işaretli uzaklık); yüzey üçgeni-gövde ve öz-çarpışma yok. Jöle parçacık yarıçapı =
  yarım kafes adımı (karşı gövde parçacıkları aradan geçmesin).
- Kumaşta uzun menzilli bağ (tether) isteğe bağlı; rüzgâr yalnız normal yönlü sürükleme.

## D20. Kısıt grafiği renklendirmesi; SIMD temas çözücüsü renk sırasıyla bit-bit eşit (2026-10-28)
- D9/D11 ilkesi ("SIMD = skaler, bit-bit") Gauss-Seidel çözücüde ancak sıra sabitlenerek sağlanabilir:
  4 temasın paralel çözümü sıralı sırayı bozar. Çözüm: skaler referansın kendisi renk sırasına geçti. Aynı
  renkteki manifoldlar ortak dinamik gövde paylaşmadığından renk içi sıra sonucu değiştirmez; SIMD lane'leri
  bir rengin 4 manifoldunu aynı işlem sırasıyla çözer → skaler renk sırası ile bit-bit aynı.
- Varsayılan renk sırası + SIMD. Oluşturulma sırası (Faz 1–5) A/B için `graph_color = FALSE` ile duruyor;
  sonuçlar (hash) o yola göre değişti, kararlılık ölçümde iyileşti (Faz 6 raporu).
- Body'nin çözücü alanları sabit yerleşimli 128 baytlık blok (statik denetimlerle korunur). Statik gövdeler
  renk tüketmez ve SIMD'de geri yazılmaz. 16 renk; taşma rengi her iki yolda skaler ve en sonda.

## D21. Dünya BVH'si: iki dinamik AABB ağacı, kalıcı çift kümesi, kanonik çift sırası (2026-10-28)
- Statik ve dinamik gövdeler ayrı ağaçlarda (Box2D gibi); dinamikte şişman AABB (±0,1 m). Geniş faz kalıcı
  çift kümesiyle: yalnızca şişman kutusundan taşan vekiller sorgular → hareketsiz / uyuyan dünya ucuz.
- Çiftler her iki yolda (a < b) anahtarıyla sıralanır: sonuç (hash) geniş faz yönteminden bağımsız olsun ve
  SAP ile BVH bit-bit karşılaştırılabilsin. Bedeli: radix sıralama (2000 kutuda ~0,1 ms) ve Faz 6 hash'lerinin
  bir kez değişmesi.
- Raycast / CCD / yumuşak aday sorguları doğrusal taramayla aynı sonucu verecek kurallarla (eşitlikte büyük
  indeks; en küçük TOI; artan indeks sırası). SAP yolu `PHYS_BP_SAP` ile A/B ve regresyon için korunuyor.
- Statik gövdeler taşınmaz varsayılır; taşınacaksa dinamik (inv_mass > 0) ya da ileride kinematik ağaç.

## D22. Rijit ↔ kumaş: yavaşta dar faz, hızlıda iz silindiri + yakalama; rijit yolu adım içinde yavaşlar (2026-10-28)
- Yavaş gövde (≤ 5 m/s; alt adımda < 1 cm, kalınlıktan küçük): üçgen başına `phys_collide_cores` — sürekli test
  gerekmez, düzlem tabanlı kurallar (kutu gibi şekillerde yanlış) yerine motorun mevcut dar fazı.
- Hızlı gövde: parçacık-üçgen ayrık / sürekli testleri ince, gerili kumaşta güvenilir değil (çadır, delik).
  Gövdenin iz silindiri + yakalama (iki yönlü tutma) kumaşı ağ gibi davrandırır; momentum korunur.
- Rijit gövdenin adım içi yolu temas itkileriyle güncellenir ve adım sonunda konumu yazılır: tek yönlü
  rijit adımı + ayrı yumuşak adımı (D19) yapısını korurken aynı karede tepki verir. Temassız gövdede yol eski
  ara değerle birebir aynı (eski sonuçlar değişmez).
- Açık hava sürüklemesi alt adımda normal göreli hızı tersine çeviremez (kararlılık sınırı).

## D23. Texture ABI 96 bayt: seviye ofset tablosu dokunun içinde (2026-10-28) — D26 ile 352 bayt
D10'un yerine geçer. `Texture` sonuna `u32 lv_ofs[16]` eklendi (ofset 32, eski alanların ofsetleri aynı).
`tex_create` doldurur; elle kurulan dokular `tex_fill_level_offsets` çağırır. Böylece Matoki, uniform bloğuna
tablo koymadan herhangi bir dokuda seviye başlangıcını tek okumayla bulur (`IR_TEX_WORD`). C tarafındaki
`TexLevel` tablosu (`LitUniforms.lv`) C shader'larının hızlı yolu olarak kaldı.

## D24. discard: ShadeQuad.kill, raster maskeyi shader'dan sonra uygular (2026-10-28)
ShadeFn imzası değişmedi; `ShadeQuad`'ın boş `_pad` alanı `kill` oldu. Raster çağrıdan önce 0 yazar, sonra
`pass &= ~kill`. Derinlik zaten shader'dan sonra yazıldığı için ayrı bir "geç z" yolu gerekmedi. Matoki'de
discard SPMD kalır: kod akmaya devam eder, yalnızca etkin yol maskesi (iç içe if'lerin VE'si) kill'e eklenir.
Ölçüm: discard'sız sahnelerde raster maliyeti ±%2 içinde (gürültü), hash'ler aynı.

## D25. Matoki matematik genişlemesi: bit-bit C eşi olan polinomlar, matris yalnızca uniform (2026-10-28)
- `exp2/log2/exp/log/pow`: `m_exp2f`/`m_log2f` (core/math.c) ile aynı işlem sırası → bit-bit aynı; göreli hata
  ~1e-6. exp2 [-150, 128]'e kenetlenir, 2^n iki normal çarpanla uygulanır (denormal/taşma doğru yuvarlanır).
  `pow(0, 0)` = NaN (GLSL'de tanımsız).
- `mat3/mat4` yalnızca uniform ve yalnızca `M * v`: lower'ın değer gösterimi bileşen başına ≤ 4 IR değeri; tam
  matris değerleri (yerel, parametre, `M * M`) bu sınırı kaldırmayı gerektirir. Sıra `mat4_mul_v4` ile aynı.
- `IR_DERIV_*` sonucu artık "quad-sabit" (uniform bayrağı 1): her lane'e aynı farkı yayar. mip seviyesi bu
  sayede tek lane'den kaydırma / tek okuma ile hesaplanır. Yan etki: türevin türevi her zaman 0 (önceden inf/NaN
  girdide NaN olabilirdi); ir_eval ve codegen aynı kuralı izler.
- `@bilinear` seviye içinde (GL_LINEAR_MIPMAP_NEAREST); seviyeler arası (trilinear) yok.

## D26. Matoki varsayılan shader: önsöz (preshader), skaler quad-sabitler, seviye tabloları (2026-10-28)
- **Önsöz:** codegen üç fonksiyon üretir: `shade` (tek başına doğru), `prelude(uniforms)` ve `shade_fast`.
  Yalnızca uniform/doku/sabitten türeyen ve quad'a bağlı bir kullanıcısı olan değerler önsözde hesaplanıp
  modülün önbelleğine (codegen arenası) yazılır. Raster, `TriSetup.prelude` varsa (önsöz, uniforms) çifti
  değiştikçe çağırır (`raster_set_prelude`, `Material.prelude`). Önbellek anahtarı içerik değil işaretçi:
  uniform içeriği flush boyunca sabit olmalı (zaten sözleşmeydi). Doğrudan çağıran (test, araç) önce
  prelude'u çağırır ya da `shade`'i kullanır. İleride worker'lar: önbellek thread başına olmalı.
- **Skaler quad-sabitler:** `uniform` bayrağı taşıyan değerler (türevler, mip seviyesi, ondan türeyen her şey)
  wasm skaler yerellerde hesaplanır, v128'e yalnızca vektör kullanıcısı varsa bir kez yayılır. Türev = iki
  lane çıkar + skaler fark. Karşılaştırma koşullu skaler select → tek wasm `select`. Ölçüm (yakalanmış gerçek
  quad'lar, aynı ikili, anahtar `codegen_set_scalar`): v128'e göre dört görünümde %6–10 hızlı.
- **Seviye tabloları:** `Texture` 352 bayt: `lv_w/lv_h/lv_fw/lv_fh[16]` (D23'teki `lv_ofs`'a ek). Seviyeye
  bağlı boyutlar "kaydır → max → dönüştür" seri zinciri yerine paralel tek okumalar (C'deki `TexLevel` gibi).
  Ölçüm: 2. görünümde shader oranı 1.24 → 1.10.
- **Sonuç:** lit.mtk varsayılan (C ile bit-bit, hash'ler aynı). Kare ölçümünde ortalama %1–4 hızlı; mip'in
  hep 0 olduğu yakın görünümde %2–6 yavaş — C'nin dallı mip seçimi orada erken çıkıyor, Matoki dalsız.
  C shader'ları kâhin ve A/B için kalır.

## D27. Paralel raster: paylaşılan bellek, aynı modülün worker örnekleri, karo başına iş (2026-10-28)
- **Bellek:** motor `--import-memory --shared-memory --max-memory=1GiB` ile bağlanır; JS paylaşılan
  `WebAssembly.Memory` oluşturur (web/mt.mjs). Sayfa crossOriginIsolated olmalı (COOP/COEP; `build.py serve`).
  Worker'lar aynı derlenmiş modülü bu bellekle örnekler; `__wasm_init_memory` veriyi bir kez kurar,
  her örnek kendi yığınını (`jobs_stack_top`, `__stack_pointer`) kullanır.
- **İş sistemi (core/jobs):** tek bilet sözcüğü (sıra << 16 | indeks), CAS ile indeks alma → geç uyanan worker
  sonraki işten indeks tüketemez. Worker'lar `memory.atomic.wait32`'de uyur; ana thread (tarayıcıda wait
  yasak) kendisi de çalışır, sonra biten sayısını bekler. Worker yoksa aynı döngü sırayla çalışır.
- **Raster:** flush, boş olmayan karoları paylaştırır; bir karonun üçgenleri gönderim sırasıyla tek thread'de
  → çıktı thread sayısından bağımsız (test: `jobs_raster_parallel_bit_exact`, bench hash'leri). İstatistik ve
  "son önsöz" thread başına; `TriSetup.drawn` atomik değişim.
- **JIT:** Matoki modülleri yalnızca ana thread'de derlenir; `jit_load` baytları kayıt defterine kopyalar,
  worker'lar iş almadan önce eksik modülleri aynı tablo indekslerine kurar. Modül `env.cache` global'ini içe
  aktarır: her örnek kendi önsöz önbellek dilimini (`cache + tid·stride`) kullanır (D26'daki tek önbellek yarışırdı).
- **Ölçüm (i7-12650H, 16 thread):** node 1280×720 kare 2.6–6.5× hızlı; tarayıcı 640×360 motor süresi 2.0–5.8×.
  Kalan seri kısım: geometri aşaması ve fizik (sıradaki iş).

## D28. Görüntü kalitesi: anizotropik süzme Matoki'de, half-Lambert² + geçirgenlik, MSAA 4x (2026-10-29)
- **lit_hq.mtk (varsayılan):** derleyiciye ek yok, dilin kendisiyle: büyük eksen boyunca 4 çift doğrusal
  örnek, mip seviyesi küçük eksenden (ayak izi ≥ büyük/4). lit.mtk C kâhiniyle bit-bit kalır (testler);
  lit_hq'nun doğruluğu derleyicinin genel farkı (ir_eval = JIT) ile güvencede, C eşi yok.
- **Diffuse:** `mix(max(n·l,0), (n·l/2+1/2)², wrap)` — wrap 1 kumaşta (kıvrım kontrastı korunur, gölge yüzü
  kara değil), arkadan ışık `translucency·max(-n·l,0)`. Önce düz sargı (n·l+w)/(1+w) denendi: kıvrımları
  sildi (görsel karşılaştırma), terk edildi. `LitUniforms.wrap/translucency` tint'ten sonra (lit.mtk okumaz).
- **MSAA 4x:** D3D döndürülmüş ızgara; kapsama ve derinlik örnek başına, shader piksel merkezinde bir kez,
  `fb_resolve` kanal ortalaması. Kenar fonksiyonu örnekte tam sayı ile kesin (sx = -dy·256). Sınır kutusu
  ve karo testleri örnek alanına genişletilir. SIMD yolu: piksel merkezi + ofset uç değerleriyle tam içeride /
  tam dışarıda sınıflaması, yalnız kenar pikselleri örnek örnek; 4 örnek bitişik → derinlik/yazma tek v128.
  Skaler yol kâhin (test: msaa_simd_matches_scalar). Temizleme ve çözümleme iş sistemiyle paralel.
- **Maliyet (16 thread, 640×360):** HQ + MSAA kare 3.6–5.3 ms (önceki varsayılan 0.8–2.7 ms); 720p 11–23 ms.

## D29. Matoki vertex aşaması ve paralel geometri (2026-10-29)
- **ABI:** `VertexFn(VertexIn*, uniforms, VertexOut*)`, bir çağrı 4 köşe (lane = köşe), wasm imzası ShadeFn ile
  aynı → JIT, önsöz (D26) ve thread önbelleği (D27) değişmeden. Köşe girdileri standart yuvalarda (ATTR3D_*),
  uniform bloğu `VertUniforms` (çizim başına boru hattı doldurur: model, view_proj, normal_mat, time, params[8]).
- **Dil:** `attribute`, `fn vert() -> vec4`; vert'te varying'ler 0 ile başlayan yereller (if'te select ile
  birleşir). IR: `IR_ATTR` kaynağı, `out` + `xout[]` ek çıktılar (ofsetli v128 saklama); ir_eval genel girdi/çıktı.
  Aşama denetimi lower'da (yalnız girişten erişilen kod): türev, discard, otomatik mip vert'te hata.
- **C kâhini:** `vert_std_c` eski sabit kodlu dönüşüm; `std_vert.mtk` onunla bit-bit → Matoki varsayılan oldu
  ve bütün kare hash'leri değişmedi. Tek thread'de Matoki (SIMD, 4 köşe) C'den hızlı: 1. görünüm 3.22 → 1.93 ms.
- **Ertelenmiş, paralel geometri:** `pipe_draw` yalnız eler ve kaydeder; `pipe_end` partiler hâlinde (arena
  bütçesi) köşe işleri (128 köşe) ve üçgen parçaları (256 üçgen) iş sistemine dağıtır. Raster'da karo kutuları
  parça başına (`raster_chunks_alloc`, `raster_triangle_to`); flush karo içinde parçaları sırayla gezer →
  gönderim sırası ve çıktı thread sayısından bağımsız (test + hash). Eski `raster_triangle` "seri parça" ile aynen.
- **Çekişme:** ilk sürüm her ayırmada ortak arenaya atomik CAS yapıyordu: 16 thread'de kurulum 6.2 → 12.6 ms
  (yavaşladı). İş başına 16 KB dilim (tek atomik) + bekleyen sayısı iş sonunda: 5.8 → 0.86 ms (6.7×).
- **Ölçüm (16 thread, HQ + MSAA, 640×360):** geometri 1. görünüm 4.10 → 0.84 ms, 3. görünüm 3.75 → 0.76 ms;
  göl (33 bin üçgen, wave_vert) 1.08 ms.

## D30. Uzak görüntü: sRGB-doğru mip, 8 örnekli trilinear anizotropi, üstel sis, Matoki gökyüzü (2026-10-29)
Teşhis (büyütülmüş kesitler, görünüm 4): uzak zemin kirli gri (sRGB'de ortalama: siyah/beyaz → 128), ayrıntılı
bölge ile düz bölge arasında testere dişli sınır (seviye atlaması, 4 örnek), sisin başlamadığı mesafede ufukta
gri duvar, tek renk gök.
- **Mip:** renk kanalları sRGB → doğrusal (16 bit tablo) ortalanır → sRGB (12 bit tablo); alfa doğrusal.
  Siyah/beyaz → 188, düz renk gidiş-dönüşte birebir (test). D7'deki "lineer uzayda" notu artık doğru.
- **lit_hq.mtk:** büyük eksen boyunca 8 örnek (oran ≤ 8), her örnek iki komşu seviyede, kesirli seviyeyle karışım.
  Denendi: 4 örnek trilinear (uzak kareler erken düzleşir) ve 8 örnek en yakın seviye (seviye sınırında sert çizgi);
  8 trilinear seçildi. Sis 1 - 2^(-4x²) (yumuşak başlar, ufukta tam).
- **sky.mtk:** kameraya ortalanmış küre (0.85·zfar), ufuk = sis rengi → zemin göğe dikişsiz; güneş diski + hale
  (üsler kareyle, pow yok). `Y` ile kapanır. Sonlu zeminlerin (80 m) sisi kenarda tamamlanacak şekilde kısaltıldı.
- **Maliyet (node, 16 thread, HQ + MSAA, önceki commit ↔ bu, dönüşümlü):** kare 1.4–2.5× (ör. görünüm 1
  2.60 → 5.47 ms); gök kubbesi bunun 0.4–1.0 ms'si. Ucuz yol: `L` ile Matoki (mip, tek örnek) shader'ı.

## D31. Küreler için hıza bağlı spekülatif temas + çarpma anı hızı (2026-10-29)
Sorun: üst üste bırakılan toplar (sahne 8) kesin olay çözümünden %6–40 sapıyordu. İki neden: (1) sabit 2 cm'lik
spekülatif pay, 10 m/s yaklaşan iki dinamik topu yakalamıyordu (CCD yalnızca statiklere bakar) → toplar 13 cm iç içe
geçip ayrılırken restitüsyon yanlış hızla uygulanıyordu; (2) restitüsyon hedefi adım başındaki hızdı, zemine düşen
top g·dt kadar yavaş sayılıyordu (her sıçramada ~%6 yükseklik kaybı).
- **Pay:** küre-küre ve küre-düzlem çiftlerinde `PHYS_MARGIN + yaklaşma hızı·dt` (`spec_margin`, `phys_collide_margin`);
  uyanık kürelerin AABB'si adım içi yol kadar uzatılır (geniş faz). Diğer şekiller değişmedi.
- **Çarpma hızı:** boşluklu temasta `rel_vel = −√(v² + 2·a·s)` (a = göreli dış ivme, s = boşluk).
- **Sınır:** adım başına tek restitüsyon geçişi → bir adımda birden çok çarpışma (hafif top ağır topla zemin arasında
  sıkışınca) çözülemez. 240 Hz'te kesin çözümle %1 içinde, 60 Hz'te ağır-üstte durumunda %15 sapma
  (tests/test_phys_drop.c). Sahne 8 bu yüzden kare başına 4 adım (240 Hz) koşar.

## D32. Gezegen çekimi g(h) = GM / (R + h)² (2026-10-29)
`PhysWorld.planet_gm/planet_radius` > 0 iken her gövdeye `phys_gravity_at` (f64; Dünya'da birkaç metrenin etkisi
~1e-6, f32'de birkaç ulp). Yön −y, h = merkez y'si (düz zemin y = 0). Varsayılan kapalı (sabit `gravity`).
Doğrulama (tests/test_phys_fall.c, 240 Hz × 4 alt adım): sabit g'de t = √(2d/g), v = √(2gd) %0,1 içinde;
R = 50 m'lik gezegende d = 50 m düşüş v = 22,142 (kesin √(2GM(1/r − 1/(r+d))) = 22,147; sabit g formülü 31,32 →
motor gerçekten g(h) kullanıyor), süre radyal Kepler formülüyle %0,01. Enerji: yarı-örtük Euler'de kapalı form
E_n − E_0 = −½·g·h·|v_n| (göreli h·v/2d), ölçülen sapma bu sınırla örtüşür (d = 2 m: 0,001633 ↔ 0,001631);
birinci mertebe, birikmez, h → 0'da sıfır. Sahne 8 Dünya değerleriyle, `P` ile R = 20 m'lik gezegen.

## D33. İstatistiksel okyanus: karmaşık genlik + Phillips spektrumu + FFT (2026-10-29)
Tessendorf 2001: h̃0(k) = (ξr + iξi)·√(P(k)/2) (Gauss olasılıklı karmaşık genlik), h̃(k,t) = h̃0·e^{−iωt} +
conj(h̃0(−k))·e^{iωt}, ω = √(g|k|). Sanal üstel faz dalgayı k yönünde yürütür; gerçek cos(kx)·cos(ωt) iki zıt
yönlü dalganın toplamı olurdu. Rüzgâra karşı (k̂·ŵ < 0) genlikler `reverse` ile çarpılır; sahnede 0 → ters dalga yok.
Testte rüzgâra karşı enerji tam 0; zaman kaydırmalı çapraz ilinti asimetrisi 0,74 (reverse = 1'de 0,01: duran dalga).
- 64² ızgara, 40 m yama (göl mesh'i ile aynı → kenarlar periyodik). İki gerçek alan bir karmaşık FFT'de (Hermitsel):
  (h + i·Sx), (Dx + i·Dz), Sz → 3 ters 2B FFT; Nyquist satırı sıfır (orada eğim Hermitsel olamaz).
- Genlikler Parseval ile hedef rms'ye ölçeklenir (test: 0,2000). Faz f64'te 2π'ye indirgenir.
- Alanlar CPU'da, köşeler her kare mesh'e yazılır (Matoki köşe aşamasında doku okuma yok); `O` ile Gerstner
  (wave_vert.mtk) geri gelir. Bench görünümü 6'nın hash'i bu yüzden değişti. Okyanus maliyeti ayrıca ölçülmedi.
## D34. Değen top yığınları için Hertz temaslı DEM (2026-10-29)
Sorun: D31'deki katı cisim çözücüsü topları 0,3 m arayla düşürüyordu; sıralı iki cisim darbesi modelinde eşit kütleler
hızı birebir devreder → her top kabaca kendi yüksekliğine döner ("hepsi aynı yükseklikte zıplıyor"). Gerçek yığında
toplar birbirine değer ve çarpışma esnek temaslarla AYNI ANDA yayılır; bunu darbe (impuls) modeli veremez.
- **phys/dem.c:** küre + zemin, f64. F = k·δ^{3/2} + γ·√(m*·k)·δ^{1/4}·δ̇ (Hertz + Tsuji sönümü), k = (4/3)·E*·√R*,
  E* = E/(2(1−ν²)). Tsuji'de e hızdan bağımsız (boyutsuz analiz) → γ(e) tek boyutsuz çarpışmada bisection ile.
  Hız Verlet, alt adım = en kısa Hertz temas süresi / 40. Çekim sabit g ya da g(h) (D32).
- **Doğrulama (tests/test_dem.c):** tek top sekme/düşme 0,808–0,809 (hedef e² = 0,81, 0,5–4 m'de aynı); e = 1'de
  toplam enerji (kinetik + çekim + esneklik) sapması 1,5e-4; gezegen düşüşü kesin formülle 5 hane.
- **Kesin çözümle doğrulama:** toplar 0,3 m aralıklıyken (sıralı çarpışmalar) DEM, test_phys_drop.c'nin olay tabanlı
  kesin çözümüyle örtüşür: üst toplar 2,440 ↔ 2,439, 2,171 ↔ 2,205, 7,279 ↔ 7,283 m. Yakınsama: alt adım t_c/40 →
  t_c/80'de sonuç %0,04 değişir.
- **Kurulum:** toplar tam değerken çarpışmalar üst üste biner; ağır altta (4,2,1) orta top enerjisini üste aktaramaz
  (0,90 geri sıçrar, üstteki 2,41). 2 mm aralıkla (yerleştirilmiş, yapışık değil) çarpışmalar ayrışır: orta 0,63,
  üstteki 2,76; eşit kütlede üstteki yine en çok (0,30 / 0,90 / 1,09); ağır üstte 0,16 / 0,38 / 1,15. Sahne 8 2 mm.

## D35. Çözünürlük: sabit 640×360 → ekranın gerçek piksel boyutu (2026-10-29)
Sayfa 640×360 kareyi ~1000 px'e `image-rendering: pixelated` ile büyütüyordu (kaba bloklar). Artık canvas boyutu
CSS genişliği × devicePixelRatio (en çok 1920×1080), `engine_resize` framebuffer'ı ayrı `fbmem` arenasında yeniden
ayırır (40 bayt/piksel, MSAA dahil); `H` ölçeği %100/75/50 gezer, `?res=WxH` sabitler. Maliyet piksel sayısıyla
doğrusal: 16 thread, HQ + MSAA, görünümler arası 640×360 3,8–7,2 ms, 1280×720 13,5–23,3 ms,
1920×1080 29,6–52,3 ms (tools/bench_res.mjs). Sayfada FPS 1 s boyunca 25'in altındaysa ölçek kendiliğinden bir kademe iner
(H'ye basılınca kapanır). İnce statik kutular (direk, cetvel) düz gri malzemeyle (doku uzaması).
## D36. Temel sağlamlık: bellek, rasterizer fuzz, fizik koruması, uyku, derleyici derinliği (2026-10-29)
- **Bellek:** `arena_try` / `ARENA_TRY_ARRAY` dolunca NULL + `fails` sayacı; dizi boyutu taşması yakalanır. Kalıcı
  (başlangıç) bellek panik olarak kaldı. Kare başına: raster parça tablosu / üçgen / kutu sığmazsa üçgen atlanır
  (`tris_dropped`, `overflows`), boru hattında çizim sınırı ya da kare belleği dolarsa çizim atlanır (`draws_dropped`).
  Sayfada doluluk satırı. Bulunan hata: `raster_begin` aynı arenayla tekrar çağrılınca parça tablosunu sızdırıyordu.
- **Rasterizer fuzz:** 6000 rastgele üçgen (dev, dejenere, NaN, sonsuz, piksel merkezine tam denk) i64 kesin referansla
  (24.8 sabit nokta + top-left) piksel piksel: 2,66 M piksel, 0 fark; SIMD = skaler (MSAA açık/kapalı); çöp mesh'ler
  (NaN, w ≈ 0, aralık dışı indeks) 400 bin üçgende tampon dışına yazmaz (nöbetçi değerler).
- **Fizik:** adım başı/sonu `guard_bodies` (NaN/sonsuz → son geçerli poz, hız sınırı 1000 m/s, 1000 rad/s);
  bozuk `BodyDesc` temizlenir. NaN'lı gövdeden ayrı adadaki yığın temiz koşuyla bit-bit aynı.
- **Uyku (üç hata):** (1) uyanan ada o adımda zeminsiz kalıp g·dt düşüyordu — uyandırma sonrası çiftler yeniden
  toplanır; (2) değmeyen komşu adalar AABB örtüşmesiyle birbirini sonsuz döngüde uyandırıyordu — uyandırma yalnızca
  dar fazda gerçek temasla; (3) küreler sonsuza dek yuvarlanıyor / yerinde dönüyordu — yuvarlanma + burulma direnci
  (tork ≤ μ_r·r·J_n, küre/kapsül μ_r = 0,02; J_n soft'ta alt adım itkilerinin toplamı `jn_sum`, SIMD'de aynı sıra).
  Kesin formül a = μ_r·g/1,4: soft 2,6562 / SI 2,6555 ↔ 2,6496 m/s. 300 ve 50 gövdelik yığın varsayılan 4 alt
  adımda tamamen uyur (önce hiç uyumuyordu). bench_phys, uyku açık: 100 gövde oturmuş 0,158 → 0,007 ms/adım,
  500: 0,924 → 0,533; uyku kapalı durum hash'leri değişmedi.
- **Matoki:** iç içe ifade/blok derinliği 200 ile sınırlı (20 000 derinlik yığını taşırıp motoru düşürüyordu);
  4800 mutasyon + 3000 jeton çorbası: çökme yok, her hata konumlu. Canlı yeniden derleme başarısızlıkta kalıcı
  arenayı geri sarar.
## D37. İki deponun birleşmesi: MatokiEngine sistemleri bu depoya, seçerek (2026-10-29)
Xassemblianist/MatokiEngine (WebGPU, ses, arayüz, font, glTF, animasyon, ECS, rollback) ile bu depo (fizik, DEM,
okyanus, belirlenimci iş sistemi, sağlam Matoki derleyicisi) tek motorda birleşir. İki depo da aynı ekibin yönetiminde.
- **Ana depo bu depo** (SamurayBalgami/Matoki). Ortak git geçmişi yok, aynı yoldaki 55 dosyanın hiçbiri aynı değil →
  `git merge` değil, alt sistem alt sistem taşıma (tasks/M13_birlesme_plani.md). MatokiEngine taşıma bitince arşivlenir.
- **Adlar bizim kalır.** `tools/port_names.py` eşleme tablosuyla çevirir (`vec3_add` → `v3_add`, `m_sin` → `m_sinf`,
  `arena_push` → `arena_alloc`, `mat4_mul_vec4(M, v)` → `mat4_mul_v4(&M, v)`, `rng_seed` → `rng_make`); dize sabitlerine
  dokunmaz; bizim çekirdekte olmayan ad / bulunmayan include kalırsa çıkış 1 — iki ad kümesi depoda kalıcı olmaz.
  Doğrulanmış anlam eşitlikleri: arena (sıfırlanmış bellek, dolunca panik), Vec/Mat düzeni (sütun öncelikli),
  `mat4_perspective` (0..1 derinlik, aynı formül), `mat4_look_at`. Farklı: RNG (onlar PCG32, biz xorshift64*) →
  taşınan kodun rastgele dizileri değişir, belirlenimcilik korunur; onların `rng_range` tamsayı → `rng_range_i32`;
  `b32` onlarda u32, bizde i32.
- **Tek iş sistemi** `core/jobs` (belirlenimci, 1T ↔ 16T bit-bit testli); onların `thread/` kullanımları buna çevrilir.
- **Rol bölümü: işlemci simülasyon, ekran kartı çizim.** Fizik / DEM / rollback / animasyon / ECS / eleme CPU'da ve
  bit-bit belirlenimci kalır (GPU'lar arası float farkı geri sarma ve ağ eşitlemesini bozar). GPU'ya yalnızca görsel
  iş: üçgen çizimi, shader, okyanus FFT görüntüsü, parçacık, gölge, MSAA, post-process. Boru hattı: CPU N+1. karenin
  simülasyonunu yaparken GPU N. kareyi çizer; kare süresi ≈ max(CPU, GPU). Sayfada CPU ms / GPU ms (zaman sorgusu) /
  kare ms ayrı gösterilir.
- **WebGPU ikinci arka uç, en son faz.** Ortak giriş aynı çizim listesi (`DrawRec`); Matoki'ye WGSL hedefi eklenir
  (shader bir kez yazılır). Yazılım render referans kalır: her görünüm iki yolda çizilip piksel farkı eşikle test
  edilir. WebGPU yoksa / açılamazsa motor sessizce yazılım render'a döner ("GPU kullanmadan çalışır" garantisi).
  JS tarafı yalnız komut akışını WebGPU çağrısına çevirir; çizim kararları C'de.
- **Çekirdeğe eklenenler (faz 0):** `isize`, `KB/MB/ALIGN_UP/IS_POW2/I32_MAX/I32_MIN/U32_MAX`, `IVec2`/`iv2`,
  `v2_*`, `v4_add/sub/scale`, `m_ceilf/m_truncf`, `M_HALF_PI_F/M_DEG2RAD_F/M_RAD2DEG_F`, `arena_sub`,
  `ARENA_ARRAY_NOZERO`, `rng_range_i32`. İlk sınama: synth, json, base64, noise, ttf, gfx2d/anim çevrilip
  `-Werror` ile hatasız derlendi. Onların 148 taşınabilir dosyasından 112'si bugün eksiksiz çevriliyor; kalan 36'nın
  ihtiyacı: printf tarzı `fmt_format` + `LOG/PANIC/ASSERT`, profiler, sabit nokta (`fixed.h`), SIMD yardımcıları,
  `mat3_2d` / `mat3_mul_point`, `mat4_inverse` → faz 1.
- **Faz 1 (aynı gün):** eksikler eklendi → 148/148. printf alt kümesi ve `LOG/PANIC/ASSERT` `core/fmt.h`'de;
  profiler yalnız ana thread (worker'lar ayrı modül örneği, bölge `jobs_parallel_for`'u sarar). `mat3_mul` işaretçi
  alır (Mat3 sözleşmemiz), fizikteki kopya kaldırıldı, işlem sırası aynı → hash değişmedi. 2D sprite yığınlayıcı
  bizim ertelenmiş raster'a yeniden yazıldı; raster'da alfa karışımı olmadığı için şimdilik opak + kesik (faz 2).
  Taşınan kodda bulunan hata (tilemap kenar toleransı) düzeltildi; JSON sayıları ≤ 3 ULP (doğru yuvarlama değil).
- **Faz 2:** raster'a `BlendMode` (NONE / ALPHA düz / PREMUL / ADD). Tamsayı formül (`div255` = round(x/255),
  [0, 65535]'te kesin) → skaler, SIMD, MSAA skaler, MSAA SIMD bit-bit aynı; karışımlı üçgende SIMD tek örnekli yol
  4 pikselli vektör yazma yerine şerit şerit yazar (karışımsız yol değişmedi). Karo içi sıra gönderim sırası olduğu
  için yarı saydam üst üste çizim 1T ↔ çok thread aynı (test). Durum `RasterState.blend` / `TriSetup.blend` ile
  üçgen başına; boru hattı raster'ın o anki ayarını alır. Font (TrueType, işaretli alan AA, raf atlas, metin
  düzeni) MatokiEngine'den; motor içi panel (`gfx3d/hud`) çözümlenmiş renge CPU'da yazar, raster'dan geçmez.
  Panel varsayılan kapalı (ölçüm araçları, kare hash'leri değişmez), sayfa açar (U, `?nohud`).
- **Faz 3:** tek `Quat` tipi `core/quat.h`'de (fizikteki satır içi işlemler birebir taşındı → fizik ve kare hash'leri
  aynı; onların `scene/transform` kopyası kaldırıldı, `quat_norm` → `quat_normalize`, `quat_rotate_vec3` →
  `quat_rotate`). ECS (seyrek küme, nesilli tutamak), dönüşüm hiyerarşisi, sahne JSON kaydı, iskelet + klip (STEP /
  LINEAR / CUBICSPLINE) + LBS kaplama (skaler + wasm SIMD), glTF/GLB yükleyici ve PNG çözücü taşındı; glTF dokuları
  bizim `tex_create` (sRGB-doğru mip) ile kurulur. Motor varlıkları `assets/` altında gömülür (`build.py`; testte
  `tests/assets` ile aynı ad bir kez). Sahne 9 ilk girişte ayrı arenada kurulur → `g.perm` yerleşimi ve diğer
  sahneler değişmez.
- **Faz 4 (kaynak 2ac467a'ya güncellendi):** ses = onların synth + halka tampon + AudioWorklet'i. Sesi ana thread
  üretir (karede `audio_pump`, hedef doluluk 60 ms); worklet paylaşılan bellekten kopyalar. Halka ses başlarken ön
  doldurulur (açılış kesintisi yok). Çarpışma sesi `audio/impact` köprüsü yalnız okur: soft çözücünün adım başı
  yaklaşma hızı (rijit) ve çekim dışı hız sıçraması (DEM), cisim başına 0,12 s bekleme, adımda en güçlü 4 aday,
  eşitlikte indeks sırası → simülasyon ve fizik hash'leri ses açık/kapalı aynı (test). Onların testsiz deniz/hava
  efektleri uç girdi testiyle alındı. Sınır: sayfa gizliyken / ana thread durunca ses kesilir (üretici ana thread).
- **Faz 5:** sim çekirdeği (anlık görüntü halkası, kare numaralı girdi, tahmin + geri sarma, kayıt / ikili dosya,
  desync özeti) onlardan; fizik özeti ve belirlenimcilik sahnesi bizim fiziğe yeniden yazıldı (`sim/sim_det.c`).
  Bizim `PhysWorld` işaretçi içerir ama tüm dizileri durum arenasından ayrılır ve anlık görüntü aynı adrese geri
  yüklenir → düz kopya yeter; özet işaretçi baytlarını değil değerleri (gövde durumu + warm start önbelleği) karıştırır.
  Anlık görüntü yalnız arenanın kullanılan kısmını kopyalar (arena hiç kullanılmadıysa tüm bölge): durum dışı
  yazılan alan geri sarmada korunmaz — durum daima `sim_state_alloc` / durum arenasıyla kurulur.
- **Faz 6:** anlık mod arayüz (`ui/ui`), 8×8 bitmap font (`ui/bitfont`; src/font TrueType'tan ayrı ad) ve canlı
  shader editörü (`ui/editor`) onlardan. Editör bizim `MatokiError` ve toplu `lex_source` ile çalışır. F2 açar;
  açıkken girdi oyuna değil editöre gider (basılı tuş durumu kare içinde saklanıp geri yüklenir), JS fareyi
  kilitlemez, imleç konumu + karakter iletir (AltGr karakterleri dahil). Canlı derleme (Ctrl+Enter) iki dönüşümlü
  8 MB arenaya: yürürlükteki program bir arenada kalır, yeni deneme diğerine; başarısızlık / sık düzenleme ana
  arenayı tüketip motoru düşüremez. Yeniden derleme açılıştaki uniform yerleşimi denetimini de yapar (önceden
  `engine_matoki_recompile` yapmıyordu: değiştirilmiş uniform sırası yanlış ofsetten okurdu).
- **Faz 7 (ölçümle seçerek):** yalnız gölge haritası alındı. Raster'da `shade == NULL` = yalnız derinlik (varying ve renk
  yok; MSAA'da çizilmez; önceden bu üçgenler sayaçsız atılıyordu). `gfx3d/shadow`: ışık uzayında ortografik, texel
  ızgarasına hizalı, ön yüz ayıklamalı derinlik geçişi; doku derinlik tamponunu gösterir, `lit_hq.mtk` mevcut
  `texel_fetch` + `bitcast_f32` ile 2×2 süzmeli karşılaştırma yapar (derleyici değişmedi). `lit_hq.mtk` iki kez
  derlenir: gölgesiz türevde `shadow_lit(wpos)` → `1.0`, gölge kodu atılır → kapalıyken maliyet yok, çıktı bit-bit
  aynı. Motor varsayılanı kapalı (ölçüm araçları / hash'ler değişmez), sayfa açar (B, `?noshadow`). Onların
  "ertelenmiş çizim"i bizde zaten var (`pipe_draw` → `pipe_end`); WAT çekirdekleri motor kodu değil, Matoki ↔ elle
  WAT ölçüm deneyi — oradaki 5 derleyici önerisi bizim derleyicide ayrıca ölçülecek.
## D7. Mip zinciri ve seviye seçimi
M8 §2.1 / §2.3'e birebir uyulur. Kutu filtresi lineer uzayda (sRGB-doğru indirgeme sonraki adım). Seviye seçimi
tamsayı: `e = exponent(rho2) - 127`, `level = e >> 1`, `rho2 < 1` / NaN / denormal → 0. `lod_bias = 0`.

## D38. GPU arka ucu: C'de komut akışı, JS'te yalnız çeviri (M13 faz 8a)
Yazılım render ana yol kalır; WebGPU isteğe bağlı ikinci arka uç (`3d.html?gpu`, kurulamazsa yazılım). Boru hattı GPU
kipinde (`pipe_set_gpu`) elemeden sonra DrawRec yerine `gpu/gcmd` akışına yazar: u32 sözcükler (FRAME, MESH, DRAW,
END), işaretçiler wasm bellek ofseti, çizim başına 256 baytlık uniform yuvası tek blokta. Mesh kimliği adres, sürümü
`Mesh.gen` — tüm mesh'ler arası tekil sayaç (`mesh_touch`; `mesh_alloc` / `mesh_compute_bounds` / `_normals` çağırır):
arena geri sarılıp aynı adrese yeni mesh gelse de GPU eski kopyayı kullanamaz. `web/gpu.mjs` akışı yürütür (mesh ve
boru hattı önbelleği, 120 kare görülmeyen mesh bırakılır); karar vermez. WGSL motorda gömülü (`assets/gpu_8a.wgsl`):
köşe dönüşümü `vert_std_c` sırasıyla; `fs_flat` = `shade_flat`'in birebir eşi → iki yol kaplama + derinlikte
karşılaştırılabilir (`web/gpu_check.html`). 8a sınırları: Material.vert (dalga) ve shader'lar GPU'da yok (8b: Matoki →
WGSL), panel / editör / gölge yazılım kipinde (8c).


## D39. Fizik ↔ görüntü ara değerlemesi (`phys/interp`)
Fizik sabit 60 Hz adımda kalır (belirlenimcilik, geri sarma); görüntü önceki ↔ şimdiki adım pozu arasında
α = birikmiş süre / adım ile çizilir (bir adım geriden, düzgün). Ölçüm (`test_phys_interp`, serbest düşme, kare başı
hız değişiminin −g·Δt'den RMS sapması): 144 Hz kare 38,3 → 0,054 m/s, 75 Hz kare 14,3 → 0,016 m/s. Uygulama: her
adımdan önce `phys_interp_save`; adımlardan sonra `apply` ara pozu gövdelere yazar (konum doğrusal, dönüş nlerp,
`rot_m` türetilir) — kamera takibi, gölge geçişi ve ana geçiş tek pozu görür, ayrı çizim yolu gerekmez; `pipe_end`
sonrası `restore` şimdiki pozu bit bit geri koyar (test: 50 adımda ara değerli ↔ ara değersiz dünya birebir; demo:
`check_demo` 144 Hz'de fizik durumu aynı). Önceki = şimdiki (uyuyan, statik) gövdeye dokunulmaz; geçmişi olmayan
(yeni eklenen, ışınlanan: `car_reset`, araçtan iniş, sahne kurulumu → `interp_reset`) gövde olduğu gibi çizilir —
ölçüm görünümleri adım atmadığı için kare hash'leri değişmez. Yumuşak cisim parçacıkları ve karakter `PhysInterpPts`
ile, sahne 8 DEM topları (f64) çizimde okunurken. Maliyet: 1024 gövdede save 3,5 µs + apply/restore 20,6 µs / kare.
I tuşu aç / kapa. Yavaş karedeki ağır çekim: D40.

## D40. Kare başı fizik adımı: sayı değil zaman bütçesi
Önceden kare başına en çok 4 adım (+ dt 0,1 s'de kırpma): 15 fps altında fizik ağır çekime giriyordu (10 fps %67,
8 fps %53, 4 fps %27 hız). Şimdi ilk adım her zaman, sonrakiler fizik bu karede max(4 ms, dt / 2) altında kaldıkça;
üst sınır 15 adım = kare dt'si kırpması 0,25 s. Fizik karenin en çok yarısını alır → kare ≤ 2 × render, ölüm sarmalı
olamaz; bütçe dolarsa artan süre atılır (sınırlı ağır çekim). Ölçüm (node, tek thread): sahne 3 / 5 / 6, 60 → 4 fps
tümünde hız 1,000; 939 gövdeli sahne 3'te 4 fps'te adım 1,2 ms, 15 adım 17 ms (bütçe 125). Bütçe dalı: oran geçici
%1'e indirilince sahne 6 fizik ~5 ms/kare'de kaldı, hız 0,14'e düştü (beklenen). `check_demo`: 8 / 4 fps'te hız
> 0,99 ve fizik ≤ bütçe + bir adım (eski yapıda üç hız denetimi başarısız: 0,533 / 0,533 / 0,267).
Bulgu: sahne 6 yumuşak cisim adımı 2,6 ms (tek thread, 10 alt adım) — 60 fps karenin belirgin payı: D41.

## D41. Kumaş öz-çarpışmasında sıradan bağımsız ön tarama
Profil (sahne 6 eşi, adım ~2,6 ms): öz-çarpışma %45, ama bayrakta menzile giren çift hiç yok (adım başı 103 680 kova
yoklaması, 60 648 aday, 0 düzeltme). Sıralı Gauss-Seidel döngüsü düzeltme yapana dek konum değiştirmez: `self_scan`
yalnız okuyarak ilk düzeltecek parçacığı (menzildeki çiftlerin min(i, j)'si) bulur, döngü oradan başlar → sonuç bit bit
aynı. Tarama sıra korumak zorunda olmadığı için yarım kabuk (27 yerine 14 kova, her çift bir kez); kova anahtarı
parçacık başına bir kez. A/B (eski ↔ yeni dönüşümlü, 6 tur × 600 kare, üçer koşu): 1 thread −%23,4 … −%24,0,
16 thread −%20,7 … −%23,6 (noise ±%2-4). Taramayı thread'lere bölmek denendi, ölçümde daha yavaş (aynı yapı sıralıya
göre +%12-13: kısa iş, her çağrıda worker uyanması) → sıralı. Sürekli çarpışan kumaşta da yavaşlama yok (adım
0,997 → 0,926 ms). Doğrulama: `soft_demo_scene` hash'i ve katlanan kumaş seed 0 hash'i değişiklik öncesi koddan sabit;
12 rastgele katlanmada (52 471 temas) taramalı ↔ `self_no_scan` birebir; üç bilerek bozma (son hücre, yarım tarama,
min yerine i) testte yakalanır. Kalan: jöle kısıtları, üçgen teması, bayrak kısıtları — sırası bağımlı, bit bit aynı
kalan kolay kazanç görülmedi; alt adım sayısını azaltmak sonucu değiştirir, ayrı karar.

## D42. Matoki → WGSL: AST değil IR'dan, doku motorun kendi düzeniyle (M13 faz 8b)
`matoki/wgsl.c` JIT'in kullandığı IR'dan üretir: düz SSA, canlı komut başına bir skaler `let`. sin, pow, exp2, doku süzme
zaten IR aritmetiği olduğundan iki arka uç aynı işlemleri aynı sırayla yapar; WGSL'in yerleşik `sin` / `textureSample`
işlevleri kullanılmaz (sürücüye göre değişirlerdi). Lane'e özgü semantik donanıma eşlenir (kaba türev, `@builtin(position)`).
Uniform bloğu C yapısının ham baytları (`array<vec4u, 16>`, 256 baytlık yuva; köşe `@binding(0)`, parça `@binding(1)`).
Doku: `Texture` başlığı (88 sözcük) + px tek storage tampon; IR'ın doku okumaları doğrudan bu tampona. Donanım örnekleyicisi
değil, bilerek: bilinear / aniso / mip seçimi yazılım yoluyla aynı kalır. Bedeli GPU'da performans; ölçümü ve gerekirse
donanım dokusuna geçiş 8c. `gcmd` akışı sürüm 2: `GC_TEX`, çizim başına köşe / parça programı, iki uniform yuvası (ardışık
aynı materyal parça yuvasını paylaşır), 8 köşe akışı (uv dahil). Dalga köşe shader'ı artık GPU'da da çalışır. Bilinen sınır:
doku kimliği adres + px + boyut, içerik sürümü yok; demo dokuları sabit, gölge haritası (8c) sürüm isteyecek.

**Kabul** (`web/gpu_check.html`, Intel Gen12LP, 640×360, MSAA ve gölge kapalı, 10 görünüm):

| Shader | Geçen | Fark > 8 olan piksel (en kötü) | Ortalama kanal farkı (en kötü) | En büyük kanal farkı | Derinlik hatası |
|---|---|---|---|---|---|
| düz | 10/10 | %0 (tolerans 0) | 0 | 0 | 0 |
| lit | 10/10 | %0,055 | 0,103 | 228 | 0 |
| lit_hq, su, gök | 10/10 | %0 | 0,164 | 8 | 0 |

lit'in büyük farkları 1 piksellik kaymalar: en yakın komşu dokunun ve kaplamanın sert kenarlarında (10 görünümde 496 pikselin
492'sinde aynı renk 3×3 komşulukta var). Eşik %0,1. Canlı derleme gerçek çizimde: kırmızı shader sonrası 37 893 kırmızı
piksel, iki yol arasında 0 fark. Yazılım yolu: kare hash'leri aynı; tek thread A/B (11 tur, görünüm 1 / 3 / 6) oran
0,995 / 0,988 / 1,005. engine.wasm 590 275 → 599 573 bayt. WGSL üretimi 0,03–1,0 ms / program, metin 4–78 KB.
İlk kare `?gpu`'da boru hattı derlemesiyle yaklaşık 2 kat uzun (1280×720, 42 ms ↔ 22 ms, lit).

Test: görüntü farkı anlamı koruyan yanlışları görmez: `round` → `floor` (`exp2` içinde) ortalama farkı değiştirmedi, denetimden
geçti. Bu yüzden `test_matoki_wgsl` her IR komutunun ifadesini sabitler; aynı bozma orada yakalanır. Diğer iki bozma (`dpdy`
yerine `dpdx`, doku başlığı −1) görüntü testinde %14–23'e varan farkla yakalanır. Sayısal sınır durumları (NaN, doygun
kesme) WGSL belirtimine dayanır, donanımda tek tek sınanmadı; yalnız bir adaptörde koşuldu. Kalan: 8c (MSAA, gölge, panel, GPU süresi).

## D43. Quad-sabit koşula gerçek dal; lit_hq'da uyarlanabilir anizotropi
Sorun: HQ, lit'in 2-3 katı (640×360, 16 thread: kare başına 3-6 ms fazla), çünkü her piksel 8 × 2 = 16 bilinear örnek alıyordu.
Ölçüm: 8 → 4 → 2 örnek görünüm 1'de 35 → 24 → 20 ms (tek thread); piksellerin çoğu 8 örneğe gerek duymuyor. Ama IR düz: `if` ve
`select` iki kolu da hesaplar, örnek sayısını seçmek tek başına kazandırmaz.

Derleyici (`matoki/ir_guard`): maskesi quad'ın tüm lane'lerinde aynı ve tam (0 / ~0) olan select'te yalnız bir kola yarayan
komutlar wasm `if` içine girer. Select yerinde kalır, seçilmeyen kolun yereli okunup maskeyle atılır: sonuç bit bit aynı.
Ters SSA geçişi, düğüm = kullanıcı kenarlarının en yakın ortak atası. Grup yalnız bitişik, aynı maskeli select'ler (vec
bileşenleri tek dal). Derinlik en çok 16; iki kol toplamı 8 komuttan azsa dal açılmaz; çizim-sabit select'ler önsözde kalır.
WGSL aynı planı kullanır (kol değerleri `var`), türevli kol dallanmaz (düzgünlük analizi). Tüm Matoki shader'ları yararlanır.

Shader (`lit_hq.mtk`): örnek sayısı quad başına büyük / küçük eksen oranından: 1 (oran² ≤ 1,5), 2 (≤ 4), 4 (≤ 16), 8. Seviye
tam sayıysa ikinci mip seviyesi okunmaz. GPU'lardaki anizotropiyle aynı fikir; görüntü değişir, HQ hash'leri bu yüzden yeni.

| Görünüm (640×360, 8 thread, MSAA + gölge kapalı) | Eski ms | Yeni ms | Oran | Eskiye göre PSNR (dB) | Fark > 8 piksel |
|---|---|---|---|---|---|
| 1 | 5,67 | 4,21 | 0,742 | 54,5 | %0,00 |
| 2 | 6,13 | 3,22 | 0,525 | 52,0 | %0,33 |
| 3 | 3,49 | 2,25 | 0,646 | 57,3 | %0,03 |
| 4 | 3,72 | 2,99 | 0,804 | 52,9 | %0,03 |
| 5 | 3,56 | 2,62 | 0,735 | 50,8 | %0,52 |
| 6 | 8,16 | 6,78 | 0,831 | 44,2 | %0,73 |
| 7 | 3,84 | 2,91 | 0,758 | 55,7 | %0,02 |
| 8 | 4,59 | 3,21 | 0,700 | 51,3 | %0,02 |
| 9 | 5,18 | 3,71 | 0,716 | 52,9 | %0,00 |
| 10 | 6,33 | 3,73 | 0,589 | 54,7 | %0,08 |
| Toplam | 50,7 | 35,6 | 0,703 | | |

Varsayılan ayarlar (1280×720, 8 thread, 7 görünüm toplamı): gölge açık oran 0,668; gölge + MSAA 4x oran 0,780 (ör. görünüm 10:
50,9 → 35,4 ms). Değişiklik yalnız HQ'da: `lit` ve C shader hash'leri birebir aynı. Fark ×8 büyütülünce damalı zeminde örnek
sayısının değiştiği yerlerde soluk bantlar; hareket halinde titreşim ayrıca ölçülmedi. Bedel: lit_hq wasm 59,8 → 103,7 KB (JIT
hâlâ yaklaşık 3 ms), engine.wasm +7,2 KB. GPU: `gpu_check` 30/30, lit_hq en kötü fark > 8 %0,029, ama GPU süresi ölçülmedi.

Doğrulama: `test_matoki_guard` (9 test) JIT tam gövde ve önsöz + hızlı gövdeyi `ir_eval` kâhiniyle bit bit karşılaştırır, gerçek
lit_hq dahil. Bilerek bozma: kollar ters, lane'e bağlı maske, kısmi maske, bitişiklik denetimi yok. İlk sürüm sonuncuyu yakalamadı;
`guard_same_mask_not_adjacent` bunun için eklendi, dördü de yakalanıyor. Test sırasında bulunan ayrı kusur: dışa açık
`engine_matoki_recompile` her başarılı derlemede `g.perm`'i büyütür, tekrarlanınca panikler; editör kendi arenalarını kullandığı
için oyuncu etkilenmez, araç / test yolu.

## D44. GPU'da gölge, MSAA, bindirme ve süre ölçümü (M13 faz 8c)
Gölge: aynı `render_scene` akışa yalnız derinlik çizer (`GC_SHADOW`, parça programsız), derinlik `copyTextureToBuffer` ile doku
tamponuna kopyalanır; `lit_hq.mtk` değişmez, aynı `texel_fetch` + `bitcast_f32` okur. MSAA: GC_FRAME örnek sayısı taşır; yazılım
örnek konumları standart 4x desenle aynı, çözümleme donanımda. Bindirme: panel kodu değişmez, siyah ve beyaz zeminde iki kez
çizilir, alfa aradaki farktan (test: 8 alfa × zemin, en büyük kanal hatası 1). Editör girdi işlediği için bir kez çizilir: opak.
GPU süresi: `timestamp-query` (yoksa kuyruk gecikmesi, "üst sınır" etiketiyle); panel ve sayfa gösterir, yazılım satırları GPU'da
gizlenir. Aynı boyutlu dinamik mesh'te tampon yeniden ayrılmaz; kare başı 10 MB akış / yuva sıfırlanmaz (ayrı ölçülmedi).

**Kabul** (`web/gpu_check.html`, Intel Gen12LP, 640×360, 10 görünüm; fark > 8 piksel, eşik %0,1, düz %0,5 / tolerans 0):

| Kip | Geçen | En kötü | Ortalama kanal farkı |
|---|---|---|---|
| düz | 10/10 | %0 | 0 |
| lit | 10/10 | %0,055 | 0,103 |
| lit_hq | 10/10 | %0,029 | 0,165 |
| lit_hq + gölge | 10/10 | %0,029 | 0,165 |
| lit + MSAA 4x | 10/10 | %0,056 | 0,135 |
| lit_hq + gölge + MSAA 4x | 10/10 | %0,030 | 0,217 |

Denetimin gerçekten sınadığı: gölgenin değiştirdiği piksel iki yolda 3373 ↔ 3372, 785 ↔ 785, 2366 ↔ 2367, 2326 ↔ 2326; MSAA'nın
11171 ↔ 11120, 1306 ↔ 1311. Bilerek bozma: derinlik kopyası yok → %53'e varan fark; örnek sayısı hep 1 → %4,4'e varan fark. Derinlik
karşılaştırması yalnız MSAA kapalı kiplerde (çok örnekli derinlik kopyalanamaz).

**1080p** (tarayıcı, 16 thread, Intel tümleşik GPU, ms; GPU = zaman damgası, çalışma sırasında motor CPU'su serbest):

| Görünüm | Varsayılan (HQ + gölge + MSAA): yazılım | motor | GPU | Gölge / MSAA yok: yazılım | motor | GPU |
|---|---|---|---|---|---|---|
| 1 | 56,3 | 0,63 | 27,3 | 27,1 | 0,39 | 23,2 |
| 2 | 57,0 | 0,58 | 15,0 | 24,1 | 0,29 | 10,4 |
| 4 | 55,2 | 0,23 | 20,6 | 24,4 | 0,17 | 18,5 |
| 5 | 39,8 | 0,30 | 16,3 | 20,4 | 0,21 | 14,5 |
| 6 | 66,8 | 1,85 | 27,6 | 36,4 | 2,29 | 24,2 |
| 8 | 46,9 | 0,24 | 17,6 | 24,4 | 0,18 | 13,8 |
| 10 | 44,3 | 0,30 | 17,4 | 20,6 | 0,14 | 11,1 |

Varsayılanda GPU 2-3 kat kısa, ayrıca motor CPU'su neredeyse tamamen boşalır. Gölge ve MSAA kapalıyken fark küçük (1,2-2,3 kat):
doku okuması donanım örnekleyicisi değil, storage tampon + 16'ya kadar bilinear IR'da (D42'nin bilinçli bedeli). Sıradaki büyük
GPU kazancı büyük olasılıkla burada; donanım dokusu görüntüyü değiştirir, ayrı karar. Sınırlar: tek adaptör. Bu ölçüm döngüsünde
kare başına duvar süresi 15-41 ms (kuyruk bekleme dahil); ön izleme bölmesi rAF'ı kıstığı için gerçek sayfada FPS ölçülemedi (yazılım
da 1 FPS gösterdi). Gölge geçişi zaman damgası 0-0,7 ms okudu; bu adaptörde güvenilirliği doğrulanmadı.

Yayın dersi: ilk Pages denetimi 32/60 verdi (yerelde 60/60). Neden kod değil: tarayıcı `max-age=600` ile eski `gpu.mjs`'i yeni
`engine.wasm`'la çalıştırdı, sessizce bozuk çizdi. Düzeltme: `coi.js` aynı kökenli dosyaları her açılışta sunucuya sordurur;
`createGpu` akış sürümünü denetler, uyuşmazsa kurulum düşer ve sayfa yazılım render'da kalır (`check_demo` iki sürümün eşitliğini denetler).

## D45. Shader kalitesi: parlaklık, kabartı, yumuşak gölge, renkli ortam, su kırışıklığı
Bütçe: kaliteyi yazılımda ortalama %5'in altında, GPU'da %20'nin altında almak. `lit.mtk` ve C shader'ları değişmez; görünüm 6 dışında
`lit` hash'leri aynı (o görünümde su, `water.mtk`).

`lit_hq.mtk`:
- Gölge: 2×2 → 4×4 üçüncü derece B-spline (basamaksız, ≈ 4 texel kenar). Her komşu alıcı düzleminin kendi derinliğiyle karşılaştırılır
  (ekran türevlerinden, kenetli): geniş çekirdek eğik yüzeyi kendi gölgesi saymasın. Tekerlek altındaki çizgili gölge lekesi de böyle gitti.
- Ön bakış: 5 nokta (quad ayak izi kadar açık köşeler + orta) quad'da hep aydınlık / hep gölgeyse 16 okuma atlanır (D43 dalı). Koşul quad-sabit
  olmalı; `q = dFdx(probe · piksel x)` probe sabitken değerin kendisini verir. Türevler dördüncü lane'i görmez: köşelerin açık olması bunun için.
- Ortam: tepe gök, dip zemin rengi; gölgeler soğuk dolgu alır. Parlaklık: Blinn-Phong + Fresnel'li gök yansıması, `LitUniforms.spec / gloss`.
  `pow` yerine `(1 - gloss (1 - x) / 16)^16`: görünüm 2'de `pow` +%37'ydi (düz), ayrıca tek bölmeli Schlick yaklaşımının kuyruğu düz yüzeyi yıkıyordu.
- Kabartı: doku parlaklığı yükseklik, normal ekran türevlerinden (Mikkelsen 2010), `LitUniforms.bump`. Türevler kolun dışında: türevli kol GPU'da
  dallanamaz, yoksa kabartısız zemin de öder (görünüm 8: 20,7 → 17,0 ms). Dezavantajı 2×2 blok; yalnız tuğla ve ızgarada açık, zemin ve kumaşta kapalı.

`water.mtk`: normale üç yönlü sinüs eğimi, uzakta söner. Zaman `LitUniforms.wrap`'in yerinde (`time`, su wrap okumaz).

**Maliyet** (A/B aynı süreçte dönüşümlü, 1920×1080, 16 thread, Intel Gen12LP; toplam = 10 görünüm):

| Yol | Ayar | Önce | Sonra | Oran | Görünüm aralığı |
|---|---|---|---|---|---|
| Yazılım | gölge + MSAA | 598 | 622 | 1,040 | 1,009 – 1,122 |
| Yazılım | düz | 346 | 355 | 1,026 | 0,917 – 1,229 |
| GPU (zaman damgası) | gölge + MSAA | 186 | 217 | 1,167 | 0,994 – 1,577 |
| GPU (zaman damgası) | düz | 160 | 172 | 1,077 | 1,028 – 1,146 |

Yazılım mutlak süreleri koşudan koşuya ×2 oynadı (ısınma / başka yük), yalnız oranlar anlamlı. GPU'da en çok gölge kenarı kaplayan görünümler (2, 3, 10:
+5..7 ms) artar. Bu adaptörde 1080p'de görünüm 8 ve 9 varsayılanda 16,6 ms sınırını aşar; gölge açık GPU maliyeti bütçede ama FPS'e bedava değil.

**Ön bakışın hatası** (tam 16 okumaya göre, 1920×1080, gölge + MSAA): fark > 8 piksel en çok 339 (%0,016, görünüm 7, top temas bölgesi), yazılımda %3,4
ve GPU'da daha büyük kazanç. Bilinen kusur: üst üste topların temasında ve tekerlek yanında birkaç blok parlak quad kalır; 1 texel'lik ince gölgeler
ön bakıştan kaçabilir.

Kabul: `gpu_check` 60/60 (en kötü %0,056), 255/255 test. Yeni test `lit_hq_shadow_factor_in_range`; koşuldan türev denetimi çıkarılınca (`q` taşar, beyaz benek)
yakalar. Gözle bulunan iki hata testten önce buradaydı: `q`'nun lane'in kendi değerinin yerine geçmemesi ve koşuldaki toplamın taşması.

Bilerek yapılmadı: suda düzlem yansıması (ek geçiş), harita çözünürlüğünü artırmak (basamak boyu 1024 texel'den geliyor; bellek ×4), güneş diski dışında gökyüzü
detayı. Sınır: tek adaptör, gerçek ekranda FPS ölçülmedi (D44).

## D46. Gölge kademeleri: yakın keskin, uzak geniş
Basamak boyu çekirdekten değil haritadan geliyordu: 22 m yarıçapa 1024 texel = 4,3 cm, arabada ekranda 15 piksellik basamak. İki kademe,
tek doku: `size × 2·size` atlas (bellek ×2), yakın 9 m (1,8 cm), uzak 42 m (8,2 cm; eskiden 40 m ötesinde gölge yoktu). Tek doku çünkü GPU yolunda doku yuvası
sınırlı ve depolama tamponunda kademe yalnız satır ofseti. Kademe seçimi lane başına, türevler kademe başına: quad'ın iki kademeye düştüğü dikişte
türev sıçramaz. Sahne 7 (göl) artık gölgeli (su gölge düşürmez; okyanus ağı kare başında bir kez güncellenir). Akış sürümü 4 (`GC_SHADOW` kademe bilgisi taşır).

Maliyet (A/B, 1920×1080, gölge + MSAA, 10 görünüm toplamı): yazılım 707 → 736 ms (×1,041; görünüm başına ×1,018 – ×1,067). Hemen hemen tamamı ikinci derinlik geçişi;
yakın kademe frustum elemesiyle ucuz. GPU A/B yapılamadı (eski motor yeni akış sürümüyle kurulmaz), son ölçüm D49'da. Kabul: `gpu_check` 60/60, 256/256 test.

## D47. Suda düzlem yansıması
Su artık sahnenin kendisini yansıtır. Ayna R: p → (x, 2h − y, z), sanal görüş V·R, göz R(kamera), 1024×512 ayrı hedef (satır adımı 2'nin kuvveti ister). Ayna sarımı çevirir; izdüşümde x de çevrilince iki çevirme
birbirini götürür, malzeme ayıklaması ve boru hattı değişmez, su haritayı 1 − u'dan okur. Düzlemin altı eğik yakın düzlemle kırpılır (Lengyel 2005): sütunların su altı parçası kameraya binmez, ayrı kırpma kodu yok.
Gök haritaya çizilmez: temizleme alfası 0, su `mirror.rgb + gök · (1 − a)` ile her piksel için analitik gök hesaplar (çift doğrusal süzme önceden çarpılmış olduğu için kenarda koyu hale yok). Harita normalle saptırılır, uzakta az.
GPU: `GC_REFLECT` / `GC_REFLECT_END`, renk doku tamponuna kopyalanır. Geçişler arasında göz değişir: `gcmd_reflect_end` aynı malzemenin yuva paylaşımını keser (test `gcmd_reflect_stream`). Akış sürümü 5.

Maliyet (göl görünümü 6, 1920×1080, yazılım, aynı motor açık / kapalı): gölge + MSAA 105,0 → 108,2 ms (×1,031), düz 59,0 → 59,7 ms (×1,012); gök ve zemin çizilmediği için geçiş ucuz. Yalnız sahne 7 öder.
Kabul: `gpu_check` 60/60, görünüm 6 en kötü %0,036, 257/257 test. Sınır: tek düzlem; dalga yüksekliği düzleme yansımaz, yalnız normalle bükülür.

## D48. Fizik tabanlı ışık: GGX, doğrusal uzay, tek saçılmalı gök
`lit_hq` gama uzayında Blinn-Phong'du. Şimdi doğrusal uzayda (doku ve ışık sayıları kare alınarak) enerji korunumlu Lambert + GGX: Trowbridge-Reitz dağılımı, Smith görünürlüğü (Hammon), Schlick Fresnel; `LitUniforms.metal / rough`.
Çevre yansıması analitik gökten, DFG için Karis 2014; ufuk çizgisi pürüz kadar bulanır. Çıkış ACES (Narkowicz) + gama 2. Sis görüntü uzayında: gök rengiyle dikişsiz kalsın diye tone'dan sonra; Beer-Lambert üsteli kanal başına (k, 1,5k, 2k): kısa dalga boyu daha çabuk saçılır.
`rough ≥ 0,97` kolu çalıştırmaz (zemin ödemez). `sky.mtk`: Rayleigh evresi 3/4 (1 + cos²θ), hava kütlesiyle ufka geçiş, Mie için Henyey-Greenstein (g = 0,8), `pow` yok; bantlaşma için ±½ basamak titreşim. Ufuk tam `fog_color` kalır.
Kapsam: `lit` kipi ve C shader'ları değişmez, ama `sky.mtk` her kipte çizilir: tüm kiplerin gök hash'leri değişti (bilinçli).

Maliyet (A/B, 1920×1080, yazılım, 10 görünüm): gölge + MSAA 700 → 752 ms (×1,075; görünüm 2'nin A değeri o koşuda aykırı düşük, kalanlar ×0,99–×1,047), düz 395 → 416 ms (×1,052). Kabul: `gpu_check` 60/60 (HQ en kötü %0,019), 257/257 test.

## D49. Fizik alanları: shader fiziğe göre şekil alır
Temel fizik mimarisi değişmedi; yeni denklemler üstüne eklenir ve shader'a tek bir yoldan ulaşır: **fizik modülü alanı hesaplar, dokuya yazar (`Texture.gen++`), malzeme okur.** Fizik ↔ shader arasında tek sözleşme doku kanallarıdır; yeni bir denklem için shader dili ya da boru hattı değişmez, GPU kopyası `gen` değişince kendiliğinden yenilenir (Texture ABI 356 bayt, `GC_TEX` `gen` taşır, parça başına 3 doku, akış sürümü 6).

- **Dalga denklemi** (`phys/wave`, göl): h_tt = c²∇²h − k·h_t, 256² ızgara, leapfrog, r² = 0,25, c = √(g·0,9 m) ≈ 3 m/s. Sandıkların batan derinliği değişince su yer değiştirir (kaynak), sütunlar sabit uçlu engel: halka dalgalar yansır ve kırınır. Çıktı eğim dokusu (`water.mtk` normali) + okyanus ağı yüksekliği. Testler: cephe c·t'de (beklenen 18 m, ölçülen 18), ayrık enerji sönümsüz korunur, sönümle azalır, engel arkası gölgede ve önüne yansır.
- **Kumaş gerinimi** (`soft_strain_field`, bayrak; çekirdeğe yazmaz): gerilme kısıtlarından uzama, büküm kısıtlarından katlanma. `lit_hq`: gerilme lifi açar (renk açılır, arkadan ışık artar), katlanma ortam ışığını keser. Alan yoksa (`field_scale = 0`) kol hiç çalışmaz.

Maliyet (A/B, 1920×1080, gölge + MSAA, yazılım): fizik adımı çalışmayan karede toplam 742 → 742 ms (×1,001); göl görünümü, adımlı kare 48,1 → 50,2 ms (≈ 2 ms: 256² adım + doku). Kabul: `gpu_check` artık bayrak ve gölü 20 kare ilerletip karşılaştırır, 60/60; `gen` yüklemesi ve kademe ofseti bilerek bozulunca yakalanır. `check_demo`: sandıklar dalga üretir, bayrak alanı dolar. 263/263 test.
Sınır: dalga yalnız göl düzleminde, jöle gövdeleri henüz alan yazmıyor; bayrak alanının görsel etkisi ince. Gerçek ekranda FPS ölçülemedi, tek adaptör.

## D49. Fizik alanları: shader fiziğe göre şekil alır
Temel fizik mimarisi değişmedi; yeni denklemler üstüne eklenir ve shader'a tek yoldan ulaşır: **fizik modülü alanı hesaplar, dokuya yazar (`Texture.gen++`), malzeme okur.** Sözleşme doku kanallarıdır: yeni denklem için shader dili ya da boru hattı değişmez, GPU kopyası `gen` değişince yenilenir (Texture ABI 356 bayt, `GC_TEX` `gen` taşır, parça başına 3 doku, akış sürümü 6).

- **Dalga denklemi** (`phys/wave`, göl): h_tt = c²∇²h − k·h_t, 256² ızgara, leapfrog, r² = 0,25, c = √(g·0,9 m) ≈ 3 m/s. Sandığın batan hacmi değişince su yer değiştirir (hacim korunumlu kaynak), sütunlar sabit uçlu engel: halkalar yansır ve kırınır. Çıktı eğim dokusu (`water.mtk` normali, görünür eğim ×5: ızgaranın taşımadığı kısa dalgaların yerine) + okyanus ağı yüksekliği. Testler: cephe c·t'de (beklenen 18 m, ölçülen 18), ayrık enerji sönümsüz korunur, sönümle azalır, engel arkası gölgede ve önüne yansır.
- **Kumaş gerinimi** (`soft_strain_field`, bayrak; çekirdeğe yazmaz): gerilme kısıtlarından uzama, büküm kısıtlarından katlanma. `lit_hq`: gerilme rengi açar ve arkadan ışığı artırır, katlanma ortam ışığını keser. Alan yoksa (`field_scale = 0`) kol çalışmaz.

Maliyet (1920×1080, gölge + MSAA, yazılım): fizik adımı çalışmayan karede 10 görünüm 742 → 742 ms (×1,001), göl adımlı kare 49,3 → 51,2 ms (≈ 2 ms: 256² adım + doku).

**D46–D49 sonrası GPU** (zaman damgası, Intel Gen12LP, 1080p; D45'te toplam 217 / 172 ms): gölge + MSAA 10 görünüm 240 ms (19,3 – 33,8), düz 170 ms (12,9 – 26,4); motor CPU'su 0,2 – 2,6 ms. Bu tümleşik GPU'da 1080p varsayılan ayar 60 FPS tutmaz (D45 öncesi de en iyi 14 ms, en kötü 27 ms'ydi); gölge + MSAA kapalıyken 10 görünümün 7'si 16,6 ms'nin altında.

Kabul: `gpu_check` artık bayrak ve gölü 20 kare ilerletip karşılaştırır, 60/60. Bozma denemeleri: `gen` yüklemesi kapatılınca görünüm 6 %0,31–0,43 ile düşer (ilk denemede yakalanmıyordu: etki çok zayıftı, kaynak ve eğim düzeltildi), kademe ofseti bozulunca hq+gölge tüm görünümlerde %11–34 ile düşer. `check_demo`: sandıklar dalga üretir, bayrak alanı dolar. 263/263 test.
Sınır: dalga yalnız göl düzleminde, jöle gövdeleri henüz alan yazmıyor, bayrak alanının görsel etkisi ince. Gerçek ekranda FPS ölçülmedi, tek adaptör.

## D50. GPU'da donanım bilinear: görüntü aynı, kare süresi yarı
Ölçüm: 1080p, gölge/MSAA'sız ana geçiş görünüm 4'te 20,3 ms; aynı shader tek bilinear okumayla 6,3 ms, dokusuz 4,5 ms. Süre anizotropinin her örneğinde depolama tamponundan IR ile yapılan 4 okuma + ağırlıktaydı. Anizotropi mantığı shader'da aynen kalır, yalnız her bilinear `sample_level` donanıma gider: WGSL için IR ayrıca indirilir (`lower_module_gpu`, `IR_HW_SAMPLE`), JIT IR'ı ve yazılım yolu değişmez. Depolama tamponu başlık ve `texel_fetch` (gölge) için kalır. `lit_hq` WGSL'i 182 → 44 KB.

| GPU, 1080p (zaman damgası, A/B aynı sayfada; 10 görünüm) | Önce | Sonra | Oran | Görünüm aralığı (sonra) |
|---|---|---|---|---|
| gölge + MSAA | 252 ms | 124 ms | 0,491 | 9,1 – 18,2 ms |
| düz | 185 ms | 76 ms | 0,410 | 5,6 – 12,3 ms |

Varsayılan ayarda 10 görünümün 9'u 16,6 ms altında (görünüm 2, ekranı dolduran küpler: 18,2 ms). Görüntü yazılımla daha da yakın: `gpu_check` 60/60, HQ en kötü %0,019. Dinamik doku yazımı bilerek kapatılınca görünüm 6 denetimde düşer.

Ek: `lit_hq`'da "brilinear": seviye geçişi kesrin orta bandında, uçlarda tek seviye. Yazılım toplam ×0,972 (gölge + MSAA) / ×0,984 (düz), görüntü farkı en kötü 636 / 2 073 600 piksel > 8. Canlı derleme (`engine_matoki_recompile`) kalıcı arenayı büyütüyordu; editörle aynı iki arenaya alındı, `check_demo` 400 derlemeyi art arda sınar.
Sınır: tek adaptör, sekme zaman damgası ölçümü; gerçek ekranda FPS ölçülmedi.

## D51. Suda gölge, parlaklık yumuşatma, jöle alanı
- **Su gölgesi:** `water.mtk` gölge haritasını okur: gök yansıması kalır, güneş parıltısı ve gövde ışığı kesilir. Dalga alanı `field` yuvasına taşındı (parça başına 3 doku).
- **Geometrik parlaklık yumuşatma** (Kaplanyan 2016): normalin piksel içi değişimi kadar GGX lobu genişler; eğri yüzeyde ve kabartıda parlak nokta kırpışmaz. Türevler kolun dışında.
- **Jöle** artık bayrakla aynı sözleşmede: `soft_strain_field` kenar kısıtlarından uzama / ezilme verir, gövde başına doku (uv'ye yazılır, boş texel komşudan). Üç jöle ayrı uniform taşır. Yeni görünüm 11: `gpu_check` jölelerle birlikte 66 karşılaştırma.
- Akış sürümü 7: D50 doku bağlama düzenini değiştirdi; eski `gpu.mjs` + yeni motor bozuk çizmek yerine kurulmaz.

Maliyet (A/B, 1920×1080, yazılım, 10 görünüm): gölge + MSAA 554 → 554 ms (×1,001), düz 310 → 309 ms (×0,999). GPU (zaman damgası, tek motor): gölge + MSAA 10 görünüm 138 ms (10,4 – 18,4 ms; 2 ve 6 sınırın üstünde), düz 91 ms (6,8 – 15,7 ms; hepsi 16,6 ms altında). D50'ye göre artış (124 → 138, 76 → 91) esas olarak bu turun GPU'da dallanamayan türevleri ve suyun ek okumaları; ayrı ölçülmedi.
Kabul: `gpu_check` 66/66 (en kötü %0,056), 263/263 test. Sınır: jöle alanı dinlenmede neredeyse boş, yalnız ezilmede görünür.

## D52. Gerçekçi malzeme ve gök: kare maliyeti eklemeden
Hedef: görüntü test sahnesi gibi durmasın, FPS düşmesin. Bu yüzden iş kare başına hesaba değil, başlangıçta bir kez üretilen dokulara kondu.
- **Dokular** (`img/procgen`, periyodik fbm, dikişsiz): beton döşeme, kaya, çim, kumaş, yıpranmış boya; dama ve ızgara demodan kalktı. Tuğlada harç artık koyu: kabartı parlaklıktan okunduğu için açık harç dışa çıkıyordu.
- **Bulut** (`sky.mtk`): piksel başına tek bilinear okuma, doku R yoğunluk, G güneş tarafı aydınlığı, saatle kayar. Yansımaya girmez: su analitik göğü yansıtır.
- **Ortam:** gölge çarpanı ortamı %22'ye kadar kısar (ek okuma yok), ortam ×1,24: ACES'in karanlık ucu gölgeyi ezmesin. Renkler doygunluktan çekildi.
- **Zemin:** küçük zemin kalktı, her sahne tek 4 km zemin (doku 4 m, taş 0,5 m, 16 parça). Dev tek üçgende yazılım ↔ GPU farkı sınırı aşıyordu (görünüm 1 %0,26, görünüm 8 derinlik). Sis 450 → 160 m: ufuk birkaç piksele sıkışıp sert çizgi yapıyordu.
- **Maliyeti karşılayan:** gök ve zemin en sona çizilir, kapanan piksel derinlikte elenir, gölgelenmez. Seviye ≥ 3'te anizotropi en çok 2 örnek.

| 1920×1080, A/B aynı süreç, 10 görünüm toplamı | Önce | Sonra | Oran |
|---|---|---|---|
| Yazılım, gölge + MSAA | 666 ms | 641 ms | 0,962 |
| Yazılım, düz | 411 ms | 394 ms | 0,958 |
| GPU, gölge + MSAA | 149 ms | 140 ms | 0,94 |
| GPU, düz | 95 ms | 89 ms | 0,94 |

Kazanç eşit dağılmadı. Gök ağırlıklı görünümler pahalandı: yazılımda görünüm 3 ×1,24–1,39, görünüm 5 düz kipte ×1,16; GPU'da görünüm 3 varsayılanda 11,7 → 15,1 ms. En ağır görünümler ucuzladı: görünüm 6 yazılımda ×0,75–0,79, GPU'da 21,2 → 14,4 ms. GPU varsayılan ayarda 16,6 ms üstü: görünüm 2 (19,5) ve 10 (16,8). Başlangıç 48 → 129 ms (1080p, doku üretimi). Bellek: dokular + mip'ler yaklaşık 2,9 MB.

Kabul: `gpu_check` 66/66 (en kötü %0,029), 268/268 test (iki kipte), `check_demo` geçti (ara değerleme kıyasında gök kapalı: bulut saate bağlı).
Sınır: kıyı çizgisi hâlâ sert, bulut yansıması ve bulut gölgesi yok, orta mesafede döşeme tekrarı hafifçe seçiliyor. Tek adaptör; gerçek ekranda FPS ölçülmedi.

## D53. Bulut gölgesi, temas gölgesi, taş kenarlı göl, ucuz gönderim
D52'nin açık bıraktığı kusurlar kapatıldı.
- **Bulut:** `clouds` dördüncü doku yuvası (akış sürümü 8, `WGSL_MAX_TEX` 4). Bulut güneşi keser (256 m'lik doku, gökle aynı rüzgâr), suya ve pürüzü 0,25'in altındaki yüzeye yansır. Yer açmak için `screen_ix/iy` çıktı: su onları metal / pürüz yuvasında okur.
- **Zemin alanı:** D49'un alan yuvası zemine de bağlandı. B büyük ölçekli leke (64 m, döşeme tekrarını kırar). G temas gölgesi: çizilen nesnelerin AABB'si zemine iz bırakır, kaynak fizik değil çizim olduğu için her sahneye bedava gelir; iz değişmediyse doku yazılmaz. Alan tekrar ettiği için G yalnız ±31 m içinde okunur (yoksa kenar taşının izi 64 m'de bir hayalet çizgi veriyordu). Dik ve alt yüzeyler yere yakın kararır (okuma yok).
- **Göl:** dalga alanının yansıyan kenarında taş kenar (`mesh_box`, metre ölçekli doku); kenar boyunca ve dalga tepesinde köpük, ek doku okuması yok.
- **`quad0(x)`:** Matoki yerleşiği, quad'ın ilk lane'indeki değer. Çıktısı quad-sabit olduğu için derleyici okumayı skalerleştirir: 16 yerine 4 yükleme. Yalnız gerçekten yumuşak veride: bulut gölgesi ve zemin alanı. Gökte ve yansımada denendi, bulut kenarı blok blok oldu (fark 70/255, `gpu_check` %1): orada piksel başına. WGSL kaba türevle aynı pikseli hesaplar, görüntü iki yolda aynı.
- **Gönderim** (`gpu.mjs`): aynı boru hattı, doku grubu ve köşe tamponu yeniden kurulmaz. **Izgara:** GPU kipinde sabit küpler z dilimi × malzeme birleşik ağ, yakından uzağa; yazılım yolu değişmedi.
- **Denenip atılan, kazancı ölçülmeyen:** gölge yoklamasını quad başına indirmek (×1,06 kayıp), küpleri tek tek önden arkaya sıralamak (kazanç yok), tek dev birleşik ağ (görünüm 2 GPU geçişi 16 → 24 ms).

Kare süresi, 1920×1080, A/B D52'ye karşı:

| 10 görünüm toplamı | D52 | D53 | Oran |
|---|---|---|---|
| Yazılım, gölge + MSAA | 2307 ms | 2380 ms | 1,03 |
| Yazılım, düz | 1195 ms | 1329 ms | 1,11 |
| GPU geçişleri, gölge + MSAA | 134 ms | 145 ms | 1,08 |
| GPU geçişleri, düz | 88 ms | 98 ms | 1,11 |

Yazılım ölçümünde makine yüklüydü (mutlak süreler normalin yaklaşık 4 katı); oran önceki koşularda ×1,01–1,06 / ×1,08–1,09 çıktı. GPU satırı iki ölçümün birleşimi: görünüm 1–3 dilimli ızgaradan sonra, 4–10 ondan önce (bu görünümlerin yolu dilimlemeden etkilenmez).

| Küp görünümleri, her kare iş bitimi beklenerek kare süresi (D52 → D53) | Görünüm 1 | Görünüm 2 | Görünüm 3 |
|---|---|---|---|
| gölge + MSAA | 42 → 29 ms | 43 → 28 ms | 41 → 25 ms |
| düz | 22 → 15 ms | 20 → 19 ms | 24 → 15 ms |
| Ana iş parçacığı (motor + gönderim, gölge + MSAA) | 9,3 → 1,5 ms | 8,5 → 1,6 ms | 9,2 → 1,5 ms |

GPU varsayılan ayarda geçiş süresi 16,6 ms üstü: görünüm 1 (18,0), 2 (17,5), 6 (17,1). Bekleme ölçümü tarayıcının bildirim gecikmesini de içerir, FPS değildir.

Kabul: `gpu_check` 66/66 (en kötü %0,029), 270/270 test (iki kipte), `check_demo` geçti (temas gölgesi dolu, duran sahnede yazılmıyor, hareket edende yazılıyor). Bozma: dördüncü doku yuvası boş bağlanınca `gpu_check` 20 satırda düşüyor.
Sınır: gerçek ekranda FPS hâlâ ölçülmedi (uygulama penceresi odakta değilken `requestAnimationFrame` saniyede bir çalışıyor). Tek adaptör. Temas gölgesi kutu izi: eğik ve yuvarlak nesnede yaklaşık. Yansıma düzlemi düz.

## D54. D53'ün yazılım maliyeti geri alındı: MSAA yolu ve anizotropi
Önce ölçüm aracı düzeltildi: `node --no-liftoff --no-wasm-tier-up` ile V8'in çalışırken yaptığı derleme gürültüsü kalkar; motor kendisiyle kıyaslanınca toplam fark %1–2 (görünüm başına ±%5). Bu bayraklar olmadan aynı kod ±%30 oynuyordu ve ilk görünüm 5 kat yavaş görünebiliyordu.

Görünüm 8, tek thread, kare başına profil:

| Kalem | MSAA'sız | MSAA, önce | MSAA, sonra |
|---|---|---|---|
| Gölgeleme (JIT) | 153 ms | 160 ms | 125 ms |
| Üçgen işleme | 14 ms | 50 ms | 39 ms |
| Çözümleme | 0 | 11,5 ms | 3,9 ms |

- **Çözümleme:** dört örnek eşitse ortalama alınmaz: (4a + 2) >> 2 = a. **Üçgen işleme:** en kötü köşesi içeride olan quad kenar testine girmez, tam kapsanan piksel eski değeri okumadan yazılır. İkisinden sonra `hashes.mjs` 11/11 aynı.
- **Anizotropi:** shader parçaları tek tek kapatıldı: süzme gölgelemenin yarısı (131,9 → 65,1 ms kapalıyken), seviye karışımı tek başına %20; bulut %7, zemin alanı %5, sis ve ton %1'in altında. En çok 8 örnek → 4, karışım bandı %60 → %40. Bedel: uzak zeminde hafif yumuşama, 1080p yakın kırpıntıda taş deseni okunuyor. Görüntü bu adımda bilerek değişti.
- **Temas gölgesi:** küre kutu izi yerine yuvarlak iz bırakır.
- **Denenip atılan:** arkası dönük yüzeyde ve uzak kademe dışında gölgeyi atlamak: yazılımda gölgenin kendi payı yalnız ~3 ms, ölçülebilir kazanç yok (×1,005).

| A/B, 1920×1080, 10 görünüm toplamı | Önce | Sonra | Oran |
|---|---|---|---|
| Yazılım, gölge + MSAA, D53'e karşı (tek thread) | 2441 ms | 2211 ms | 0,906 |
| Yazılım, düz, D53'e karşı (tek thread) | 1542 ms | 1418 ms | 0,920 |
| Yazılım, gölge + MSAA, D52'ye karşı (tüm thread) | 554 ms | 521 ms | 0,940 |
| Yazılım, düz, D52'ye karşı (tüm thread) | 323 ms | 325 ms | 1,006 |
| GPU geçişleri, gölge + MSAA, D53'e karşı | 158 ms | 146 ms | 0,92 |
| GPU geçişleri, düz, D53'e karşı | 100 ms | 95 ms | 0,95 |

Kazanç eşit dağılmadı: görünüm 3 tek thread'de ×1,03, görünüm 6 D52'ye karşı ×1,06, görünüm 2 düz kipte D52'ye karşı ×1,15. GPU varsayılan ayarda geçiş süresi 16,6 ms üstü: görünüm 2 (21,6), 10 (17,2); görünüm 6 sınırda (16,6). Bu ölçüm öncekilerden sıcak bir koşuda; D53'te aynı görünümler 17,5 / 14,7 / 17,1 ms okunmuştu, görünüm başına tek değere güvenilmez, oran aynı koşudan.

Kabul: `gpu_check` 66/66 (en kötü %0,029), 270/270 test, `check_demo` geçti.
Sınır: gerçek ekranda FPS hâlâ ölçülmedi. GPU'da MSAA ve gölge kopyası (zaman damgası dışında) açık kalem; tek adaptör. Yazılımda üçgen işleme MSAA'sız yolun hâlâ yaklaşık 3 katı.

## D55. Keskin kenar: Lanczos3 mip ve kutu pahı
- **Mip zinciri Lanczos3 ile** (`img_build_mips_sharp`, malzeme dokuları): kutu ortalaması her seviyede bulanıklık biriktiriyordu. 6 dokunuşlu ayrılabilir çekirdek, doğrusal ışıkta, kenar sarmalı. Sınamada 16 piksel periyotlu çizginin genliği seviye 2'de kutuyla 147, Lanczos3 ile 165. Kare maliyeti yok, başlangıç ~10 ms. D54'teki 4x anizotropinin uzak zemindeki yumuşamasını da geri aldı. `tex_build_mips` (kutu) testler ve glTF için kalır.
- **Kutu pahı:** yüzü dokuya tam oturan kutularda (ızgara, tuğla, sütun, araba) uv kenarından çeyrek daire yükseklik, kabartıya eklenir: kenar yuvarlanır, ışık yakalar. Bayrak `use_mip`'in 2. biti (`LIT_BEVEL`); 1. bit mip (`LIT_MIP`). Küre ve kapsül ayrı malzeme (`m_ball`), pahsız: orada doku kenarı yüz kenarı değil.
- **Denenip atılan:** pahı doku alfasına yazmak. Alfa okununca anizotropik süzmenin her dokunuşunda bir kanal daha çözülüyor: gölgeleme 117 → 170 ms (×1,45). Analitik hesap 114,9 ms.

| A/B, 1920×1080, tek thread, D54'e karşı (10 görünüm) | Önce | Sonra | Oran |
|---|---|---|---|
| gölge + MSAA | 2077 ms | 2111 ms | 1,017 |
| düz | 1300 ms | 1302 ms | 1,002 |

Görünüm 2 (ekranı dolduran pahlı küpler) ×1,05–1,10; toplam ölçüm gürültüsü ±%2.
Kabul: `gpu_check` 66/66 (en kötü %0,027), 271/271 test, `check_demo` geçti.
Sınır: pah analitik olduğu için uzakta da aynı genişlikte; küçük kutularda kenar ışığı ince bir çizgi olarak kalır, parlaklık yumuşatması (D51) kırpışmayı bastırır.

## D56. Uniform'lar adla: düzen tablosu, kare / malzeme sırası, 512 bayt
Sorun: tüm frag shader'ları tek 256 baytlık `LitUniforms` yapısını, onun bildirim sırasıyla birebir yazmak zorundaydı. `lit_hq` ~30 uniform'u doğru sırada bildiriyordu, kullanmadıklarını da yer tutucu olarak. Yapı dolmuştu ve her yeni alan bir takma ad gerektiriyordu: su `metal` / `rough` yuvasında ekran boyunu, `wrap` yuvasında zamanı, `field` yuvasında `wave`'i okuyordu.
- **Düzen tablosu** (`MatokiOptions.layout`, `gfx3d/lit_layout.c`): uniform ofseti adla tablodan gelir, bildirim sırası serbest, shader yalnız kullandığını bildirir. Tabloda olmayan ad ya da yanlış tip satır:sütunlu derleme hatası (canlı editör de aynı yolu kullanır). Tablo `offsetof` ile yapının kendisinden üretilir: düzen tek yerde. Tablo verilmezse eski davranış (sırayla), küçük test shader'ları ve köşe shader'ları için.
- **`LitUniforms` sırası:** önce kare alanları (göz, güneş, zaman, ekran, gölge, bulut), sonra malzeme. `time`, `screen_ix/iy` gerçek alan. Takma adların hepsi kalktı (`cloud_time` → `time`, `wave` → `field`).
- **Yer:** parça uniform'u 256 → 512 bayt (iki `GCMD_SLOT`, WGSL `array<vec4u, 32>`), köşe uniform'u 256. Akış sürümü 9.
- **Kare / malzeme ayrı bloklar ileride:** C shader'ları ve JIT tek uniform işaretçisi alıyor; ayrı blok bu ABI'yi değiştirir. Bu adımda sıralama ve tablo yeterli; motor kare alanlarını her malzemeye yazmayı sürdürür.

Kabul: kare hash'leri önce / sonra 11/11 aynı (görüntü bit bit aynı), `gpu_check` 66/66 (en kötü %0,027), 272/272 test (iki kipte; yeni: adla düzen, bilinmeyen ad, yanlış tip, 512 bayt sınırı), `check_demo` geçti.
Maliyet: GPU geçiş süresi aynı (görünüm 1, 4, 6, 8, 9, 10: 15,2 / 11,9 / 20,1 / 13,1 / 12,8 / 17,5 → aynı ±0,1 ms), ana iş parçacığı aynı. Yazılım, 5 görünüm, tek thread: gölge + MSAA ×1,008, düz ×1,03 (görünüm başına gürültü ±%5).
