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
## D7. Mip zinciri ve seviye seçimi
M8 §2.1 / §2.3'e birebir uyulur. Kutu filtresi lineer uzayda (sRGB-doğru indirgeme sonraki adım). Seviye seçimi
tamsayı: `e = exponent(rho2) - 127`, `level = e >> 1`, `rho2 < 1` / NaN / denormal → 0. `lod_bias = 0`.
