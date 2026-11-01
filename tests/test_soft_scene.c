/* tests/test_soft_scene.c — demo sahne 6'nın (bayrak + 3 jöle + direk / blok / sandık / zemin) eşi: 600 adım
 * (10 s), sonuç parmak izi soft_hash ve adım süresi. Yumuşak cisim çözücüsündeki hızlandırmaların sonucu
 * değiştirmediğini (bit bit aynı hash) ve kazancı ölçmek için; kurulum demo.c soft_scene_build ile birebir. */
#include "test.h"
#include "core/math.h"
#include "core/sys.h"
#include "phys/phys.h"
#include "phys/soft.h"

#define SCENE_STEPS 600u
#define SCENE_HASH  3630462212u   /* hızlandırmalar öncesi ölçülen; değişirse çözücü sonucu değişmiştir */

static Vec3 scene_wind(f32 t) {
    return v3(6.0f + 2.5f * m_sinf(t * 0.7f), 0.0f, 1.5f * m_sinf(t * 0.37f) + 1.0f * m_sinf(t * 1.3f + 1.0f));
}

TEST(soft_demo_scene_hash_and_cost) {
    Arena* a = test_arena();
    PhysWorld* w = ARENA_NEW(a, PhysWorld);
    phys_world_init(w, a, 1024, 8192);
    w->allow_sleep = TRUE;
    SoftWorld* sw = ARENA_NEW(a, SoftWorld);
    soft_world_init(sw, a, 8);
    sw->substeps = 10;
    BodyDesc ground = phys_desc_plane(v3(0, 1, 0), 0.0f);
    ground.friction = 0.8f;
    phys_add_body(w, &ground);
    BodyDesc pole = phys_desc_box(v3(-6.0f, 3.5f, -4.0f), v3(0.08f, 3.5f, 0.08f), 0.0f);
    phys_add_body(w, &pole);
    const u32 nu = 24, nv = 16;
    SoftBody* flag = soft_cloth(a, v3(-5.9f, 6.8f, -4.0f), v3(3.0f, 0, 0), v3(0, -2.0f, 0), nu, nv, 0.9f, 0.0f, 1e-4f, 0.05f);
    for (u32 j = 0; j < nv; j++) soft_pin(flag, j * nu);
    soft_add_tethers(a, flag);
    soft_world_add(sw, flag);
    BodyDesc block = phys_desc_box(v3(3.5f, 0.5f, -1.0f), v3(1.0f, 0.5f, 1.0f), 0.0f);
    phys_add_body(w, &block);
    BodyDesc crate = phys_desc_box(v3(0.0f, 0.4f, -2.0f), v3(0.6f, 0.4f, 0.6f), 6.0f);
    phys_add_body(w, &crate);
    const Vec3 jc[3] = { v3(-2.5f, 0.8f, 0.5f), v3(0.0f, 1.8f, -2.0f), v3(2.7f, 3.5f, -0.6f) };
    const f32 js[3] = { 1.4f, 1.0f, 1.0f };
    const u32 jn[3] = { 4, 3, 3 };
    for (u32 k = 0; k < 3; k++) soft_world_add(sw, soft_jelly(a, jc[k], js[k], jn[k], 4.0f * js[k] * js[k] * js[k], 3e-4f, 0.0f));
    const f32 dt = 1.0f / 60.0f;
    f64 total = 0.0, worst = 0.0;
    for (u32 s = 0; s < SCENE_STEPS; s++) {
        sw->wind = scene_wind((f32)(s + 1) * dt);
        phys_step(w, dt);
        soft_world_step(sw, w, dt);
        total += sw->step_ms;
        if (sw->step_ms > worst) worst = sw->step_ms;
    }
    u32 h = soft_hash(sw);
    test_note("sahne 6 eşi: yumuşak adım ort. (ms)", total / SCENE_STEPS, 3);
    test_note("sahne 6 eşi: yumuşak adım en kötü (ms)", worst, 3);
    test_note("sahne 6 eşi: soft_hash", (f64)h, 0);
    CHECK(h == SCENE_HASH);
    CHECK(soft_max_stretch(flag) < 1.2f);
}
