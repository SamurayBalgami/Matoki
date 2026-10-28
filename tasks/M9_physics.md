DURUM: IN_PROGRESS (Faz 1 ✅, Faz 2 ✅ (SIMD çözücü ertelendi), Faz 3a ✅ raycast/araç/karakter, Faz 3b ✅ eklemler/kapsül/hull/mesh+BVH, Faz 4 ✅ CCD/mesh iç kenarları/hull eylemsizliği, Faz 5 ✅ XPBD kumaş + jöle, Faz 6 ✅ grafik renklendirme + SIMD temas çözücüsü, Faz 7 ✅ dünya BVH'si (geniş faz / raycast / CCD / yumuşak aday), Faz 8 ✅ kumaş öz-çarpışması + rijit-kumaş teması (mermi yakalama); kalan: SIMD dar faz, kinematik gövdeler)
HAT: P (fizik). Bağımlılık: M8 Faz 1 (boru hattı, demo). M8 Faz 6 (şehir) bu görevin Faz 3'ünü kullanır.

# M9 — MATOKI fizik motoru: rijit cisimler, çarpışma, kısıt çözücü, araç ve karakter

> Dil: C11, freestanding, libc yok, malloc yok (arena). Fortran / dış kütüphane yok (karar: docs/DECISIONS.md D14).
> Önce `AGENTS.md`, `docs/DECISIONS.md` ve bu dosyanın tamamını oku.

## 0. Hedef
Tarayıcıda, tek thread'de, 60 Hz sabit adımla, **belirleyici** (aynı girdi → bit-bit aynı durum) bir rijit cisim
fizik motoru. Kullanım yerleri: demo sahneleri, M8 şehri (araçlar, karakter, düşen nesneler).

## 1. Kurallar
- `src/phys/**` bu hattın dosyalarıdır. Raster/Matoki dosyalarına dokunma; fizik yalnızca demo üzerinden çizilir.
- Belirleyicilik: sabit `dt`, sabit iterasyon sayısı, gövde ve çift sıraları her adımda aynı. Hash
  tabloları yalnızca arama için kullanılır, yineleme sırası dizilerden gelir.
- Her optimizasyon ölçümle gelir (`tools/bench.mjs` benzeri bir fizik ölçümü veya demo istatistiği).
- SIMD sürümleri skaler sürümle bit-bit eşleşmeli (D9, D11 ilkesi).

## 2. Fazlar

### Faz 1 — Çekirdek dinamik + temel çarpışmalar + çözücü
- Matematik: `Quat`, rotasyon matrisi, dünya uzayında ters eylemsizlik (`R D R^T`).
- Şekiller: küre, kutu (yarı boyutlar), sonsuz düzlem (statik).
- Gövde: kütle 0 = statik; yarı-örtük Euler; quaternion integrasyonu + normalize.
- Geniş faz: x ekseninde sweep-and-prune (zamansal tutarlılık için ekleme sıralaması); düzlemler ayrı.
- Dar faz: küre-küre, küre-düzlem, küre-kutu, kutu-düzlem, **kutu-kutu SAT** (15 eksen, yüz kırpma
  + kenar-kenar), manifold en çok 4 nokta, spekülatif temas payı.
- Çözücü: sequential impulse, normal + 2 eksen Coulomb sürtünme (kutu kenetleme), Baumgarte + slop,
  restitüsyon eşiği, önceki adımdan **warm start** (temas eşleme: A'nın yerel uzayında yakınlık).
- Demo: sahne 3 "fizik" — zemin, kutu piramidi, küreler; `F` kameradan küre fırlatır, `T` sıfırlar.
- **Testler** (`tests/test_phys*.c`): serbest düşüş (yarı-örtük Euler kapalı formu), izotropik dönen
  küpte açısal momentum korunumu, elastik kafa kafaya çarpışma (hızlar yer değiştirir, momentum korunur),
  düzlemde duran kutu (yükseklik, hız, dönme), 5 kutuluk yığın kararlılığı, eğik düzlemde sürtünme
  (μ > tanθ duruyor; μ < tanθ ivmesi ≈ g(sinθ − μ cosθ)), kutu-kutu SAT birim vakaları, belirleyicilik
  (iki koşunun durum hash'i aynı).
- **Kabul:** demoda 5 katlı piramit kendiliğinden yıkılmıyor; fırlatılan küreler piramidi yıkıyor;
  adım süresi istatistik panelinde.

### Faz 2 — Kararlılık ve ölçek
- Uyku (adalar + eşik), ada tabanlı çözme; 1000+ gövde.
- Soft constraint (Box2D v3 "soft step") veya XPBD karşılaştırması: aynı sahnelerde kararlılık/enerji ölçümü,
  seçimi DECISIONS.md'ye yaz.
- SIMD: 4 temas noktasını birlikte çözme (wide solver) — skaler ile bit-bit aynı olacak şekilde.
- Benchmark: 100 / 500 / 2000 kutu, adım ms, temas sayısı.

### Faz 3 — Eklemler ve oyun fiziği
- Eklemler: top-mafsal, menteşe (limitli), mesafe, sabit.
- Işın/şekil sorguları (raycast, sweep), tetik hacimleri.
- Araç: raycast tekerlek + yay/sönüm süspansiyonu, motor torku, Pacejka benzeri basit lastik modeli.
- Karakter kontrolcüsü (kapsül, kinematik, basamak çıkma, eğim sınırı).
- Şekil: kapsül, dışbükey gövde (GJK + EPA), statik üçgen mesh (BVH) — şehir geometrisi için.

### Faz 4 (esnek) — İleri
- CCD (hızlı küreler için zaman-of-impact), yumuşak cisim/kumaş (XPBD), basit akışkan parçacıkları (SPH).

## 3. Notlar şablonu
```
## Notlar
### Faz raporları (yapılanlar, kararlar, ölçümler, bilinen sorunlar)
### Benchmark tabloları
### Ertelenenler
```

---

## Notlar

### Faz 1 raporu (2026-10-27) — tamamlandı
Dosyalar: `src/phys/pmath.h` (quaternion, rotasyon matrisi, `R D Rᵀ`, ortonormal taban), `phys.h` (API),
`collide.c` (dar faz), `world.c` (dünya, SAP, çözücü), `tests/test_phys.c`, demo sahne 3 (`src/gfx3d/demo.c`),
`mesh_sphere` (`src/gfx3d/mesh.c`).

Yapılanlar:
- Küre / kutu / sonsuz düzlem; yarı-örtük Euler, dünya uzayında açısal hızla quaternion integrasyonu.
- Geniş faz: x ekseninde sweep-and-prune (ekleme sıralaması, eşitlikte indeks → belirleyici); düzlemler ayrı.
- Dar faz: küre-küre, küre-düzlem, küre-kutu (içeride/dışarıda), kutu-düzlem, kutu-kutu SAT (6 yüz + 9 kenar
  ekseni; yüz → Sutherland–Hodgman kırpma, kenar → en yakın noktalar; yüz ekseni ve A referansı histerezisle
  tercih edilir), ≤ 4 noktaya indirgeme, 0,02 m spekülatif pay.
- Çözücü: sequential impulse, Coulomb diski sürtünme, Baumgarte (β = 0,2, slop 0,005), restitüsyon eşiği 1 m/s,
  warm start (önceki adım; A'nın yerel uzayında 5 cm içinde eşleme).
- Demo: `3` fizik sahnesi (6 katlı piramit, 2 kule, 5 küre), `F` kameradan küre fırlatır, `T` sıfırlar; sabit
  60 Hz adım, karede en çok 4 adım; panelde gövde/temas/adım ms.

Hata ayıklama kaydı (testler yakaladı):
1. **Restitüsyon enerji pompası:** ön hesap aynı döngüde hem yaklaşma hızını ölçüyor hem warm start
   uyguluyordu. Yığın tabanının büyük warm start itkisi sonraki temaslarda sahte `vn < −1 m/s` gösterdi, restitüsyon
   olmayan bir sıçrama ekledi ve 5'li yığın dağıldı (tepe KE 76 J). Çözüm: iki geçiş, önce tüm bias'lar, sonra
   warm start (Box2D'deki gibi). DECISIONS D15.
2. **Referans yüz gidip gelmesi:** eşit ayrılmada A/B referansı karelere göre değişiyordu. Histerezis eklendi.
3. Test hatası: eğik düzlem testinde oturma adımlarındaki başlangıç hızı beklentiye katılmamıştı.

Bilinen sınırlama: **sürtünmesiz** uzun yığınlar yavaşça kayıyor. İlk, henüz yakınsamamış adım ~0,0005 rad'lık
kalıcı eğim bırakıyor ve sürtünme olmadığı için eğik normaller yanal kuvvet üretiyor. Sürtünmeli yığınlar sağlam
(aşağıda). Faz 2'de soft step / relax iterasyonlarıyla ele alınacak.

Testler (`python build.py test`: 37/37, fizikte 10 test):
| Test | Sonuç |
|---|---|
| Serbest düşüş ↔ yarı-örtük Euler kapalı formu (120 adım) | y 1e-3, vy 1e-4 içinde |
| Dönen küp (torksuz, 600 adım) | ω sabit (1e-6), KE 1e-4 bağıl, |q| = 1 |
| Elastik çarpışma: 1:1, 1:3, 5:1 kütle | hızlar analitik değerin %5'i içinde, momentum 1e-4 |
| SAT: hizalı yüz (4 nokta, d = 0,1), ayrık, 45° dönmüş üst kutu (4 nokta), kenar-kenar (1 nokta, d = 0,0142), ters sıra | doğru |
| Küre-kutu, küre-düzlem, kutu-düzlem | normal yönleri ve derinlikler doğru |
| Düzlemde duran kutu (300 adım) | y = 0,5 ± 0,01, |v|, |ω| < 1e-2, warm start eşleşiyor |
| 5'li yığın (600 adım) | en büyük yatay kayma 1,8 mm, KE ≈ 0 |
| 15 kutuluk piramit (600 adım) | en büyük yer değiştirme 1,3 mm |
| Eğik düzlem 20°: μ = 0,5 | kayma 0 (durur) |
| Eğik düzlem 20°: μ = 0,2, 2 s | 3,399 m; analitik (v₀ dahil) 3,330 m, %2,1 fark (yarı-örtük Euler ayrıklaştırması) |
| Belirleyicilik: piramit + 6 hızlı küre, 300 adım, iki koşu | durum hash'i aynı |

Ölçümler (i7-12650H, tek thread, wasm):
| Sahne | Gövde | Temas | Adım ms |
|---|---|---|---|
| Test piramidi (15 kutu, Node) | 16 | 100 | 0,080 (600 adım ortalaması) |
| Demo sahne 3, oturmuş (tarayıcı) | 39 | 241 | 0,42 |
| Demo sahne 3, piramit yıkıldıktan sonra (tarayıcı) | 40 | 148 | 0,13 |
`engine.wasm` 105.874 → 130.705 bayt.

Sonraki adımlar (Faz 2): uyku ve adalar, soft step / XPBD karşılaştırması, SIMD (4 temasın birlikte çözülmesi),
100 / 500 / 2000 kutuluk benchmark.

### Faz 2 raporu (2026-10-27) — tamamlandı (SIMD çözücü hariç)
Yapılanlar:
- **Ölçüm altyapısı:** `src/phys/phys_bench.c` (wasm export'ları) + `tools/bench_phys.mjs`. Sahneler: 0 düşen yığın,
  1 3B basamaklı piramit, 2 tek kalınlıklı 2B piramit duvar. 600 adım; aktif/oturmuş ms, çarpışma/çözücü
  ayrımı, temas, uyanık gövde, KE, en büyük yer değiştirme ve girişim, durum hash'i.
- **Soft step çözücü** (`PHYS_SOLVER_SOFT`, Box2D v3 tarzı): adım başına 4 alt adım; her alt adımda hız entegrasyonu
  → warm start → yumuşak kısıtlı çözüm (30 Hz, sönüm oranı 10, en çok 3 m/s itme) → konum entegrasyonu → relax
  (bias'sız çözüm). Sonda restitüsyon geçişi. Güncel ayrılma, gövdelerin yerel uzayındaki çapalardan hesaplanır.
  SI çözücü seçenek olarak duruyor (`phys_world_set_solver`). **Varsayılan artık soft** (D16).
- **Adalar ve uyku:** temaslı uyanık gövdeler birleşim-bul ile adalara ayrılır (statik gövdeler bağlamaz). Adadaki
  her gövde 0,5 s boyunca |v| < 0,05 m/s ve |ω| < 0,05 rad/s ise ada birlikte uyur. Uyanık bir gövdeye değen
  uyuyan ada bütünüyle uyanır (geniş fazda 3 aşama: adaylar → uyandırma → dar faz). Uyuyan-uyuyan ve uyuyan-statik
  çiftler aday listesine bile girmez. `allow_sleep` bayrağı; demo açık.
- Demo: sahne 3 soft + uyku; panelde uyanık gövde sayısı.

Testler (`python build.py test`: 38/38). Fizik sistem testleri iki çözücüyle koşuyor (`FOR_SOLVERS`):
| Test | SI | soft |
|---|---|---|
| Serbest düşüş (soft: alt adımlı kapalı form) | ✅ | ✅ |
| Elastik çarpışmalar (3 kütle oranı) | ✅ | ✅ |
| 5'li yığın: en büyük yatay kayma | 1,83 mm | **0,30 mm** |
| 15'li piramit: en büyük yer değiştirme | 1,30 mm | 2,50 mm |
| Eğik düzlem μ = 0,5 / μ = 0,2 (analitik 3,330 m) | 0 / 3,399 m | 0,1 mm / 3,381 m |
| Belirleyicilik (uyku açık ve kapalı) | ✅ | ✅ |
| Uyku: piramit 30. adımda uyur, 60 adım boyunca konum değişmez, küre çarpınca 16/16 uyanır ve dağılır | ✅ | ✅ |

Ölçüm (i7-12650H, Node 24.22.1, tek thread, 600 adım; ms = adım başına ortalama):
| Sahne | n | Çözücü | Uyku | ms aktif | ms oturmuş | KE son (J) | en büyük yer değ. (m) |
|---|---|---|---|---|---|---|---|
| yığın | 2000 | SI | kapalı | 8,21 | 9,28 | 249,95 | 22,1 |
| yığın | 2000 | soft | kapalı | 8,01 | 9,22 | **0,63** | 22,7 |
| piramit | 1785 | SI | kapalı | 30,24 | 30,24 | 0 | 0,009 |
| piramit | 1785 | soft | kapalı | 29,26 | 29,41 | 0 | 0,009 |
| piramit | 1785 | soft | **açık** | **3,89** | **0,29** | 0 | 0,008 |
| piramit | 385 | soft | açık | 0,62 | 0,03 | 0 | 0,003 |
| duvar | 496 | SI | kapalı | 4,34 | 4,44 | **3068,99 (çöktü)** | 12,3 |
| duvar | 496 | soft | kapalı | 3,85 | 3,82 | 0,06 | **0,16** |
| duvar | 496 | soft | açık | 0,81 | 0,02 | 0 | 0,07 |
| duvar | 1953 | SI / soft | — | — | — | çöker | — |

Bulgular:
- Soft step, SI'nın çöktüğü 31 katlı duvarı ayakta tutuyor ve 2000 kutuluk yığını 10 s'de oturtuyor; maliyet eşit
  veya %5–10 daha düşük. Varsayılan olarak seçildi (D16).
- Uyku, oturmuş sahnelerde adım süresini 30–110 kat düşürüyor (1785 kutu: 29,4 → 0,29 ms). Kalan maliyet SAP
  sıralaması ve taraması.
- **Rastgele düşen yığınlar 10 s'de tamamen uyumuyor:** yığın tek ada; tek bir oynak kutu bütün adayı uyanık
  tutuyor. Box2D'de de aynı davranış; daha agresif eşik ya da ada bölme Faz 3'te değerlendirilecek.
- **62 katlı tek kalınlıklı duvar** iki çözücüde de çöküyor (16 alt adım / 40 iterasyonla bile). 62 m yükseklik ve
  1 m kalınlıkla devrilme açısı sınırı ≈ 0,9°; fiziksel olarak da sınırda. 3B piramitler 1785 kutuya kadar sağlam.
- Uyanıkken süreyi çözücü yiyor (%80–90). **SIMD çözücü ertelendi:** 4 temasın paralel çözümü Gauss-Seidel
  sırasını değiştirir. Skalerle bit-bit eşitlik için Box2D v3'teki gibi kısıt grafiği renklendirmesi gerekiyor;
  bu ayrı ve büyük bir iş (Faz 2b).

`engine.wasm` 134.156 → 143.204 bayt.

### Faz 3a raporu (2026-10-27) — raycast, araç, karakter
Dosyalar: `src/phys/query.c` (raycast, dış itki/kuvvet, uyandırma), `vehicle.h/.c`, `character.h/.c`,
`tests/test_phys_game.c`, demo sahne 4, `mesh_cylinder`, `m_atan2f` (`core/math.c`).

- **Raycast:** düzlem, küre, döndürülmüş kutu (yerel uzayda slab); AABB ön eleme; en yakın isabet, normal,
  hariç tutulan gövde, içeriden başlama (t = 0). Doğrusal tarama (BVH sonraki adım).
- **Kuvvet biriktiricisi:** `phys_apply_force_at` — kuvvet yerçekimiyle aynı zamanlamada (soft step'te her alt
  adımda) entegre edilir, adım sonunda sıfırlanır. `BODY_NO_SLEEP` bayrağı (oyuncu aracı uyumaz).
- **Araç:** kutu şasi + 4 raycast tekerlek (önler direksiyonlu, arkalar çekişli ve el frenli). Süspansiyon:
  yay + sıkışma hızıyla sönüm, kuvvet olarak uygulanır. Lastik: 4 turlu küçük yinelemeli çözücü, her turda sıra
  tersine döner; sabit motor itkisi + birikimli fren kısıtı + yanal kaymayı sıfırlayan birikimli itki,
  |(L, S)| <= μ·Fn·dt sürtünme çemberinde. Yanal itki kütle merkezi yüksekliğine doğru taşınır (roll_reduction =
  0,6, devrilmeyi azaltır). El freni arka tutuşu %40'a düşürür (kaydırma).
- **Karakter:** kinematik kapsül (3 küre). Yürünebilir yüzeyde yalnızca dikey itme (eğimden kaymaz), dik yüzeyde
  normal boyunca itme + hızın yüzeye giren bileşenini silme. Basamak: önü kapanınca ileride aşağı ışınla üst yüzü
  yokla, en çok 0,4 m ise oraya çık. Zemine yapışma yalnızca çözme destek bulamadıysa. Zıplama, hava kontrolü,
  dinamik kutuları itme.
- **Demo sahne 4:** rampa, merdiven, kutu piramidi, koni sırası, sütunlar. `V` sürüş → yürüyüş → uçuş; takip
  kamerası (fareyle döndürülür, yavaşça arkaya döner). `T` arabayı düzeltir, `F` küre fırlatır. Tarayıcıda
  sürüldü: 25 km/s, 4 tekerlek yerde, konileri deviriyor.

Hata ayıklama (testler yakaladı):
1. Süspansiyon itki olarak tek seferde uygulanınca alt adımlı yerçekimiyle testere dişi hız (−0,06 m/s) oluştu →
   kuvvet biriktiricisi eklendi.
2. Tekerlekler tek sırayla işlenince Gauss-Seidel asimetrisi 33 m'de 0,51 m sapma yaptı → tur başına ters sıralı
   yinelemeli lastik çözücü (sapma 0,6 mm).
3. Basamak: "yukarı-ileri-aşağı" çarpışma denemesi yarıçap > ilerleme olduğunda kenardan geri itiyordu → ışınla
   yoklama. Ardından merkez ışınlı zemin yapışması karakteri basamaktan geri çekiyordu → çözme destek bulduysa
   yapışma yok.

Testler (`python build.py test`: 51/51; yeni 13 test):
| Test | Sonuç |
|---|---|
| Raycast: düzlem/küre/x'te dönmüş kutu, hariç tutma, menzil, ıska, içeriden | t ve normal 1e-4 içinde |
| Araç oturma yüksekliği (analitik: sıkışma = m·g / 4k) | 1,0519 m = 1,0519 m; hız ≈ 0, eğim yok |
| Düz hızlanma 3 s | 14,2 m/s, yanal sapma 0,6 mm |
| Sola dönüş (direksiyon +1) | ileri x = −0,80 (sola), devrilmedi |
| Fren 15 m/s'den (analitik v²/2a = 8,84 m) | 8,45 m |
| Araç belirleyiciliği (kutulara çarpan sürüş) | hash aynı |
| Karakter iniş + 2 s yürüme | y = 0 ± 0,01; 10,000 m |
| Duvar + duvar boyunca kayma | z ≥ −2,65; x −3,5 m kaydı |
| Basamak 0,3 m / duvar 0,8 m | çıktı (y = 0,302) / takıldı |
| Eğim 30° / 60° | 0 m kayma / 14,4 m kayma |
| Zıplama (analitik v²/2g = 1,54 m) | 1,498 m |
| Uyuyan kutuyu itme | uyanır, itilir |
| atan2 doğruluğu | en büyük hata 1,9e-6 rad |

Bilinen sınırlamalar: karakter fizik dünyasında gövde değil (araç karakterin içinden geçer); raycast doğrusal
tarama; araç modeli arcade-gerçekçi (Pacejka eğrisi yok, kayma oranı yok, motor/vites modeli yok).
Sonraki adımlar (Faz 3b): eklemler (top-mafsal, menteşe, mesafe), kapsül ve dışbükey şekiller (GJK/EPA), statik
üçgen mesh + BVH (şehir geometrisi için), raycast için BVH.
`engine.wasm` 143.204 → 166.560 bayt.

### Faz 3b raporu (2026-10-27) — eklemler, kapsül / dışbükey şekiller, statik mesh + BVH
Dosyalar: `src/phys/gjk.h/.c` (çekirdek + yarıçap, GJK, EPA), `shapes.h/.c` (ConvexHull, TriMesh + BVH),
`joints.h/.c`, `world.c` / `collide.c` / `query.c` eklemeleri, `tests/test_phys_shapes.c`, demo sahne 5.

- **Şekiller:** küre, kapsül, kutu, hull, mesh, düzlem. Her dışbükey şekil "çekirdek + yarıçap" (nokta /
  doğru parçası / çokyüzlü). Çekirdekler ayrıksa GJK en yakın noktaları verir (yarıçaplar çıkarılır);
  kesişiyorsa EPA derinlik + normal. Çokyüzlü–çokyüzlü manifold: referans / olay yüzü kırpması; doğru parçası –
  çokyüzlü ve doğru parçası – doğru parçası 2 noktalı temas (kapsül yatar, yuvarlanmaz).
- **Hull:** kaba kuvvetle düzlem bulma, köşe bit maskesiyle düzlem tekilleştirme, yüz çokgeni 2B monoton zincir,
  hacim merkezi. Euler V − E + F = 2 testte doğrulanıyor.
- **Mesh + BVH:** orta nokta bölmesi, yaprakta ≤ 4 üçgen; AABB sorgusu ve Möller–Trumbore raycast. Her üçgen ayrı
  manifold (`Manifold.sub`), çift başına en çok 16. Sıcak başlatma (a, b, sub) anahtarıyla.
- **Eklemler:** top-mafsal, menteşe (eksen kilidi + açı sınırı + motor), mesafe (ip: yalnız germe / çubuk).
  Temaslardan önce çözülür; soft step'te 60 Hz / ζ = 2 yumuşak kısıt, SI'da Baumgarte. Eklemler ada ve uyandırma
  grafiğinde; dünya gövdesine (`PHYS_NO_BODY`) bağlanabilir; bağlı çiftin çarpışması kapatılabilir.
- **Demo sahne 5:** tümsekli mesh arazi, 8 kapsül halkalı top-mafsal zinciri, 10 tahtalı menteşeli asma köprü,
  motorlu menteşeli yel değirmeni, açı sınırlı menteşeli tabela, 30 düşen kapsül / kaya (hull) / kutu. Tarayıcıda doğrulandı: 55 gövde, 109 temas, adım 1,05 ms (4 alt adım), hata yok.

Hata ayıklama (testler yakaladı):
1. Hull Euler testi düştü: eş düzlemli üçlüler aynı düzlemi birden çok kez üretiyordu → köşe maskesiyle
   tekilleştirme + 2B dışbükey zarfla yüz sırası.
2. GJK–SAT normal uyuşmazlığı: yüz kırpması EPA normalinden daha kötü bir yüzü seçebiliyordu → yüz normali
   yalnızca bu eksendeki girişim ≤ derinlik + 0,005 + %2 ise kabul (aksi halde tek nokta, EPA normali).
3. GJK döngüye girip yanlış "kesişiyor" diyordu → en iyi |v| izlenir, ilerleme yoksa çıkış (kod 4); yassı
   dörtyüzlüde (hacim² ≤ 1e-10·ölçek³) içerme kararı yerine 4 yüz denenir.
4. Test arenası taştı (6000 dünya) → tek geçici dünya yeniden kullanılıyor.

Testler (`python build.py test`: 61/61; yeni 10 test):
| Test | Sonuç |
|---|---|
| Hull: kutu / rastgele bulut, Euler, hacim merkezi | V − E + F = 2, merkez 1e-5 içinde |
| EPA derinliği vs SAT en küçük ayırma (2716 çift) | en büyük fark 2,3e-5 m |
| GJK manifold normali vs SAT (sığ temas) | hiçbir normal MTD'den belirgin kötü değil |
| Kapsül: düzlem / kutu / kapsül, yatma | 2 temas noktası, derinlik doğru |
| BVH sorgusu = kaba kuvvet; mesh raycast | birebir; hata 3e-6 |
| Mesh üzerinde kutu / küre / kapsül oturma | girişim < 1 cm, uyuyor |
| Sarkaç periyodu (analitik 2,8371 s) | 2,8394 s |
| Zincir / ip / çubuk | ip y = 6,9999; çubuk hatası 0,6 mm; zincir < 5 cm |
| Menteşe düzlem / sınır / motor | düzlem dışı sapma 0; sınır tutuyor; 5,000 rad/s |
| Eklem belirleyiciliği | hash aynı |

Performans: 2000 kutuluk piramit ilk ölçümde 29 → 46 ms göründü; eski ve yeni wasm aynı sahnede dönüşümlü
koşturulunca 4,99 vs 5,08 ms — fark makine gürültüsü (±%50), gerileme değil. Kutu-kutu yolu değişmedi;
eski fizik hash'leri aynı.

Bilinen sınırlamalar: hull eylemsizliği AABB kutusu yaklaşımı; mesh'te iç kenar düzeltmesi yok (üçgen
kenarlarında küçük sekmeler olabilir); zincir halkaları yumuşak kısıt yüzünden 4 cm'e kadar uzayabiliyor;
mesh dışındaki raycast hâlâ doğrusal tarama; karakter fizik gövdesi değil.
Sonraki adımlar (Faz 4): CCD (hızlı küreler / mermiler), mesh iç kenar düzeltmesi, gerçek hull eylemsizliği,
raycast için dünya BVH'si, kısıt grafiği renklendirmesi + SIMD çözücü.
`engine.wasm` 166.560 → 227.805 bayt.

### Faz 4 raporu (2026-10-27) — CCD, mesh iç kenar düzeltmesi, gerçek hull eylemsizliği
Dosyalar: `world.c` (CCD, eylemsizlik çerçevesi), `shapes.c/.h` (tetrahedron tensörü, Jacobi özayrışımı,
kenar komşuluğu + aktif kenar bayrakları), `collide.c` (`mesh_fix_edges`), `tests/test_phys_ccd.c`,
`phys_bench.c` / `tools/bench_phys.mjs` (`--ccd 0|1`), demo `G` tuşu (mermi).

- **Hull eylemsizliği:** merkez etrafında tetrahedron kovaryansı → tam 3×3 tensör → Jacobi ile asal eksenler
  (`hull->inertia_axes`, det = +1) ve momentler. Gövde köşegen tensörü asal çerçevede tutar; dünya tensörü
  (R·Q)·diag·(R·Q)ᵀ. Eski AABB yaklaşımı döndürülmüş bir kutuda %69 hatalıydı.
- **Mesh iç kenarları:** `trimesh_create` köşe indeksleriyle kenar komşuluğunu bulur (açık adresli tablo, geçici
  bellek arena işaretiyle geri verilir). Kenar aktif: komşusuz, çoklu, ters sarımlı ya da belirgin dışbükey
  (dihedral > ~5°). Temasta normal yüz normalinden sapıyor ve hiçbir temas noktası aktif kenarda / köşede
  değilse normal yüz normaline çevrilir, derinlik yeniden hesaplanır. Küre / kapsülde temas, çekirdek
  uçlarının gerçek en alt noktasına taşınır (yuvarlanırken zıplamayı önler). `w->mesh_edge_fix`.
- **CCD (Box2D v3 yaklaşımı):** adım sonunda yer değiştirmesi (+ dönüş × max_extent) yarım min_extent'i aşan
  gövdeler adım başı → sonu süpürülür; muhafazakâr ilerleme (GJK mesafesi, doğrusal + sabit eksenli dönüş
  sınırı). Çarpışma varsa gövde TOI pozuna konur, hız korunur; sonraki adımın spekülatif teması durdurur.
  Normal gövdeler statiklere (düzlem, kutu, hull, mesh üçgenleri), `BODY_BULLET` gövdeler dinamiklere de
  karşı. Başta değiyorsa (t = 0) merkezde 0,25·min_extent küreyle yeniden süpürülür. Adım sonu ilişkisi sığsa
  (boşluk < PHYS_MARGIN ya da girişim < 0,25·min_extent) geri çekilmez — eş düzlemli üçgen / karo
  kenarlarında takılmayı önler; merkez üçgen düzlemini geçtiyse her zaman tünel sayılır. `w->continuous`.

Hata ayıklama (testler yakaladı):
1. Mesh'te yuvarlanan küre düzeltmeyle de zıplıyordu (|v_y| 1,74): düzeltilen temasın çapası eğik normal
   boyunca kalan yüzey noktasındaydı; küre yuvarlanınca bu nokta aşağı döner, soft çözücü girişim sanıp iter →
   küre/kapsülde temas çekirdek ucunun yüz normali boyunca en alt noktasına (0,12; düzlem 0,08).
2. CCD, mesh üzerinde 40 m/s kayan kutuyu 0,5 s'de 20 m yerine 10,5 m'de bıraktı: eş düzlemli sonraki üçgene
   yatay TOI bulunuyor, kutu her adım geri çekiliyordu → "adım sonu sığ ise geri çekme" kuralı (statik kutu
   karolarında da aynı sorun vardı, testte).

Testler (`python build.py test`: 69/69; yeni 8 test):
| Test | Sonuç |
|---|---|
| Döndürülmüş kutu bulutundan hull: I⁻¹ vs analitik R·diag·Rᵀ | göreli hata 1e-6 (eski AABB: 0,69) |
| Rastgele hull: tensör vs 72³ ızgara örneklemesi | göreli hata 1,4e-4; hacim 1,4948 / 1,4946 |
| Kenar bayrakları: düz ızgara / çatı sırtı / vadi | sınır 16/16 aktif, iç 0; sırt aktif; vadi pasif |
| Düz ızgarada kayan kutu (düzeltme kapalı → açık) | \|v_y\| 2,59 → 0,0003; \|ω\| 5,30 → 0,001; düzleme göre son konum 6,58 m → 0,3 mm |
| Yuvarlanan küre (kapalı → açık) | \|v_y\| 1,67 → 0,12 (düzlem 0,08); sapma 1,38 m → 7 mm |
| Kayan hull | düzlemle aynı (sapma 1 mm) |
| Küre 300 m/s → 10 cm kutu duvar / sıfır kalınlık mesh duvar | CCD kapalı x = 12,5 / 200 (geçti); açık 4,900 |
| Dönen kapsül (40 rad/s) 200 m/s → duvar | kapalı 133 (geçti); açık 4,975 |
| Kutu 150 m/s düzleme | en büyük girişim 0,854 m → −0,005 (boşlukta durdu) |
| 40 m/s kayma: düzlem / mesh / statik karolar | 20,000 m (takılma yok; her adım süpürüldü) |
| Mermi 400 m/s → 10 cm dinamik tahta | normal: geçti (tahta 0 m/s); BODY_BULLET: tahta 8,61 m/s, belirleyici |

Ölçüm (`node tools/bench_phys.mjs --n 500,2000 --scenes 0,1 --solvers 1 --ccd 0|1`, dönüşümlü 2 tur, ms oturmuş):
| sahne | n | CCD kapalı | CCD açık | hash |
|---|---|---|---|---|
| yığın | 500 | 2,32 / 2,37 | 2,28 / 2,58 | değişti (17 m'den düşen kutular süpürülüyor) |
| yığın | 2000 | 13,35 / 14,32 | 12,89 / 13,71 | değişti; KE 0,63 → 0,65 J |
| piramit | 500 | 8,79 / 8,67 | 7,97 / 8,48 | aynı |
| piramit | 2000 | 48,31 / 46,79 | 45,12 / 45,89 | aynı |
CCD maliyeti gürültü içinde (±%10). CCD kapalıyken yığın 2000 KE 0,6305 J = Faz 2 kaydı (0,63).

Bilinen sınırlamalar: CCD statik engelleri doğrusal tarar (dünya BVH'si yok; hızlı gövde azsa ucuz); dönüş
süpürmesi kısa yoldan (bir adımda > 180° dönen gövde yaklaşık); TOI sonrası adımın kalanı harcanır (Box2D gibi
alt adım yok). Jiroskopik terim yok (asimetrik hull'da açısal momentum korunmaz; serbest dönüşte ω sabit kalır).
Mesh komşuluğu köşe indeksleriyle: aynı konumda ayrı köşeli üçgenler kenar paylaşmaz (o kenar aktif kalır).
Soft çözücüde dönen çapa: yuvarlanan küre düzlemde de |v_y| ≈ 0,08 (Faz 1'den beri).
Sonraki adımlar: yumuşak cisim / kumaş, kısıt grafiği renklendirmesi + SIMD çözücü, dünya BVH'si (raycast + CCD).
`engine.wasm` 227.805 → 240.124 bayt.

### Faz 5 raporu (2026-10-27) — XPBD yumuşak cisimler: kumaş ve jöle
Dosyalar: `src/phys/soft.h/.c`, `tests/test_soft.c`, demo sahne 6 (`soft_scene_build`, `soft_mesh_*`), `web/3d.js`
(yumuşak satırı), `web/3d.html`.

- **Çözücü:** XPBD "küçük adımlar" (Macklin ve ark. 2019): adım başına 10 alt adım, alt adımda tek Gauss-Seidel
  turu, λ her alt adımda sıfırdan. Uyum α = 1/k (m/N), α̃ = α/h². Kısıtlar: mesafe (yapısal / kesme / bükülme /
  tetrahedron kenarı), tek yönlü bağ (tether), tetrahedron hacmi (C = V − V0, Müller'in gradyan sırası).
- **Kumaş:** ızgara; yapısal + iki köşegen kesme + iki öteye bükülme kısıtı. Rüzgâr: üçgen başına normal yönlü
  sürükleme F = −½ρC_d·A·v_n|v_n|·n̂ (köşelere üçte bir). İğneleme + uzun menzilli bağlar (Müller 2012).
- **Jöle:** küp ızgara, hücre başına 5 tetrahedron (tek pariteli hücrelerde aynalı → uyumlu yüzler), tekil
  kenarlar, hacim kısıtı (uyum 0 = sıkışmaz). Kütle tetrahedron hacmiyle dağıtılır. Parçacık yarıçapı = kafes
  adımının yarısı (komşular değer; dış zarf = verilen boyut). Çizimde yüzey normal boyunca yarıçap kadar itilir.
- **Rijit dünyayla iki yönlü çarpışma:** aday gövdeler adım başında AABB ile (adım başı + sonu birleşimi);
  hareketli gövdenin pozu `pos0/rot0 → pos/rot` arası alt adımlara ara değerlenir (yoksa bir karede giren gövde
  parçacığı alt adım sayısı kadar hızlandırırdı). Yüzey uzaklığı: düzlem, küre, kapsül, kutu analitik; hull yüz
  düzlemleri (dışarıda kesin uzaklık GJK); mesh BVH + en yakın üçgen noktası, alt adımda düzlem üçgenin içinden
  geçildiyse önceki tarafa. Parçacık tam dışarı itilir + PBD sürtünmesi (gövde yüzey hızına göre); dinamik gövdeye
  parçacığın momentum değişimi kadar ters itki (`phys_apply_impulse_at`). Uyuyan gövdeye küçük itki uyandırmaz.
- **Yumuşak-yumuşak:** farklı gövdelerin parçacıkları küre-küre itme + sürtünme (AABB ön elemeli O(n·m)).
- **Demo sahne 6:** direkte 3×2 m bayrak (384 parçacık, zamanla değişen rüzgâr), zeminde 1,4 m jöle (125),
  dinamik sandığın üstünde ve statik bloğun kenarına düşen iki 1 m jöle (64'er). `F` top / `G` mermi ile
  vurulabilir (tarayıcıda: top küçük jöleyi sandıktan düşürdü). Yumuşak adım 637 parçacıkta ~0,85 ms
  (gizli bölmede ölçüldü, yavaş taraf).

Hata ayıklama (testler yakaladı):
1. Üstteki jöle alttakinin içine gömülüp kayıyordu: parçacık yarıçapı kafes adımının çeyreğiydi, karşı gövdenin
   parçacıkları aradan geçiyordu → yarıçap = yarım adım, kafes içe çekildi (dış boyut korunur), yumuşak-yumuşak
   sürtünme eklendi.
2. Bayrak uzaması %6: tepe değer başlangıçta (rüzgâra dik bayrağa ≈ 176 N, 0,4 kg) iğneli kenardaydı → gerçekçi
   kütle (0,9 kg ≈ 150 g/m² naylon) ve ölçüm 2 s sonra. Kalan %5 serbest kenarın kırbaç anlarında; bağlar ve
   20 alt adım bunu ancak %4'e indiriyor (yakınsama hatası değil, yerel).

Testler (`python build.py test`: 77/77; yeni 8 test):
| Test | Sonuç |
|---|---|
| Uyumlu yay statik uzaması (analitik m·g·α = 0,01962 m) | 5 / 10 alt adım: 0,01962; 20: 0,01989 (f32: g·h² ≈ 57 ulp) |
| Sarkaç periyodu (analitik 2,8388 s, θ0 = 0,1) | 2,8361 s (%0,1); uzunluk hatası 0 |
| Jöle düzleme düşer | hacim 0,992–1,000 × dinlenme; en alçak parçacık = yarıçap; KE 0 |
| Kumaş küreye / mesh eğimine düşer | küreye en yakın = yarıçap (0,0505); mesh altına geçen yok; uzama %2,6 |
| İki yönlü momentum (jöle 4 m/s → serbest kutu, g = 0) | 8,0000 → 7,9993 (jöle −3,56 + kutu 11,56) |
| Bayrak rüzgârda 5 s (bağsız / bağlı / bağlı 20 alt adım) | uzama %5,2 / %5,0 / %4,1; ağırlık merkezi rüzgâr yönünde 1,4 m; 0,33 ms/adım |
| İki jöle üst üste | en yakın parçacık çifti = rA + rB (0,25); üstteki y 1,49; hacim korunur |
| Belirleyicilik (bayrak + jöle + rijit kutu/küre) | hash aynı |

Bilinen sınırlamalar: kumaşın kendiyle çarpışması yok; rijit gövde kumaşın parçacıkları arasından geçebilir
(küçük / hızlı gövde: mermi bayraktan geçer); yumuşak-yumuşak teması parçacık küreleri (yüzey "yumurta kolisi":
büyük kaydırmayla üst üste konan jöle yavaşça kayıp düşebilir); yumuşak gövdeler uyumaz; rijit gövde parçacığa
konumla değil itkiyle tepki verir (bir adım gecikmeli); jöle hacmi yalnız tetrahedronlarla (yüzey basıncı yok);
f32'de çok küçük alt adımda (> 20) yuvarlama hatası büyür.
Sonraki adımlar: kısıt grafiği renklendirmesi + SIMD çözücü (rijit ve XPBD), dünya BVH'si, kumaş öz-çarpışması.
`engine.wasm` 240.124 → 263.154 bayt.

### Faz 6 raporu (2026-10-28) — kısıt grafiği renklendirmesi ve SIMD temas çözücüsü
Dosyalar: `world.c` (`color_manifolds`, `warm_manifold` / `solve_manifold`, renk sırası), `solver.h`,
`solver_simd_wasm.c`, `phys.h` (Body çözücü bloğu, renk alanları), `tests/test_phys_simd.c`,
`phys_bench.c` / `tools/bench_phys.mjs` (`--order 1|2|3`).

- **Renklendirme (Box2D v3):** dar fazdan sonra açgözlü: her manifold, iki dinamik gövdesinin de kullanmadığı
  en küçük renge (16 renk, gövde bit kümeleri); statik gövdeler renk tüketmez; sığmayan taşma rengine.
  Renk içindeki manifoldlar ortak dinamik gövde paylaşmaz → renk içi sıra sonucu DEĞİŞTİRMEZ.
- **Bit-bit eşitliğin dayanağı:** skaler yol manifoldları renk sırasıyla aynı fonksiyonla çözer; SIMD aynı
  formülü aynı işlem sırasıyla 4 manifoldda (lane = manifold) uygular: `m_maxf(a, b)` = `f32x4.pmax(b, a)`,
  dallar `bitselect`, FMA yok, eksik nokta / boş lane'de gövde ve itkiler seçimle eski haline döner, statik
  gövdeler geri yazılmaz (skalerde itki × 0 bit-bit etkisiz). Taşma rengi iki yolda da skaler, en sonda.
- **Veri yerleşimi:** Body'nin çözücü alanları 16 bayt hizalı ardışık 128 bayt (hız + ters kütle, açısal hız,
  konum, I⁻¹ dünya, R) → lane başına 8 vektör yükleme + 4×4 devrik; yalnızca hız / açısal hız geri yazılır.
  Kısıt verisi adım başında SoA gruplarına kopyalanır, adım sonunda itkiler manifoldlara geri yazılır
  (restitüsyon + sonraki adımın warm start'ı). Eklemler ve restitüsyon skaler.
- Varsayılan: `graph_color = simd = TRUE`. Eski oluşturulma sırası `graph_color = FALSE` ile duruyor (A/B).

Doğrulama:
| Test | Sonuç |
|---|---|
| Skaler renk sırası vs SIMD, durum hash'i HER adımda (240 adım): yığın / piramit + top / karışık şekil + mesh + eklem / warm start yok + uyku | 4 sahnede ilk farklı adım: yok; en çok 8 / 16 / 9 / 9 renk, taşma 0 |
| Renk geçerliliği (piramit, 960 manifold, 13 renk) | aynı renkte ortak dinamik gövde 0; her manifold bir kez |
| Ölçüm hash'leri (2000 kutu): renk skaler = renk SIMD | 8fb51018, 2f112758, f4fca578, 22de5c2f — birebir |
| Oluşturulma sırası hash'leri = Faz 4 kaydı | 77494e50 / 5fa6c7bb — eski yol değişmedi |
| Tüm testler | 79/79 |

Ölçüm (`node tools/bench_phys.mjs --n 500,2000 --scenes 0,1 --solvers 1 --order 1|2|3`, dönüşümlü 2 tur,
oturmuş ms / çözücü ms; i7-12650H, node 24):
| sahne | n | sıralı (Faz 5) | renk skaler | renk SIMD | çözücü hızlanması |
|---|---|---|---|---|---|
| yığın | 500 | 2,62 / 2,25 — 2,54 / 2,17 | 2,77 / 2,36 — 2,52 / 2,17 | 1,06 / 0,72 — 1,10 / 0,75 | ×3,0 |
| yığın | 2000 | 15,17 / 12,17 — 14,10 / 11,43 | 15,07 / 12,12 — 14,51 / 11,74 | 6,08 / 3,56 — 5,92 / 3,47 | ×3,4 |
| piramit | 500 | 8,65 / 7,78 — 8,39 / 7,54 | 8,50 / 7,59 — 9,96 / 8,89 | 3,36 / 2,38 — 3,34 / 2,43 | ×3,2 |
| piramit | 2000 | 44,50 / 39,42 — 45,48 / 40,18 | 46,41 / 41,23 — 47,71 / 42,34 | 18,21 / 12,52 — 17,72 / 12,41 | ×3,3 |
Adım toplamı ×2,4–2,6 (kalan: çarpışma ~2,5–5,7 ms, eklem/restitüsyon/entegrasyon skaler). Renk sırası tek başına
maliyeti değiştirmiyor. Kararlılık renk sırasıyla daha iyi: 2000 yığın KE 0,65 → 0,14 J; 31 katlı duvar
(496) en büyük yer değiştirme 0,156 → 0,073 m, adım 4,69 → 1,79 ms; uyku açık 2000 yığın 14,30 → 5,50 ms.

Bilinen sınırlamalar: eklemler, restitüsyon, entegrasyon ve XPBD yumuşak cisimler skaler; çarpışma (dar faz)
artık adımın büyük kısmı; 16 rengi aşan yoğun düğümler taşma rengine düşer (testlerde hiç olmadı).
`engine.wasm` 263.154 → 278.076 bayt.

### Faz 7 raporu (2026-10-28) — dinamik dünya BVH'si
Dosyalar: `src/phys/bvh.h/.c` (dinamik AABB ağacı), `world.c` (iki ağaç, kalıcı çift kümesi, kanonik çift
sırası, `phys_query_aabb`, CCD adayları), `query.c` (ağaç raycast'i), `soft.c` (rijit adaylar),
`tests/test_phys_bvh.c`, `phys_bench.c` / `tools/bench_phys.mjs` (`--bp sap|bvh`, `--rays N`, sahne 3 "şehir").

- **Ağaç:** Box2D v2.4 b2DynamicTree'nin 3B uyarlaması: yüzey alanı maliyetiyle kardeş seçimi, yukarı yürürken
  AVL tarzı döndürme, şişman yaprak (dinamik ağaçta ±0,1 m), sabit havuz + boş liste, yığın tabanlı sorgu ve
  ışın taraması (yaprakta geri çağırma tmax'ı küçültür). İki ağaç: statik (hiç taşınmaz) ve dinamik.
- **Geniş faz:** kalıcı çift kümesi P = şişman kutuları kesişen tüm (dinamik, dinamik|statik) çiftler. Yalnızca
  şişman kutusundan taşıp yeniden eklenen ("taşınan") vekiller ağaçları sorgular; her adım P'den artık kesişmeyenler
  atılır, adımın çiftleri P'de sıkı AABB'si kesişen ve biri uyanık olanlardır → SAP ile AYNI küme.
- **Kanonik çift sırası:** iki modda da çiftler (a < b) anahtarıyla radix sıralanır → dar faz, manifold sırası,
  renklendirme ve sonuç geniş faz yönteminden bağımsız. SAP ↔ BVH hash'i her adımda eşit. (Bu sıralama Faz 6
  hash'lerini değiştirdi: yığın 2000 2f112758 → c5e58ec1; kararlılık aynı.)
- **Sorgular:** raycast iki ağaç + düzlemler; doğrusal taramayla aynı kural (en küçük t, eşitlikte büyük indeks).
  CCD adayları süpürme kutusunun ağaç sorgusundan (sonuç = en küçük TOI, sıradan bağımsız). Yumuşak cisim rijit
  adayları ağaç sorgusu (kutu, rijitlerin son adımdaki en büyük yer değiştirmesi kadar büyütülür) + düzlemler,
  artan indeks sırasıyla birleştirilir. Ağaç adım sonunda güncellenir (adımlar arası raycast doğru).
- `w->broadphase = PHYS_BP_SAP` eski yolu (SAP + doğrusal raycast / CCD / yumuşak aday) A/B için tutar.

Hata ayıklama: ilk sürüm her uyanık gövdeyi her adım ağaçta sorguluyordu; yoğun, hepsi uyanık yığında SAP'tan
yavaştı (2000 yığın geniş faz 1,35 → 2,60 ms) → kalıcı çift kümesi + taşınan vekil sorgusu (0,44 ms).

Doğrulama (`python build.py test`: 82/82; yeni 3 test):
| Test | Sonuç |
|---|---|
| Ağaç: 1500 yaprak + 3000 rastgele ekle / taşı / çıkar | yapı tutarlı; yükseklik 13 → 12 (log₂ n ≈ 10,6); sorgu kaçırılan 0 |
| SAP ↔ BVH durum hash'i HER adımda (240): yığın + uyku / karışık + mesh + eklem / şehir + CCD mermileri / yığın + jöle + kumaş | 4 sahnede ilk farklı adım: yok |
| 2000 rastgele ışın, SAP (doğrusal) ↔ BVH | 1543 isabet; gövde, t, normal bit-bit aynı |

Ölçüm (`node tools/bench_phys.mjs --solvers 1 --bp sap|bvh [--rays 10000]`, i7-12650H, node 24; ms oturmuş /
çarpışma / geniş faz; şehir: ms aktif ve ışın ms/1000; hash'ler ve Σt iki modda aynı):
| sahne | n | Faz 6 (SAP, sırasız) | SAP + kanonik sıra | BVH | geniş faz hızlanması |
|---|---|---|---|---|---|
| yığın | 500 | 1,08 / 0,34 / — | 1,07 / 0,36 / 0,145 | 0,94 / 0,24 / 0,051 | ×2,8 |
| yığın | 2000 | 6,12 / 2,53 / — | 5,78 / 2,41 / 1,350 | 4,58 / 1,42 / 0,435 | ×3,1 |
| piramit | 500 | 3,45 / 0,95 / — | 3,10 / 0,87 / 0,166 | 2,90 / 0,72 / 0,050 | ×3,3 |
| piramit | 2000 | 18,61 / 5,61 / — | 17,62 / 5,33 / 1,609 | 15,39 / 3,87 / 0,246 | ×6,5 |
| piramit 2000, hepsi uyuyor | | | 0,363 / 0,338 / 0,324 | 0,161 / 0,102 / 0,086 | ×3,8 |

| şehir (statik bina, n/10 düşen kutu, 16 mermi) | SAP: ms aktif / oturmuş / ışın | BVH: ms aktif / oturmuş / ışın | ışın hızlanması |
|---|---|---|---|
| 1000 | 0,244 / 0,115 / 2,33 | 0,196 / 0,058 / 0,228 | ×10 |
| 4000 | 1,374 / 0,443 / 8,99 | 0,713 / 0,205 / 0,348 | ×26 |
| 8000 | 5,567 / 1,101 / 28,8 | 1,545 / 0,438 / 0,481 | ×60 |
| 16000 | 28,24 / 4,217 / 92,7 | 3,657 / 1,034 / 0,935 | ×99 |
Şehir 16000'de aktif adım ×7,7 (doğrusal CCD taraması gitti). Kalan geniş faz maliyeti P taraması (çift sayısıyla
doğrusal) ve ağaç güncellemesi (tüm dinamik gövdeler, şişman kutu denetimi).

Bilinen sınırlamalar: statik gövde taşınırsa ağaç güncellenmez (statikler sabit varsayılır); gövde çıkarma API'si
yok; 65536 gövde sınırı (çift anahtarı 16+16 bit); P taşarsa (pair_cap) çiftler düşer; kanonik sıralama SAP
yolunu da etkiledi (Faz 6 hash'leri değişti).
`engine.wasm` 278.076 → 294.210 bayt.

### Faz 8 raporu (2026-10-28) — kumaş öz-çarpışması ve rijit ↔ kumaş teması
Dosyalar: `soft.c/.h` (öz-çarpışma, rijit yolu, iz silindiri + yakalama, rijit-üçgen dar fazı, sürükleme
kararlılığı), `tests/test_soft_cloth.c`, demo (mermi 50 g, sahne 6 kamerası bayrağa bakar).

- **Öz-çarpışma:** uzamsal karma (hücre = 2r, sayma sıralaması → artan indeks, belirleyici), 27 komşu hücre;
  dinlenmede 2r'den yakın çiftler (kumaşın kendi örgüsü) hariç; itme + sürtünme. Parçacık bir alt adımda en çok
  r ilerler (hız sınırı r/h) ve yarıçap > 0,354·aralık → başka katmanın parçacığı dört komşunun arasından geçemez.
- **Rijit yolu:** hareketli rijit gövdenin adım içi konumu yumuşak gövdeler arasında ortak bir yolda tutulur.
  Temas olana dek eski ara değer (sonuç değişmez); ilk itkiden sonra p += v·h ve itkiler v'yi de değiştirir →
  gövde AYNI karede yavaşlar; adım sonunda konumu yolun sonuna yazılır. (Önceden gövde kumaşın içinden kendi
  rijit yolunu sürüyor, itkisi ancak sonraki karede etki ediyordu.)
- **Hızlı gövde (> 5 m/s) — iz silindiri + yakalama:** hareket yönünde gövdenin iz silindirindeki (yanal < R) ve
  alt adım başında önünde olan parçacıklar ön yarıküreye konur ve gövdeye "yakalanır" (göreli sabit). Her alt
  adımda kumaş kısıtlarının yakalanan parçacığı çektiği kadar momentum gövdeye döner → kumaş ağ gibi tutar.
  Gövde < 1 m/s'ye düşünce, geri dönünce ya da parçacık gövde boyundan fazla çekilince bırakılır.
- **Yavaş gövde — üçgen teması:** `phys_collide_cores` (GJK / EPA + kırpma, mesh üçgenleriyle aynı dar faz);
  en derin nokta kalınlık kadar dışarı, köşelere barisentrik (Δx_i = D·w_i·m_i⁻¹ / Σw²m⁻¹), gövdeye −P.
  Nokta ağırlıkça iğneli köşelerdeyse (Σw²m⁻¹ < 0,1·max m⁻¹) bölge sabit sayılır.

Hata ayıklama (zor kısım; testler + adım adım iz):
1. Mermi bayraktan geçiyordu (her şey kapalı: 160 m/s'de hiç yavaşlamadan). İlk deneme (tek üçgeni itmek + merkez
   düzlemi geçti mi) yalnızca 1,2 m/s yavaşlattı: tek üçgen itilince kumaş "çadır" olur, mermi tepe köşeden geçer
   (eğik izdüşümde hiçbir üçgenin içinde değil) ve kısıtlar çadırı merminin adımından hızlı geri çeker.
2. Parçacık düzeyi süpürülmüş küre: mermi 7 m/s'ye düştü ama gerili kumaşın parçacıkları kürenin yanından kayıp
   arkaya geçti (0,13 m aralıklı ağda delik) → iz SİLİNDİRİ (yanal kayma yok) + iki yönlü yakalama.
3. NaN: kutu yığınına düşen kumaşta. Adım içi denetimle ilk NaN "tahmin" evresinde bulundu → kaynak açık ikinci
   dereceden hava sürüklemesi (rüzgârsız da çalışır): yüksek hızda bir alt adımda Δv, v'yi aşıp salınımı
   büyütüyordu (280 m/s'de ~700 m/s) → köşe başına |Δv_n| ≤ |v_n|. Yeni özellikler yalnızca kumaşı o hıza itiyordu.
4. Düşen kutular (20 m/s) da "mermi" sayılıp dönerken yakalamayla çekişiyordu → yakalama eşiği 5 m/s, bırakma 1 m/s,
   aşırı çekilme sınırı; üçgen temasında düzlem tabanlı yüz kuralı kutularda yanlıştı (üçgen dışarıdayken düzlemi
   gövdeyi keser) → genel dar faz.
5. SAP ↔ BVH eşitliği bu NaN yüzünden bozulmuştu (NaN karşılaştırmaları iki geniş fazda farklı) → düzeldi.

Testler (`python build.py test`: 85/85; yeni 3 test):
| Test | KAPALI | AÇIK |
|---|---|---|
| Mermi 160 m/s, r 0,12, 50 g → 3×2 m bayrak (iğneli, bağlı) | geçti, 161 m/s, z = −235 | geçmedi: cepte, z en az −1,07, uzama %6 |
| Top 28 m/s, r 0,4, 4 kg | geçti, 18 m/s, z = −14,6 | geçmedi: z en az −1,14 (geri sekti) |
| Mermi 160 m/s, 1 kg (bilgi) | geçti | geçti ama 125 m/s'ye yavaşladı: 160 N·s, 0,9 kg bayrak fiziksel olarak tutamaz |
| Dikey bırakılıp yerde katlanan kumaş (12×24): dinlenmede uzak çiftlerin en yakını | 0,041 m (yarı kalınlıktan yakın 1 çift) | 0,085 m (0 çift; kalınlık 0,104) |
| Rüzgârda bayrak + 5 mermi, iki koşu | — | hash aynı |
| SAP ↔ BVH (yığın + jöle + kumaş, öz-çarpışma + üçgen + yakalama açık) | — | her adım eşit |

Maliyet (bayrak 384 parçacık, 10 alt adım, rüzgâr, direk; ms/adım): ikisi kapalı 0,49 · öz-çarpışma 1,38 ·
üçgen teması 0,76 · ikisi açık 1,79. Katlanan kumaş (288 parçacık): 0,23 → 0,98. Öz-çarpışmanın maliyeti alt
adım başına karma kurulumu ve 27 hücre taraması (komşu listesini adım başına bir kez kurmak sonraki iyileştirme).

Bilinen sınırlamalar: yakalama yalnızca kumaş ↔ rijit (yumuşak-yumuşak yok); yakalanan parçacığın gövdeye göre
konumu dünya ofsetiyle (gövde dönüşü yok sayılır — küre için doğru); çok ağır mermi kumaşı yine geçer (fiziksel);
yakalama küre için kesin, diğer şekillerde iç küre (min_extent); kumaş yırtılması yok; öz-çarpışma üçgen-üçgen
değil parçacık-parçacık (ince açılı kıvrımlarda kaçak olabilir); 150 kutunun dövdüğü kumaşta hız 250 m/s'ye
çıkabiliyor (kararlı, ama gerçekçi değil).
`engine.wasm` 294.210 → 302.304 bayt.
