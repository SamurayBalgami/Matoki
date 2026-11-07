/* tests/test_phys_suite.c — fizik doğruluk düzeneğinin (D59) gerilemeye karşı korunması.
 * Bugün eşiği geçen sahneler (piramit, ağır zincir (D60), esneklik, domino, mermi, sarkaç enerjisi, belirlenimcilik) geçmeye devam
 * etmeli; kalanlar (dikey yığın, kütle oranı, sürtünme ivmesi, girişim hızı, açısal momentum) iyileştirme listesi: burada yalnız
 * düzeneğin koştuğu ve sonucun belirleyici olduğu denetlenir. Büyük yığın (hız) testte koşmaz. */
#include "test.h"
#include "core/fmt.h"
#include "core/mem.h"

i32 phys_suite_count(void);
f64* phys_suite_run(i32 id);
const char* phys_suite_text(i32 id, i32 k);

enum { S_STACK, S_PYRAMID, S_MASS, S_CHAIN, S_FRICTION, S_RESTITUTION, S_OVERLAP, S_DOMINO, S_BULLET, S_PENDULUM, S_GYRO, S_DET, S_LARGE };

TEST(phys_suite_passing_scenes_stay_passing) {
    CHECK(phys_suite_count() == 13);
    static const i32 MUST[] = { S_PYRAMID, S_CHAIN, S_RESTITUTION, S_DOMINO, S_BULLET, S_PENDULUM, S_DET };
    for (u32 i = 0; i < ARRAY_COUNT(MUST); i++) {
        f64* r = phys_suite_run(MUST[i]);
        CHECK(r != NULL);
        if (!r) return;
        if (r[0] != 1.0) {
            LOG("    geriledi: %s", phys_suite_text(MUST[i], 0));
        }
        CHECK(r[0] == 1.0);
    }
}

TEST(phys_suite_failing_scenes_are_deterministic) {
    static const i32 IDS[] = { S_STACK, S_MASS, S_FRICTION, S_OVERLAP, S_GYRO };
    for (u32 i = 0; i < ARRAY_COUNT(IDS); i++) {
        f64 a[6], b[6];
        mem_copy(a, phys_suite_run(IDS[i]), sizeof(a));
        mem_copy(b, phys_suite_run(IDS[i]), sizeof(b));
        /* ölçütler bit-bit aynı (adım süresi a[5] hariç) */
        CHECK(mem_equal(a, b, 5 * sizeof(f64)));
    }
    /* sürtünme rampası: sınıflandırma bugün doğru (9/9), ivme hatası ayrı iş */
    CHECK(phys_suite_run(S_FRICTION)[1] == 9.0);
}
