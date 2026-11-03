/* tests/test_wave.c — dalga denklemi (phys/wave): yayılma hızı analitikle, enerji korunumu, kararlılık, sönüm, engel yansıması,
 * doku eğim kodlaması ve kumaş gerinim alanı. */
#include "test.h"
#include "core/math.h"
#include "core/mem.h"
#include "phys/soft.h"
#include "phys/wave.h"

/* tepenin dairesel cephesi c·t kadar ilerler */
TEST(wave_front_travels_at_c) {
    Arena* a = test_arena();
    WaveField w;
    wave_init(&w, a, 257, 64.0f, 3.0f, 0.0f);
    wave_add(&w, 0.0f, 0.0f, 0.5f, 0.1f);
    f32 t = 6.0f;
    wave_step(&w, 0.0f);
    for (f32 s = 0.0f; s < t; s += w.dt) wave_step(&w, w.dt);
    f32 best = 0.0f, best_r = 0.0f;
    for (f32 r = 2.0f; r < 30.0f; r += 0.05f) {
        f32 v = m_absf(wave_height(&w, r, 0.0f));
        if (v > best) { best = v; best_r = r; }
    }
    test_note("cephe yarıçapı (m), beklenen 18", (f64)best_r, 0);
    CHECK(best > 0.0005f);
    CHECK_NEAR(best_r, 18.0f, 1.0f);
}

/* sönümsüz: leapfrog'un ayrık enerjisi korunur, patlamaz */
TEST(wave_energy_conserved_without_damping) {
    Arena* a = test_arena();
    WaveField w;
    wave_init(&w, a, 129, 32.0f, 3.0f, 0.0f);
    wave_add(&w, 1.0f, -2.0f, 1.0f, 0.2f);
    wave_step(&w, w.dt * 5.0f);
    f32 e0 = wave_energy(&w), lo = e0, hi = e0;
    for (u32 i = 0; i < 2000; i++) {
        wave_step(&w, w.dt * 1.001f);
        f32 e = wave_energy(&w);
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    test_note("enerji sapması (göreli)", (f64)((hi - lo) / e0), 4);
    CHECK(e0 > 0.0f);
    CHECK((hi - lo) / e0 < 0.01f);
}

TEST(wave_damping_decays) {
    Arena* a = test_arena();
    WaveField w;
    wave_init(&w, a, 65, 16.0f, 3.0f, 1.0f);
    wave_add(&w, 0.0f, 0.0f, 0.5f, 0.2f);
    wave_step(&w, w.dt * 5.0f);
    f32 e0 = wave_energy(&w);
    for (u32 i = 0; i < 4000; i++) wave_step(&w, w.dt * 1.001f);
    CHECK(wave_energy(&w) < e0 * 0.02f);
}

/* sert engel cepheyi geri çevirir: arkası gölgede kalır, önünde yansıma görülür */
TEST(wave_obstacle_reflects_and_shadows) {
    Arena* a = test_arena();
    WaveField w, f;
    wave_init(&w, a, 257, 64.0f, 3.0f, 0.0f);
    wave_init(&f, a, 257, 64.0f, 3.0f, 0.0f);
    wave_block(&w, 8.0f, 0.0f, 0.5f, 10.0f);
    wave_add(&w, 0.0f, 0.0f, 0.5f, 0.2f);
    wave_add(&f, 0.0f, 0.0f, 0.5f, 0.2f);
    f32 behind_w = 0.0f, behind_f = 0.0f, back_w = 0.0f, back_f = 0.0f;
    for (f32 s = 0.0f; s < 5.5f; s += w.dt) {
        wave_step(&w, w.dt);
        wave_step(&f, f.dt);
        behind_w = m_maxf(behind_w, m_absf(wave_height(&w, 12.0f, 0.0f)));
        behind_f = m_maxf(behind_f, m_absf(wave_height(&f, 12.0f, 0.0f)));
        if (s > 4.0f) {   /* serbest cephe çoktan geçti: burada ancak yansıma olabilir */
            back_w = m_maxf(back_w, m_absf(wave_height(&w, 3.0f, 0.0f)));
            back_f = m_maxf(back_f, m_absf(wave_height(&f, 3.0f, 0.0f)));
        }
    }
    CHECK(behind_w < behind_f * 0.25f);
    CHECK(back_w > back_f * 2.0f);
    CHECK(wave_height(&w, 8.0f, 0.0f) == 0.0f);
}

/* doku: düz yüzey 128, eğim kodu analitik eğimle aynı */
TEST(wave_bake_encodes_slope) {
    Arena* a = test_arena();
    WaveField w;
    wave_init(&w, a, 33, 8.0f, 3.0f, 0.0f);
    u32 px[33 * 33];
    wave_bake(&w, px);
    CHECK((px[16 * 33 + 16] & 0xFFFFFFu) == 0x808080u);
    for (u32 y = 0; y < 33; y++) for (u32 x = 0; x < 33; x++) w.h[y * 33 + x] = 0.1f * (f32)x * w.dx;
    wave_bake(&w, px);
    u32 r = px[16 * 33 + 16] & 0xFFu;
    CHECK(r >= 128u + 50u && r <= 128u + 52u);   /* eğim 0,1 → 51 */
    CHECK(((px[16 * 33 + 16] >> 8) & 0xFFu) == 128u);
}

/* kumaş: dinlenmede gerinim 0, tek kenarı çekince gerilme, katlayınca büzülme */
TEST(soft_strain_field_rest_stretch_fold) {
    Arena* a = test_arena();
    SoftBody* b = soft_cloth(a, v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 9, 9, 1.0f, 0.0f, 0.0f, 0.0f);
    f32* ten = ARENA_ARRAY(a, f32, b->count);
    f32* fold = ARENA_ARRAY(a, f32, b->count);
    soft_strain_field(b, ten, fold);
    f32 mx = 0.0f;
    for (u32 i = 0; i < b->count; i++) mx = m_maxf(mx, m_maxf(ten[i], fold[i]));
    CHECK(mx < 1e-5f);
    for (u32 i = 0; i < b->count; i++) b->x[i].x *= 1.1f;
    soft_strain_field(b, ten, fold);
    CHECK_NEAR(ten[4 * 9 + 4], 0.1f, 0.02f);
    CHECK(fold[4 * 9 + 4] < 1e-4f);
    /* orta sütunda 90° kat: büküm kısıtları kısalır */
    for (u32 j = 0; j < 9; j++) {
        for (u32 i = 0; i < 9; i++) {
            f32 u = (f32)i / 8.0f;
            b->x[j * 9 + i] = v3(u < 0.5f ? u : 0.5f, u < 0.5f ? 0.0f : u - 0.5f, (f32)j / 8.0f);
        }
    }
    soft_strain_field(b, ten, fold);
    CHECK(fold[4 * 9 + 3] > 0.02f);
    CHECK(fold[4 * 9 + 1] < 1e-4f);
}
