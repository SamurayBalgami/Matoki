/* phys/phys_bench.c — fizik ölçüm sahneleri (tools/bench_phys.mjs ve testler kullanır).
 * Sahne 0 "yığın": n kutu, ızgarada hafif rastgele döndürülmüş, zemine düşer (çok temaslı yığın).
 * Sahne 1 "piramit": 3B basamaklı piramit (≈ n kutu) — uzun kısıt zinciri, kararlılık testi.
 * Sahne 2 "duvar": 1 kutu kalınlığında 2B piramit duvar; yükseldikçe kalınlık yönünde sınırda kararlı.
 * Her koşu 600 adım (10 s @ 60 Hz); ölçümler g_phys_bench[] dizisine yazılır. Belirleyici. */
#include "phys/phys.h"
#include "core/rng.h"
#include "core/sys.h"

enum {
    PB_MS_ACTIVE,      /* adım 0..299 ortalama ms (düşüş, çarpışma) */
    PB_MS_SETTLED,     /* adım 300..599 ortalama ms */
    PB_COLLIDE_MS,     /* son 300 adım: çarpışma ortalaması */
    PB_SOLVE_MS,       /* son 300 adım: çözücü ortalaması */
    PB_CONTACTS,       /* son adım */
    PB_AWAKE,          /* son adım */
    PB_KE,             /* son adım kinetik enerji (J) */
    PB_MAX_DISP,       /* duvar: başlangıca göre en büyük yer değiştirme (m) */
    PB_MAX_PEN,        /* son adım en büyük girişim (m) */
    PB_HASH,
    PB_BROAD_MS,       /* son 300 adım: geniş faz ortalaması */
    PB_COUNT
};

static f64 g_phys_bench[PB_COUNT];
static Arena g_bench_arena;
static b32 g_bench_ready;
static u32 g_bench_substeps = 4, g_bench_iterations = 10;
static b32 g_bench_ccd = TRUE;
static i32 g_bench_order = 3;
static i32 g_bench_bp = PHYS_BP_BVH;
static PhysWorld* g_last;   /* phys_bench_rays için son koşunun dünyası */

/* soft alt adım ve SI iterasyon sayısı (0 = değiştirme); ccd: 0 değiştirme, 1 açık, 2 kapalı;
 * order (soft): 0 değiştirme, 1 oluşturulma sırası (Faz 1–5), 2 renk sırası skaler, 3 renk sırası SIMD */
EXPORT(phys_bench_config) void phys_bench_config(i32 substeps, i32 iterations, i32 ccd, i32 order, i32 bp) {
    if (substeps > 0) g_bench_substeps = (u32)substeps;
    if (iterations > 0) g_bench_iterations = (u32)iterations;
    if (ccd) g_bench_ccd = ccd == 1;
    if (order >= 1 && order <= 3) g_bench_order = order;
    if (bp == 1) g_bench_bp = PHYS_BP_SAP;   /* bp: 0 değiştirme, 1 SAP + doğrusal sorgular, 2 BVH */
    if (bp == 2) g_bench_bp = PHYS_BP_BVH;
}

/* L katlı 3B piramitteki kutu sayısı: Σ k² */
static u32 true_count(u32 L) { return L * (L + 1) * (2 * L + 1) / 6; }

static u32 build_scene(PhysWorld* w, i32 scene, i32 n) {
    BodyDesc ground = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &ground);
    Rng r = rng_make(12345);
    if (scene == 0) {
        u32 side = 1;
        while (side * side * side < (u32)n) side++;
        u32 k = 0;
        for (u32 y = 0; y < side && k < (u32)n; y++) {
            for (u32 z = 0; z < side && k < (u32)n; z++) {
                for (u32 x = 0; x < side && k < (u32)n; x++, k++) {
                    Vec3 p = v3(((f32)x - (f32)side * 0.5f) * 1.3f + rng_range(&r, -0.1f, 0.1f),
                                0.7f + (f32)y * 1.3f,
                                ((f32)z - (f32)side * 0.5f) * 1.3f + rng_range(&r, -0.1f, 0.1f));
                    BodyDesc b = phys_desc_box(p, v3(0.5f, 0.5f, 0.5f), 1.0f);
                    b.rot = quat_from_axis_angle(v3(rng_range(&r, -1, 1), 1.0f, rng_range(&r, -1, 1)), rng_range(&r, -0.4f, 0.4f));
                    phys_add_body(w, &b);
                }
            }
        }
    } else if (scene == 1) {
        /* 3B basamaklı piramit: kat l'de (L-l)² kutu */
        u32 levels = 1;
        while (true_count(levels + 1) <= (u32)n) levels++;
        for (u32 l = 0; l < levels; l++) {
            u32 cnt = levels - l;
            for (u32 iz = 0; iz < cnt; iz++) {
                for (u32 ix = 0; ix < cnt; ix++) {
                    f32 x = ((f32)ix - (f32)(cnt - 1) * 0.5f) * 1.02f;
                    f32 z = ((f32)iz - (f32)(cnt - 1) * 0.5f) * 1.02f;
                    BodyDesc b = phys_desc_box(v3(x, 0.5f + (f32)l, z), v3(0.5f, 0.5f, 0.5f), 1.0f);
                    phys_add_body(w, &b);
                }
            }
        }
    } else if (scene == 3) {
        /* şehir: n statik bina (3×3 m taban, 4 m aralık, 2–20 m), n/10 düşen kutu, 16 mermi (CCD) */
        u32 side = 1;
        while (side * side < (u32)n) side++;
        f32 half = (f32)side * 2.0f;
        for (u32 k = 0; k < (u32)n; k++) {
            f32 h = rng_range(&r, 2.0f, 20.0f);
            BodyDesc s = phys_desc_box(v3((f32)(k % side) * 4.0f - half, h * 0.5f, (f32)(k / side) * 4.0f - half), v3(1.5f, h * 0.5f, 1.5f), 0.0f);
            phys_add_body(w, &s);
        }
        for (u32 k = 0; k < (u32)n / 10; k++) {
            BodyDesc b = phys_desc_box(v3(rng_range(&r, -half, half), 22.0f + rng_range(&r, 0, 10), rng_range(&r, -half, half)),
                                       v3(0.4f, 0.4f, 0.4f), 1.0f);
            b.rot = quat_from_axis_angle(v3(rng_range(&r, -1, 1), 1, rng_range(&r, -1, 1)), rng_range(&r, -1, 1));
            phys_add_body(w, &b);
        }
        for (u32 k = 0; k < 16; k++) {
            BodyDesc s = phys_desc_sphere(v3(-half - 5.0f, 3.0f + (f32)k, rng_range(&r, -half, half)), 0.1f, 0.2f);
            s.vel = v3(200.0f, 0, rng_range(&r, -20, 20));
            u32 id = phys_add_body(w, &s);
            w->bodies[id].flags |= BODY_BULLET;
        }
    } else {
        u32 levels = 1;
        while ((levels + 1) * (levels + 2) / 2 <= (u32)n) levels++;
        for (u32 l = 0; l < levels; l++) {
            u32 cnt = levels - l;
            for (u32 i = 0; i < cnt; i++) {
                f32 x = ((f32)i - (f32)(cnt - 1) * 0.5f) * 1.02f;
                BodyDesc b = phys_desc_box(v3(x, 0.5f + (f32)l, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
                phys_add_body(w, &b);
            }
        }
    }
    return w->count;
}

EXPORT(phys_bench_run) f64* phys_bench_run(i32 scene, i32 n, i32 solver, i32 allow_sleep) {
    if (!g_bench_ready) {
        if (!arena_init_sys(&g_bench_arena, (usize)96 << 20)) return NULL;
        g_bench_ready = TRUE;
    }
    arena_reset(&g_bench_arena, 0);
    PhysWorld* w = ARENA_NEW(&g_bench_arena, PhysWorld);
    /* şehir çoğunlukla statik: manifold kapasitesi düşen kutulara göre */
    u32 manifolds = scene == 3 ? (u32)n * 2 + 4096 : (u32)n * 12 + 64;
    phys_world_init(w, &g_bench_arena, (u32)n + (u32)n / 8 + 64, manifolds);
    w->broadphase = (PhysBroadphase)g_bench_bp;
    g_last = w;
    phys_world_set_solver(w, solver);
    w->substeps = g_bench_substeps;
    w->iterations = g_bench_iterations;
    w->allow_sleep = allow_sleep != 0;
    w->continuous = g_bench_ccd;
    w->graph_color = g_bench_order >= 2;
    w->simd = g_bench_order == 3;
    build_scene(w, scene, n);
    Vec3* start = ARENA_ARRAY(&g_bench_arena, Vec3, w->count);
    for (u32 i = 0; i < w->count; i++) start[i] = w->bodies[i].pos;

    f64 a = 0, s = 0, c = 0, v = 0, broad = 0;
    const f32 dt = 1.0f / 60.0f;
    for (u32 step = 0; step < 600; step++) {
        phys_step(w, dt);
        if (step < 300) {
            a += w->stats.step_ms;
        } else {
            s += w->stats.step_ms;
            c += w->stats.collide_ms;
            v += w->stats.solve_ms;
            broad += w->stats.broad_ms;
        }
    }
    f32 disp = 0.0f;
    for (u32 i = 0; i < w->count; i++) {
        if (w->bodies[i].shape == SHAPE_PLANE) continue;
        disp = m_maxf(disp, v3_len(v3_sub(w->bodies[i].pos, start[i])));
    }
    g_phys_bench[PB_MS_ACTIVE] = a / 300.0;
    g_phys_bench[PB_MS_SETTLED] = s / 300.0;
    g_phys_bench[PB_COLLIDE_MS] = c / 300.0;
    g_phys_bench[PB_SOLVE_MS] = v / 300.0;
    g_phys_bench[PB_CONTACTS] = w->stats.contacts;
    g_phys_bench[PB_AWAKE] = w->stats.awake;
    g_phys_bench[PB_KE] = phys_kinetic_energy(w);
    g_phys_bench[PB_MAX_DISP] = disp;
    g_phys_bench[PB_MAX_PEN] = phys_max_penetration(w);
    g_phys_bench[PB_HASH] = (f64)phys_state_hash(w);
    g_phys_bench[PB_BROAD_MS] = broad / 300.0;
    return g_phys_bench;
}

/* son koşunun dünyasında count rastgele ışın (belirleyici tohum): [ms, isabet, isabet t toplamı] */
EXPORT(phys_bench_rays) f64* phys_bench_rays(i32 count) {
    static f64 out[3];
    if (!g_last) return out;
    Vec3 mn = v3(3.4e38f, 3.4e38f, 3.4e38f), mx = v3(-3.4e38f, -3.4e38f, -3.4e38f);
    for (u32 i = 0; i < g_last->count; i++) {
        const Body* b = &g_last->bodies[i];
        if (b->shape == SHAPE_PLANE) continue;
        mn = v3(m_minf(mn.x, b->aabb_min.x), m_minf(mn.y, b->aabb_min.y), m_minf(mn.z, b->aabb_min.z));
        mx = v3(m_maxf(mx.x, b->aabb_max.x), m_maxf(mx.y, b->aabb_max.y), m_maxf(mx.z, b->aabb_max.z));
    }
    Rng r = rng_make(99);
    u32 hits = 0;
    f64 tsum = 0.0;
    f64 t0 = sys_now_ms();
    for (i32 k = 0; k < count; k++) {
        Vec3 o = v3(rng_range(&r, mn.x, mx.x), rng_range(&r, 1.0f, 25.0f), rng_range(&r, mn.z, mx.z));
        Vec3 d = v3_norm(v3(rng_range(&r, -1, 1), rng_range(&r, -0.6f, 0.2f), rng_range(&r, -1, 1)));
        RayHit h;
        if (phys_raycast(g_last, o, d, 60.0f, PHYS_NO_BODY, &h)) {
            hits++;
            tsum += h.t;
        }
    }
    out[0] = sys_now_ms() - t0;
    out[1] = hits;
    out[2] = tsum;
    return out;
}
