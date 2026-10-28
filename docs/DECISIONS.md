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

## D7. Mip zinciri ve seviye seçimi
M8 §2.1 / §2.3'e birebir uyulur. Kutu filtresi lineer uzayda (sRGB-doğru indirgeme sonraki adım). Seviye seçimi
tamsayı: `e = exponent(rho2) - 127`, `level = e >> 1`, `rho2 < 1` / NaN / denormal → 0. `lod_bias = 0`.
