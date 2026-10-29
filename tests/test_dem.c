/* tests/test_dem.c — Hertz temaslı küreler (phys/dem.c), M11 Faz 4.
 * Tek top: sekme yüksekliği e²·d, düşme yüksekliğinden bağımsız (Tsuji sönümü). e = 1: toplam enerji (kinetik +
 * çekim + esneklik) korunur. Değen yığınlar (boşluksuz): eşit kütlede en üstteki en çok sıçrar, en ağır üstteyse
 * en üstteki en az; en ağır alttaysa üstteki katapult gibi fırlar. Serbest düşüş g(h) ile kesin formüller. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "phys/dem.h"

#define DEM_E      5.0e7   /* sert kauçuk, Pa */
#define DEM_NU     0.45
#define DEM_FRAME  (1.0 / 60.0)

static DemWorld* dem_new(f64 e) {
    DemWorld* w = ARENA_NEW(test_arena(), DemWorld);
    dem_init(w, test_arena(), 8, DEM_E, DEM_NU, e);
    return w;
}

/* ilk zemin temasından sonraki 'window' saniyede her topun en yüksek merkez noktası */
static void dem_peaks(DemWorld* w, f64 window, f64* peak) {
    b32 hit = FALSE;
    f64 t = 0.0, t_end = 1e30;
    for (u32 i = 0; i < w->n; i++) peak[i] = 0.0;
    for (u32 f = 0; f < 60 * 20 && t < t_end; f++) {
        dem_step(w, DEM_FRAME);
        t += DEM_FRAME;
        if (!hit && w->b[0].v[1] > 0.0) { hit = TRUE; t_end = t + window; }   /* temas ~ms: kare sonuna düşmez */
        if (!hit) continue;
        for (u32 i = 0; i < w->n; i++) if (w->b[i].x[1] > peak[i]) peak[i] = w->b[i].x[1];
    }
}

static void note_i(const char* tag, u32 i, f64 v) {
    char buf[96];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    sb_cstr(&sb, tag);
    sb_cstr(&sb, " top ");
    sb_u64(&sb, i);
    buf[sb.len] = 0;
    test_note(buf, v, 3);
}

TEST(dem_single_bounce_restitution_velocity_independent) {
    static const f64 D[] = { 0.5, 1.5, 4.0 };
    for (u32 k = 0; k < 3; k++) {
        DemWorld* w = dem_new(0.9);
        dem_add(w, 0, 0.25 + D[k], 0, 0.25, 2.0);
        f64 p[1];
        dem_peaks(w, 2.0 * __builtin_sqrt(2.0 * D[k] / 9.81) * 0.9 + 0.1, p);
        f64 ratio = (p[0] - 0.25) / D[k];
        note_i("tek: sekme/dusme (hedef e^2 = 0.81)", k, ratio);
        CHECK_NEAR(ratio, 0.81, 0.01);
    }
}

TEST(dem_energy_conserved_elastic) {
    DemWorld* w = dem_new(1.0);
    dem_add(w, 0, 1.25, 0, 0.25, 2.0);
    dem_add(w, 0, 1.75, 0, 0.25, 2.0);
    dem_add(w, 0, 2.25, 0, 0.25, 2.0);
    f64 e0 = dem_energy(w), worst = 0.0;
    for (u32 f = 0; f < 180; f++) {
        dem_step(w, DEM_FRAME);
        f64 d = __builtin_fabs(dem_energy(w) - e0) / e0;
        if (d > worst) worst = d;
    }
    test_note("e=1 yigin: en buyuk goreli enerji sapmasi", worst, 8);
    CHECK(worst < 3e-4);   /* hız Verlet O(h²), h = t_c/40; birikmez (temaslar arası sabit) */
}

/* boşluksuz yığın: alttaki topun yüzeyi 'bottom' yüksekliğinde, toplar birbirine değer */
static f64 stack_gap(DemWorld* w, const f64* mass, u32 n, f64 bottom, f64 gap, f64* start);
static f64 stack(DemWorld* w, const f64* mass, u32 n, f64 bottom, f64* start) { return stack_gap(w, mass, n, bottom, 0.0, start); }
static f64 stack_gap(DemWorld* w, const f64* mass, u32 n, f64 bottom, f64 gap, f64* start) {
    f64 y = bottom;
    for (u32 i = 0; i < n; i++) {
        f64 r = 0.25 * (f64)m_powf((f32)mass[i], 1.0f / 3.0f);
        dem_add(w, 0, y + r, 0, r, mass[i]);
        start[i] = y + r;
        y += 2.0 * r + gap;
    }
    return y;
}

static f64 ratio(const DemWorld* w, const f64* peak, const f64* start, u32 i) {
    return (peak[i] - w->b[i].r) / (start[i] - w->b[i].r);
}

TEST(dem_touching_stacks_real_physics) {
    /* e = 0,9 sert kauçuk; oran = sekme / bırakılış yüksekliği (yüzeyden). Momentum aşağıdan yukarı akar:
     * üstteki ancak altındakiler ona momentum verebiliyorsa (hafif üstte) bırakıldığından yükseğe çıkar. */
    static const f64 M[4][3] = { { 2, 2, 2 }, { 1, 2, 4 }, { 1, 4, 16 }, { 4, 2, 1 } };
    static const char* const TAG[4] = { "esit", "agir ustte", "cok agir ustte", "agir altta" };
    f64 r[4][3];
    for (u32 c = 0; c < 4; c++) {
        DemWorld* w = dem_new(0.9);
        f64 start[3], peak[3], e0 = 0.0;
        stack(w, M[c], 3, 1.5, start);
        e0 = dem_energy(w);
        dem_peaks(w, 2.5, peak);
        for (u32 i = 0; i < 3; i++) { r[c][i] = ratio(w, peak, start, i); note_i(TAG[c], i, r[c][i]); }
        CHECK(dem_energy(w) < e0);   /* sönümlü: enerji yalnızca azalır */
    }
    /* eşit kütle: yukarı doğru artar, en üstteki bırakıldığından yükseğe (alttakilerin momentumu) */
    CHECK(r[0][2] > r[0][1] && r[0][1] > r[0][0]);
    CHECK(r[0][2] > 1.1 && r[0][0] < 0.4);
    /* ağır üstte: katapult yok (≈ bırakıldığı yükseklik), altında sıkışan hafifler neredeyse hiç sıçramaz */
    CHECK(r[1][2] < 1.3 && r[2][2] < 1.1);
    CHECK(r[1][0] < 0.2 && r[2][0] < 0.2);
    CHECK(r[2][2] < r[0][2]);   /* kütle farkı büyüdükçe üstteki eşit-kütle üst topundan az sıçrar */
    /* ağır altta: katapult, en yükseğe çıkan üst top */
    CHECK(r[3][2] > 2.0 && r[3][2] > r[0][2] && r[3][2] > r[1][2]);
}
TEST(dem_free_fall_planet_gravity) {
    const f64 R = 50.0, gm = 9.81 * R * R, d = 50.0;
    DemWorld* w = dem_new(0.9);
    w->planet_gm = gm;
    w->planet_radius = R;
    dem_add(w, 0, d + 0.25, 0, 0.25, 1.0);
    f64 yp = w->b[0].x[1], vp = 0.0, v_hit = 0.0;
    const f64 dt = 1.0 / 240.0;
    for (u32 s = 0; s < 240 * 20; s++) {
        dem_step(w, dt);
        if (w->b[0].x[1] <= 0.25) {
            /* temas (~3 ms) bu adımın içinde: çarpma hızı adım başından kalan yol üzerinden (g(y) bu ~9 cm'de sabit) */
            f64 gy = gm / ((R + yp) * (R + yp));
            v_hit = __builtin_sqrt(vp * vp + 2.0 * gy * (yp - 0.25));
            break;
        }
        yp = w->b[0].x[1];
        vp = w->b[0].v[1];
    }
    f64 v = __builtin_sqrt(2.0 * gm * (1.0 / (R + 0.25) - 1.0 / (R + 0.25 + d)));
    test_note("DEM gezegen: v motor", v_hit, 4);
    test_note("DEM gezegen: v kesin", v, 4);
    CHECK_NEAR(v_hit, v, 1e-3 * v);
}





TEST(dem_matches_exact_event_solution) {
    /* toplar 0,3 m aralıklı → çarpışmalar sıralı; test_phys_drop.c kesin olay çözümü (aynı kurulum, e = 0,9).
     * Hertz temasının sonlu süresi dışında iki model aynı fizik: üst toplar %2, sık çarpışan alttakiler %6 */
    static const f64 M[3][3] = { { 1, 1, 1 }, { 1, 2, 4 }, { 4, 2, 1 } };
    static const f64 REF[3][3] = { { 1.078, 1.741, 2.439 }, { 0.767, 1.289, 2.205 }, { 0.395, 1.238, 7.283 } };
    for (u32 c = 0; c < 3; c++) {
        DemWorld* w = dem_new(0.9);
        for (u32 i = 0; i < 3; i++) dem_add(w, 0, 1.75 + i * 0.8, 0, 0.25, M[c][i]);
        f64 peak[3];
        dem_peaks(w, 1.5, peak);
        for (u32 i = 0; i < 3; i++) note_i("kesin cozum karsilastirma", c * 3 + i, peak[i]);
        CHECK_NEAR(peak[2], REF[c][2], 0.02 * REF[c][2]);
        CHECK_NEAR(peak[1], REF[c][1], 0.03 * REF[c][1]);
        CHECK_NEAR(peak[0], REF[c][0], 0.06 * REF[c][0]);
    }
}

TEST(dem_substep_converged) {
    /* alt adım yarıya inince sonuç değişmemeli (yakınsama) — değen ve 2 mm aralıklı yığın, ağır altta */
    static const f64 M[3] = { 4, 2, 1 };
    for (u32 g = 0; g < 2; g++) {
        f64 top[2];
        for (u32 k = 0; k < 2; k++) {
            DemWorld* w = dem_new(0.9);
            w->steps_per_contact = k ? 80.0 : 40.0;
            f64 start[3], peak[3];
            stack_gap(w, M, 3, 1.5, g ? 0.002 : 0.0, start);
            dem_peaks(w, 2.5, peak);
            top[k] = ratio(w, peak, start, 2);
        }
        test_note("yakinsama: t_c/40", top[0], 4);
        test_note("yakinsama: t_c/80", top[1], 4);
        CHECK_NEAR(top[0], top[1], 0.005 * top[1]);
    }
}

TEST(dem_scene_stacks_2mm) {
    /* sahne 8 kurulumu: toplar 2 mm aralıklı (yerleştirilmiş, yapışık değil) */
    static const f64 M[3][3] = { { 2, 2, 2 }, { 1, 2, 4 }, { 4, 2, 1 } };
    f64 r[3][3];
    for (u32 c = 0; c < 3; c++) {
        DemWorld* w = dem_new(0.9);
        f64 start[3], peak[3];
        stack_gap(w, M[c], 3, 1.5, 0.002, start);
        dem_peaks(w, 2.5, peak);
        for (u32 i = 0; i < 3; i++) { r[c][i] = ratio(w, peak, start, i); note_i("sahne 2 mm", c * 3 + i, r[c][i]); }
    }
    CHECK(r[0][2] > r[0][1] && r[0][1] > r[0][0] && r[0][2] > 1.0);   /* eşit: üstteki en çok, bırakıldığından yükseğe */
    CHECK(r[2][2] > 2.5 && r[2][1] < 0.75 && r[2][0] < 0.3);          /* ağır altta: enerji üste aktarılır */
    CHECK(r[1][0] < 0.25);                                             /* ağır üstte: alttaki hafif sıkışır */
}