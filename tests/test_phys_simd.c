/* tests/test_phys_simd.c — M9 Faz 6: kısıt grafiği renklendirmesi + SIMD temas çözücüsü.
 * Bit-bit eşitlik: aynı sahne renk sırasıyla skaler ve SIMD çözülür, durum hash'i HER adımda karşılaştırılır
 * (kutu yığını, piramit, karışık şekiller + mesh + eklemler, warm start kapalı, uyku açık).
 * Renk geçerliliği: aynı renkteki manifoldlar ortak dinamik gövde paylaşmaz. */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "phys/phys.h"
#include "phys/shapes.h"

static const f32 DT = 1.0f / 60.0f;

static PhysWorld* world(u32 n) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), n, n * 16 + 64);
    return w;
}

static TriMesh* bumpy(void) {
    Arena* a = test_arena();
    const u32 n = 16;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_ARRAY(a, u32, n * n * 6);
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i - 8.0f, z = (f32)j - 8.0f;
            v[j * (n + 1) + i] = v3(x, 0.4f * m_sinf(x * 0.7f) * m_cosf(z * 0.5f), z);
        }
    }
    u32 k = 0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 a0 = j * (n + 1) + i, a1 = a0 + 1, b0 = a0 + n + 1, b1 = b0 + 1;
            idx[k++] = a0; idx[k++] = b0; idx[k++] = b1;
            idx[k++] = a0; idx[k++] = b1; idx[k++] = a1;
        }
    }
    return trimesh_create(a, v, nv, idx, n * n * 2);
}

static const ConvexHull* g_rock;

/* sahne 0: rastgele düşen kutu yığını, 1: piramit, 2: karışık şekiller + mesh + eklemler */
static void build(PhysWorld* w, u32 scene, const TriMesh* mesh) {
    Rng r = rng_make(321 + scene);
    if (scene == 2) {
        BodyDesc m = phys_desc_mesh(v3(0, 0, 0), mesh);
        phys_add_body(w, &m);
    } else {
        BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
        phys_add_body(w, &g);
    }
    if (scene == 0) {
        for (u32 i = 0; i < 120; i++) {
            BodyDesc b = phys_desc_box(v3(rng_range(&r, -2, 2), 0.6f + (f32)i * 0.35f, rng_range(&r, -2, 2)), v3(0.4f, 0.3f, 0.5f), 1.0f);
            b.rot = quat_from_axis_angle(v3(rng_range(&r, -1, 1), 1, rng_range(&r, -1, 1)), rng_range(&r, -1, 1));
            phys_add_body(w, &b);
        }
    } else if (scene == 1) {
        for (u32 l = 0; l < 8; l++) {
            for (u32 iz = 0; iz < 8 - l; iz++) {
                for (u32 ix = 0; ix < 8 - l; ix++) {
                    f32 o = (f32)(7 - l) * 0.5f;
                    BodyDesc b = phys_desc_box(v3(((f32)ix - o) * 1.01f, 0.5f + (f32)l, ((f32)iz - o) * 1.01f), v3(0.5f, 0.5f, 0.5f), 1.0f);
                    phys_add_body(w, &b);
                }
            }
        }
        BodyDesc ball = phys_desc_sphere(v3(-12, 4, 0), 0.6f, 20.0f);
        ball.vel = v3(14, 0, 0);
        phys_add_body(w, &ball);
    } else {
        for (u32 i = 0; i < 60; i++) {
            Vec3 p = v3(rng_range(&r, -5, 5), 2.0f + (f32)i * 0.4f, rng_range(&r, -5, 5));
            BodyDesc d;
            if (i % 4 == 0) d = phys_desc_capsule(p, 0.25f, 0.4f, 1.0f);
            else if (i % 4 == 1) d = phys_desc_hull(p, g_rock, 2.0f);
            else if (i % 4 == 2) d = phys_desc_sphere(p, 0.35f, 1.0f);
            else d = phys_desc_box(p, v3(0.3f, 0.4f, 0.3f), 1.0f);
            d.rot = quat_from_axis_angle(v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), 1), rng_range(&r, -2, 2));
            phys_add_body(w, &d);
        }
        u32 prev = PHYS_NO_BODY;
        for (u32 i = 0; i < 6; i++) {
            BodyDesc c = phys_desc_box(v3(-3.0f + (f32)i * 0.6f, 5.0f, 6.0f), v3(0.28f, 0.1f, 0.2f), 1.0f);
            u32 id = phys_add_body(w, &c);
            JointDesc j = phys_joint_desc(JOINT_BALL, prev, id, v3(-3.3f + (f32)i * 0.6f, 5.0f, 6.0f));
            phys_add_joint(w, &j);
            prev = id;
        }
    }
}

/* SIMD ile skaler renk sırası arasındaki ilk farklı adım (0 = hiç) */
static u32 first_mismatch(u32 scene, b32 warm, b32 sleep, u32 steps, u32* colors, u32* overflow) {
    const TriMesh* mesh = scene == 2 ? bumpy() : NULL;
    PhysWorld* a = world(512);
    PhysWorld* b = world(512);
    a->simd = FALSE;
    b->simd = TRUE;
    a->warm_start = b->warm_start = warm;
    a->allow_sleep = b->allow_sleep = sleep;
    build(a, scene, mesh);
    build(b, scene, mesh);
    *colors = *overflow = 0;
    for (u32 s = 1; s <= steps; s++) {
        phys_step(a, DT);
        phys_step(b, DT);
        *colors = a->stats.colors > *colors ? a->stats.colors : *colors;
        *overflow = a->stats.overflow > *overflow ? a->stats.overflow : *overflow;
        if (phys_state_hash(a) != phys_state_hash(b)) return s;
        if (b->wide_count == 0 && b->cur_count > 8) return 0xFFFFFFFFu;   /* SIMD yolu çalışmadı */
    }
    return 0;
}

TEST(phys_simd_bit_exact) {
    Vec3 pts[12];
    Rng r = rng_make(4);
    for (u32 i = 0; i < 12; i++) pts[i] = v3(rng_range(&r, -0.5f, 0.5f), rng_range(&r, -0.4f, 0.4f), rng_range(&r, -0.5f, 0.5f));
    g_rock = hull_create(test_arena(), pts, 12);
    CHECK(g_rock != NULL);
    static const char* const NAMES[4] = { "yığın", "piramit + top", "karışık + mesh + eklem", "yığın, warm start yok + uyku" };
    for (u32 c = 0; c < 4; c++) {
        u32 colors, overflow;
        u32 scene = c == 3 ? 0 : c;
        u32 bad = first_mismatch(scene, c != 3, c == 3, 240, &colors, &overflow);
        test_note(NAMES[c], bad, 0);
        test_note("  en çok renk", colors, 0);
        test_note("  en çok taşma manifoldu", overflow, 0);
        CHECK(bad == 0);
    }
}

TEST(phys_graph_coloring_valid) {
    PhysWorld* w = world(512);
    build(w, 1, NULL);
    for (u32 s = 0; s < 30; s++) phys_step(w, DT);
    u32 bad = 0;
    for (u32 c = 0; c < PHYS_COLORS; c++) {
        for (u32 i = w->color_start[c]; i < w->color_start[c + 1]; i++) {
            const Manifold* mi = &w->cur[w->color_order[i]];
            for (u32 j = i + 1; j < w->color_start[c + 1]; j++) {
                const Manifold* mj = &w->cur[w->color_order[j]];
                const u32 bi[2] = { mi->a, mi->b }, bj[2] = { mj->a, mj->b };
                for (u32 p = 0; p < 2; p++) {
                    for (u32 q = 0; q < 2; q++) {
                        if (bi[p] == bj[q] && w->bodies[bi[p]].inv_mass > 0.0f) bad++;
                    }
                }
            }
        }
    }
    /* her manifold tam bir kez sıralamada */
    u32 seen = 0;
    for (u32 i = 0; i < w->color_start[PHYS_COLORS + 1]; i++) seen += w->color_order[i] < w->cur_count;
    test_note("manifold", w->cur_count, 0);
    test_note("renk", w->stats.colors, 0);
    test_note("aynı renkte ortak dinamik gövde", bad, 0);
    CHECK(bad == 0);
    CHECK(seen == w->cur_count && w->color_start[PHYS_COLORS + 1] == w->cur_count);
}
