/* tests/test_phys_tree.c — eklem ağacı doğrudan çözücüsü (D60): kütle oranından bağımsız tam eşitlik çözümü, döngü kapatan
 * eklemin Gauss-Seidel'e düşmesi, iki ucu dünyaya bağlı köprü, yıldız ağaç (çok çocuklu gövde), enerji korunumu.
 * Ölçütler düzeneğin (phys_suite) eşikleriyle aynı ruhta: hata metre, enerji % mgL. */
#include "test.h"
#include "core/math.h"
#include "phys/phys.h"

static const f32 DT = 1.0f / 60.0f;

static PhysWorld* world(u32 n) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), n, n * 16 + 64);
    w->allow_sleep = FALSE;
    return w;
}

static f32 max_joint_error(const PhysWorld* w) {
    f32 e = 0.0f;
    for (u32 j = 0; j < w->joint_count; j++) e = m_maxf(e, phys_joint_error(w, j));
    return e;
}

static b32 finite_world(const PhysWorld* w) {
    for (u32 i = 0; i < w->count; i++) {
        Vec3 p = w->bodies[i].pos;
        if (!(p.x == p.x && p.y == p.y && p.z == p.z) || m_absf(p.x) > 1e4f || m_absf(p.y) > 1e4f) return FALSE;
    }
    return TRUE;
}

/* 20 hafif halka (0,1 kg) + 100 kg ağırlık, yataydan bırakılır: Gauss-Seidel'de patlıyordu (269 m) */
TEST(phys_tree_heavy_chain_mass_ratio_1000) {
    PhysWorld* w = world(32);
    u32 prev = PHYS_NO_BODY;
    for (u32 i = 0; i <= 20; i++) {
        b32 last = i == 20;
        f32 x = last ? 0.25f * 20.0f + 0.75f : 0.25f * (f32)(i + 1);
        BodyDesc b = phys_desc_sphere(v3(x, 12.0f, 0), last ? 0.5f : 0.1f, last ? 100.0f : 0.1f);
        u32 id = phys_add_body(w, &b);
        JointDesc j = phys_joint_desc(JOINT_BALL, id, prev, v3(0.25f * (f32)i, 12.0f, 0));
        phys_add_joint(w, &j);
        prev = id;
    }
    f32 err = 0.0f, stretch = 0.0f;
    for (u32 s = 0; s < 600; s++) {
        phys_step(w, DT);
        err = m_maxf(err, max_joint_error(w));
        stretch = m_maxf(stretch, v3_len(v3_sub(w->bodies[w->count - 1].pos, v3(0, 12.0f, 0))) / 5.75f - 1.0f);
    }
    test_note("ağır zincir: en büyük eklem hatası (m)", err, 6);
    test_note("ağır zincir: uzama (%)", stretch * 100.0f, 4);
    CHECK(err < 0.005f);
    CHECK(stretch < 0.01f);
}

/* tek çubuklu sarkaç: 10 s'de enerji sapması < %1 mgL (relax'lı katı izdüşüm %21 kaybettiriyordu) */
TEST(phys_tree_pendulum_energy) {
    PhysWorld* w = world(4);
    BodyDesc b = phys_desc_sphere(v3(1.0f, 5.0f, 0), 0.05f, 1.0f);
    u32 id = phys_add_body(w, &b);
    JointDesc j = phys_joint_desc(JOINT_DISTANCE, id, PHYS_NO_BODY, v3(1.0f, 5.0f, 0));
    j.anchor_b = v3(0, 5.0f, 0);
    j.min_length = j.max_length = 1.0f;
    phys_add_joint(w, &j);
    f32 e0 = 9.81f * 5.0f, dev = 0.0f;
    for (u32 s = 0; s < 600; s++) {
        phys_step(w, DT);
        const Body* p = &w->bodies[id];
        dev = m_maxf(dev, m_absf(0.5f * v3_dot(p->vel, p->vel) + 9.81f * p->pos.y - e0));
    }
    test_note("sarkaç: enerji sapması (% mgL)", dev / 9.81f * 100.0f, 3);
    CHECK(dev / 9.81f < 0.01f);
}

/* dört kutudan halka: dördüncü eklem döngüyü kapatır → Gauss-Seidel; ağaç kısmı doğrudan. Kararlı ve bağlı kalmalı. */
TEST(phys_tree_loop_closure_falls_back) {
    PhysWorld* w = world(8);
    Vec3 c[4] = { v3(0, 10, 0), v3(1, 10, 0), v3(1, 9, 0), v3(0, 9, 0) };
    u32 id[4];
    for (u32 i = 0; i < 4; i++) {
        BodyDesc b = phys_desc_box(c[i], v3(0.15f, 0.15f, 0.15f), i == 2 ? 50.0f : 1.0f);
        id[i] = phys_add_body(w, &b);
    }
    JointDesc top = phys_joint_desc(JOINT_BALL, id[0], PHYS_NO_BODY, v3(0, 10.5f, 0));
    phys_add_joint(w, &top);
    for (u32 i = 0; i < 4; i++) {
        u32 a = id[i], b = id[(i + 1) & 3];
        JointDesc j = phys_joint_desc(JOINT_BALL, a, b, v3_scale(v3_add(c[i], c[(i + 1) & 3]), 0.5f));
        phys_add_joint(w, &j);
    }
    f32 err = 0.0f;
    for (u32 s = 0; s < 300; s++) {
        phys_step(w, DT);
        if (s >= 60) err = m_maxf(err, max_joint_error(w));
    }
    test_note("halka: oturduktan sonra en büyük eklem hatası (m)", err, 5);
    CHECK(finite_world(w));
    CHECK(err < 0.02f);
}

/* iki ucu dünyaya bağlı 10 parçalı köprü (V biçiminde sarkık kurulur: 9 m açıklık, 10 m tahta; düz gergin köprü tekil
 * olurdu), ortadaki tahta 1000 kat ağır. Dünya düğüm değil → ağaç, iki "dünya yaprağı". */
TEST(phys_tree_bridge_heavy_load) {
    PhysWorld* w = world(16);
    const f32 c = 0.9f, s = 0.43589f;   /* cos, sin (25,8°) */
    u32 prev = PHYS_NO_BODY;
    Vec3 p = v3(0, 5.0f, 0);
    for (u32 i = 0; i < 10; i++) {
        Vec3 d = i < 5 ? v3(c, -s, 0) : v3(c, s, 0);
        BodyDesc b = phys_desc_box(v3_add(p, v3_scale(d, 0.5f)), v3(0.45f, 0.05f, 0.4f), i == 5 ? 500.0f : 0.5f);
        b.rot = quat_from_axis_angle(v3(0, 0, 1), i < 5 ? -0.4510f : 0.4510f);
        u32 id = phys_add_body(w, &b);
        JointDesc h = phys_joint_desc(JOINT_HINGE, prev, id, p);
        h.axis = v3(0, 0, 1);
        phys_add_joint(w, &h);
        prev = id;
        p = v3_add(p, d);
    }
    JointDesc end = phys_joint_desc(JOINT_HINGE, prev, PHYS_NO_BODY, p);
    end.axis = v3(0, 0, 1);
    phys_add_joint(w, &end);
    f32 err = 0.0f;
    for (u32 s2 = 0; s2 < 300; s2++) {
        phys_step(w, DT);
        err = m_maxf(err, max_joint_error(w));
    }
    test_note("köprü: en büyük eklem hatası (m)", err, 6);
    CHECK(finite_world(w));
    CHECK(err < 0.005f);
}

/* yıldız: bir göbeğe dört kol (göbek dört eklemin "yukarı" gövdesi), göbek ağır, kollar hafif; dünyaya göbekten asılı */
TEST(phys_tree_star_hub) {
    PhysWorld* w = world(16);
    BodyDesc hub = phys_desc_box(v3(0, 8, 0), v3(0.3f, 0.3f, 0.3f), 200.0f);
    u32 h = phys_add_body(w, &hub);
    JointDesc top = phys_joint_desc(JOINT_BALL, h, PHYS_NO_BODY, v3(0, 8.3f, 0));
    phys_add_joint(w, &top);
    const Vec3 dir[4] = { v3(1, 0, 0), v3(-1, 0, 0), v3(0, 0, 1), v3(0, 0, -1) };
    for (u32 k = 0; k < 4; k++) {
        u32 p = h;
        for (u32 i = 0; i < 3; i++) {
            Vec3 pos = v3_add(v3(0, 8, 0), v3_scale(dir[k], 0.6f + 0.5f * (f32)i));
            BodyDesc b = phys_desc_sphere(pos, 0.1f, 0.2f);
            u32 id = phys_add_body(w, &b);
            JointDesc j = phys_joint_desc(JOINT_BALL, id, p, v3_sub(pos, v3_scale(dir[k], 0.25f)));
            phys_add_joint(w, &j);
            p = id;
        }
    }
    f32 err = 0.0f;
    for (u32 s = 0; s < 300; s++) {
        phys_step(w, DT);
        err = m_maxf(err, max_joint_error(w));
    }
    test_note("yıldız: en büyük eklem hatası (m)", err, 6);
    CHECK(finite_world(w));
    CHECK(err < 0.005f);
}
