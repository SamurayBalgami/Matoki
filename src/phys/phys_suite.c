/* phys/phys_suite.c — fizik doğruluk düzeneği (D59): Box2D / PhysX'in kendi testlerindeki türden zor sahneler, her biri ölçütler ve
 * açık bir geçme eşiğiyle. Analitik cevabı olanlar (sürtünme rampası, esneklik, enerji, açısal momentum) fizik yasasıyla karşılaştırılır;
 * olmayanlar (yığın, kütle oranı, girişim, mermi) bozulma ölçüsüyle. Varsayılan dünya (yumuşak çözücü, 60 Hz, 4 alt adım), uyku kapalı:
 * motorun kendisi ölçülür, uykunun gizlediği titreme değil. Belirleyici. Sonuç: [geçti, ölçüt0..3, ms/adım] (tools/phys_suite.mjs). */
#include "phys/phys.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "core/quat.h"
#include "core/sys.h"

#define SUITE_METRICS 4
#define DT (1.0f / 60.0f)

typedef struct SuiteInfo {
    const char* name;
    const char* what;
    const char* metric[SUITE_METRICS];
    const char* rule;
} SuiteInfo;

enum {
    ST_STACK, ST_PYRAMID, ST_MASS_RATIO, ST_HEAVY_CHAIN, ST_FRICTION, ST_RESTITUTION, ST_OVERLAP, ST_DOMINO,
    ST_BULLET, ST_PENDULUM, ST_GYRO, ST_DETERMINISM, ST_LARGE, ST_COUNT
};

static const SuiteInfo SUITE[ST_COUNT] = {
    { "dikey yığın", "20 kutu üst üste, 10 s", { "yatay kayma (m)", "tepe yükseklik kaybı (m)", "en büyük girişim (m)", "" },
      "kayma < 0,02 ve kayıp < 0,05" },
    { "piramit", "taban 20, 210 kutu duvar, 10 s", { "en büyük yer değ. (m)", "en büyük girişim (m)", "KE son (J)", "" },
      "yer değ. < 0,05" },
    { "kütle oranı", "4 hafif kutu (1 kg) üstünde 1000 kg kutu, 10 s", { "tepe batması (m)", "en büyük girişim (m)", "yatay kayma (m)", "" },
      "batma < 0,05 ve kayma < 0,02" },
    { "ağır sarkaç zinciri", "20 halka 0,1 kg + uçta 100 kg (oran 1000), yataydan bırakılır, 10 s",
      { "en büyük eklem hatası (m)", "zincir uzaması (%)", "", "" }, "eklem hatası < 0,02" },
    { "sürtünme rampası", "30° rampa, μ = 0,1 … 0,9 (kayma sınırı tan 30° = 0,577)",
      { "doğru kayan/duran (9)", "kayan ivme hatası (%)", "duranın sürünmesi (m)", "" }, "9/9, ivme hatası < 5, sürünme < 0,01" },
    { "esneklik", "e = 0 … 1 küreler 2 m'den; ilk sekme / h₀ = e²",
      { "en büyük |h/h₀ − e²|", "e = 1 sekme oranı", "e = 0,5 sekme oranı", "" }, "hata < 0,05" },
    { "girişim kurtarma", "4 katlı piramit, kutular %25 iç içe doğar, 3 s",
      { "en büyük hız (m/s)", "son girişim (m)", "son KE (J)", "" }, "hız < 3 ve girişim < 0,01" },
    { "domino", "40 taş (0,6 m aralık), ilki itilir, 20 s", { "devrilen (40)", "sonuncunun düştüğü an (s)", "dalga hızı (m/s)", "" }, "40/40" },
    { "mermi", "r = 5 cm küre 50 … 800 m/s; 10 cm duvar (statik) ve 10 cm kutu (dinamik, BULLET)",
      { "statik duvardan geçen (5)", "dinamik kutudan geçen (5)", "", "" }, "0 ve 0" },
    { "sarkaç enerjisi", "1 m ip, 1 kg, yataydan, sürtünmesiz, 10 s", { "enerji sapması (% mgL)", "en büyük ip hatası (m)", "", "" },
      "sapma < 2" },
    { "açısal momentum", "çekimsiz kutu, ara eksene yakın dönme (Dzhanibekov: takla atmalı), 10 s",
      { "|ΔL| / |L| (%)", "enerji sapması (%)", "takla sayısı", "" }, "|ΔL| < 1, enerji < 1, takla ≥ 2" },
    { "belirlenimcilik", "piramit iki kez + SIMD kapalı", { "iki koşu aynı (1/0)", "SIMD = skaler (1/0)", "", "" }, "1 ve 1" },
    { "büyük yığın (hız)", "2000 kutu düşer, 10 s", { "ms/adım aktif", "ms/adım oturmuş", "temas", "en büyük girişim (m)" }, "bilgi" },
};

static Arena g_suite_arena;
static b32 g_suite_ready;
static f64 g_out[2 + SUITE_METRICS];

EXPORT(phys_suite_count) i32 phys_suite_count(void) { return ST_COUNT; }
/* k: 0 ad, 1 açıklama, 2..5 ölçüt adları, 6 eşik */
EXPORT(phys_suite_text) const char* phys_suite_text(i32 id, i32 k) {
    if (id < 0 || id >= ST_COUNT) return "";
    const SuiteInfo* s = &SUITE[id];
    if (k == 0) return s->name;
    if (k == 1) return s->what;
    if (k >= 2 && k < 2 + SUITE_METRICS) return s->metric[k - 2];
    return k == 2 + SUITE_METRICS ? s->rule : "";
}

static PhysWorld* world_new(u32 bodies, u32 manifolds) {
    PhysWorld* w = ARENA_NEW(&g_suite_arena, PhysWorld);
    phys_world_init(w, &g_suite_arena, bodies, manifolds);
    w->allow_sleep = FALSE;
    return w;
}

static u32 add_ground(PhysWorld* w, f32 friction, f32 restitution) {
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    g.friction = friction;
    g.restitution = restitution;
    return phys_add_body(w, &g);
}

static f64 g_step_ms;
static u32 g_steps;
static void step(PhysWorld* w) {
    phys_step(w, DT);
    g_step_ms += w->stats.step_ms;
    g_steps++;
}

static f32 hdist(Vec3 a, Vec3 b) {
    f32 dx = a.x - b.x, dz = a.z - b.z;
    return m_sqrtf(dx * dx + dz * dz);
}

/* ---- sahneler ---- */

static void t_stack(f64* m) {
    PhysWorld* w = world_new(32, 256);
    add_ground(w, 0.6f, 0.0f);
    u32 first = w->count;
    for (u32 i = 0; i < 20; i++) {
        BodyDesc b = phys_desc_box(v3(0, 0.5f + (f32)i, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
        phys_add_body(w, &b);
    }
    f32 drift = 0, pen = 0;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        pen = m_maxf(pen, phys_max_penetration(w));
        for (u32 i = 0; i < 20; i++) drift = m_maxf(drift, hdist(w->bodies[first + i].pos, v3(0, 0, 0)));
    }
    f32 loss = 19.5f - w->bodies[first + 19].pos.y;
    m[0] = drift; m[1] = loss; m[2] = pen;
    m[-1] = drift < 0.02f && loss < 0.05f;
}

static u32 build_wall(PhysWorld* w, u32 base, Vec3* start) {
    u32 n = 0;
    for (u32 l = 0; l < base; l++) {
        u32 cnt = base - l;
        for (u32 i = 0; i < cnt; i++) {
            BodyDesc b = phys_desc_box(v3(((f32)i - (f32)(cnt - 1) * 0.5f) * 1.02f, 0.5f + (f32)l, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
            u32 id = phys_add_body(w, &b);
            if (start) start[n] = w->bodies[id].pos;
            n++;
        }
    }
    return n;
}

static void t_pyramid(f64* m) {
    PhysWorld* w = world_new(256, 256 * 8);
    add_ground(w, 0.6f, 0.0f);
    Vec3 start[210];
    u32 first = w->count;
    u32 n = build_wall(w, 20, start);
    f32 pen = 0;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        pen = m_maxf(pen, phys_max_penetration(w));
    }
    f32 disp = 0;
    for (u32 i = 0; i < n; i++) disp = m_maxf(disp, v3_len(v3_sub(w->bodies[first + i].pos, start[i])));
    m[0] = disp; m[1] = pen; m[2] = phys_kinetic_energy(w);
    m[-1] = disp < 0.05f;
}

static void t_mass_ratio(f64* m) {
    PhysWorld* w = world_new(16, 128);
    add_ground(w, 0.6f, 0.0f);
    for (u32 i = 0; i < 4; i++) {
        BodyDesc b = phys_desc_box(v3(0, 0.25f + 0.5f * (f32)i, 0), v3(0.25f, 0.25f, 0.25f), 1.0f);
        phys_add_body(w, &b);
    }
    BodyDesc top = phys_desc_box(v3(0, 2.0f + 0.5f, 0), v3(0.5f, 0.5f, 0.5f), 1000.0f);
    u32 t = phys_add_body(w, &top);
    f32 pen = 0, drift = 0;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        pen = m_maxf(pen, phys_max_penetration(w));
        for (u32 i = 1; i < w->count; i++) drift = m_maxf(drift, hdist(w->bodies[i].pos, v3(0, 0, 0)));
    }
    f32 sink = 2.5f - w->bodies[t].pos.y;
    m[0] = sink; m[1] = pen; m[2] = drift;
    m[-1] = sink < 0.05f && drift < 0.02f;
}

static void t_heavy_chain(f64* m) {
    PhysWorld* w = world_new(32, 256);
    const u32 links = 20;
    const f32 gap = 0.25f;
    Vec3 anchor = v3(0, 12.0f, 0);
    u32 prev = PHYS_NO_BODY;
    for (u32 i = 0; i <= links; i++) {
        b32 last = i == links;
        /* halka i, önceki halkanın merkezinden asılı; ağırlık (r = 0,5) son halkadan 0,75 m ötede: halkalarla çakışmaz */
        f32 x = last ? gap * (f32)links + 0.75f : gap * (f32)(i + 1);
        BodyDesc b = phys_desc_sphere(v3(x, anchor.y, 0), last ? 0.5f : 0.1f, last ? 100.0f : 0.1f);
        u32 id = phys_add_body(w, &b);
        JointDesc j = phys_joint_desc(JOINT_BALL, id, prev, v3(gap * (f32)i, anchor.y, 0));
        phys_add_joint(w, &j);
        prev = id;
    }
    f32 err = 0, stretch = 0;
    f32 rest = gap * (f32)links + 0.75f;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        for (u32 j = 0; j < w->joint_count; j++) err = m_maxf(err, phys_joint_error(w, j));
        stretch = m_maxf(stretch, v3_len(v3_sub(w->bodies[w->count - 1].pos, anchor)) / rest - 1.0f);
    }
    m[0] = err; m[1] = stretch * 100.0f;
    m[-1] = err < 0.02f;
}

static void t_friction(f64* m) {
    PhysWorld* w = world_new(16, 128);
    const f32 th = 30.0f * 3.14159265f / 180.0f;
    Quat rot = quat_from_axis_angle(v3(0, 0, 1), -th);
    Vec3 n = quat_rotate(rot, v3(0, 1, 0)), down = quat_rotate(rot, v3(1, 0, 0));
    BodyDesc ramp = phys_desc_box(v3(0, 0, 0), v3(15.0f, 0.5f, 12.0f), 0.0f);
    ramp.rot = rot;
    ramp.friction = 1.0f;   /* karışım √(μa·μb): kutuya μ² verilir, temas μ olur */
    phys_add_body(w, &ramp);
    u32 first = w->count;
    for (u32 k = 0; k < 9; k++) {
        f32 mu = 0.1f * (f32)(k + 1);
        Vec3 p = v3_add(v3_add(v3_scale(n, 1.0f), v3_scale(down, -12.0f)), v3(0, 0, -10.0f + 2.5f * (f32)k));
        BodyDesc b = phys_desc_box(p, v3(0.5f, 0.5f, 0.5f), 1.0f);
        b.rot = rot;
        b.friction = mu * mu;
        b.restitution = 0.0f;
        phys_add_body(w, &b);
    }
    Vec3 p0[9], pa[9], pb[9];
    for (u32 k = 0; k < 9; k++) p0[k] = w->bodies[first + k].pos;
    for (u32 s = 0; s < 120; s++) {
        step(w);
        if (s + 1 == 30) for (u32 k = 0; k < 9; k++) pa[k] = w->bodies[first + k].pos;
        if (s + 1 == 90) for (u32 k = 0; k < 9; k++) pb[k] = w->bodies[first + k].pos;
    }
    u32 right = 0;
    f32 acc_err = 0, creep = 0;
    for (u32 k = 0; k < 9; k++) {
        f32 mu = 0.1f * (f32)(k + 1);
        f32 d = v3_dot(v3_sub(w->bodies[first + k].pos, p0[k]), down);
        b32 slides = mu < m_tanf(th);
        b32 slid = d > 0.5f;
        if (slides == slid) right++;
        if (slides) {
            /* t = 0,5 → 1,5 s: d = a/2 · (1,5² − 0,5²) = a */
            f32 a = v3_dot(v3_sub(pb[k], pa[k]), down);
            f32 want = 9.81f * (m_sinf(th) - mu * m_cosf(th));
            acc_err = m_maxf(acc_err, m_absf(a - want) / want);
        } else {
            creep = m_maxf(creep, m_absf(d));
        }
    }
    m[0] = right; m[1] = acc_err * 100.0f; m[2] = creep;
    m[-1] = right == 9 && acc_err < 0.05f && creep < 0.01f;
}

static void t_restitution(f64* m) {
    PhysWorld* w = world_new(16, 64);
    add_ground(w, 0.6f, 0.0f);
    const f32 r = 0.25f, h0 = 2.0f;
    u32 first = w->count;
    for (u32 k = 0; k < 5; k++) {
        BodyDesc b = phys_desc_sphere(v3(1.5f * (f32)k, r + h0, 0), r, 1.0f);
        b.restitution = 0.25f * (f32)k;
        b.rolling = 0.0f;
        phys_add_body(w, &b);
    }
    f32 apex[5] = { 0 };
    u32 phase[5] = { 0 };   /* 0 düşüyor, 1 sekti (yükseliyor), 2 bitti */
    for (u32 s = 0; s < 240; s++) {
        step(w);
        for (u32 k = 0; k < 5; k++) {
            const Body* b = &w->bodies[first + k];
            if (phase[k] == 0 && b->vel.y > 0.0f && b->pos.y < r + 0.5f) phase[k] = 1;
            if (phase[k] == 1) {
                apex[k] = m_maxf(apex[k], b->pos.y - r);
                if (b->vel.y < 0.0f) phase[k] = 2;
            }
        }
    }
    f32 err = 0;
    for (u32 k = 0; k < 5; k++) {
        f32 e = 0.25f * (f32)k;
        err = m_maxf(err, m_absf(apex[k] / h0 - e * e));
    }
    m[0] = err; m[1] = apex[4] / h0; m[2] = apex[2] / h0;
    m[-1] = err < 0.05f;
}

static void t_overlap(f64* m) {
    PhysWorld* w = world_new(32, 256);
    add_ground(w, 0.6f, 0.0f);
    const f32 s = 0.75f;   /* 1 m kutular 0,75 m aralıkla: %25 iç içe */
    for (u32 l = 0; l < 4; l++) {
        for (u32 i = 0; i < 4 - l; i++) {
            BodyDesc b = phys_desc_box(v3(((f32)i - (f32)(3 - l) * 0.5f) * s, 0.5f + (f32)l * s, 0), v3(0.5f, 0.5f, 0.5f), 1.0f);
            phys_add_body(w, &b);
        }
    }
    f32 vmax = 0;
    for (u32 st = 0; st < 180; st++) {
        step(w);
        for (u32 i = 1; i < w->count; i++) vmax = m_maxf(vmax, v3_len(w->bodies[i].vel));
    }
    f32 pen = phys_max_penetration(w);
    m[0] = vmax; m[1] = pen; m[2] = phys_kinetic_energy(w);
    m[-1] = vmax < 3.0f && pen < 0.01f;
}

static void t_domino(f64* m) {
    PhysWorld* w = world_new(64, 512);
    add_ground(w, 0.6f, 0.0f);
    u32 first = w->count;
    for (u32 i = 0; i < 40; i++) {
        BodyDesc b = phys_desc_box(v3(0.6f * (f32)i, 0.5f, 0), v3(0.05f, 0.5f, 0.25f), 0.5f);
        phys_add_body(w, &b);
    }
    w->bodies[first].ang_vel = v3(0, 0, -2.0f);
    f32 t_last = -1.0f;
    u32 fallen = 0;
    for (u32 s = 0; s < 1200; s++) {
        step(w);
        u32 f = 0;
        for (u32 i = 0; i < 40; i++) f += quat_rotate(w->bodies[first + i].rot, v3(0, 1, 0)).y < 0.7071f;
        if (f == 40 && t_last < 0.0f) t_last = (f32)(s + 1) * DT;
        fallen = f;
    }
    m[0] = fallen; m[1] = t_last; m[2] = t_last > 0.0f ? 0.6f * 39.0f / t_last : 0.0f;
    m[-1] = fallen == 40;
}

static void t_bullet(f64* m) {
    static const f32 SPEED[5] = { 50, 100, 200, 400, 800 };
    u32 through_static = 0, through_dyn = 0;
    for (u32 k = 0; k < 5; k++) {
        for (u32 dyn = 0; dyn < 2; dyn++) {
            arena_reset(&g_suite_arena, 0);
            PhysWorld* w = world_new(8, 64);
            add_ground(w, 0.6f, 0.0f);
            BodyDesc wall = phys_desc_box(v3(10.0f, 1.0f, 0), v3(0.05f, 1.0f, 1.0f), dyn ? 1.0f : 0.0f);
            phys_add_body(w, &wall);
            BodyDesc b = phys_desc_sphere(v3(0, 1.0f, 0), 0.05f, 0.1f);
            b.vel = v3(SPEED[k], 0, 0);
            u32 id = phys_add_body(w, &b);
            if (dyn) w->bodies[id].flags |= BODY_BULLET;
            for (u32 s = 0; s < 60; s++) step(w);
            if (w->bodies[id].pos.x > 10.0f) {
                if (dyn) through_dyn++;
                else through_static++;
            }
        }
    }
    m[0] = through_static; m[1] = through_dyn;
    m[-1] = through_static == 0 && through_dyn == 0;
}

static f64 pend_energy(const Body* b, f64 mass) {
    f64 v2 = (f64)v3_dot(b->vel, b->vel);
    return 0.5 * mass * v2 + mass * 9.81 * (f64)b->pos.y;
}

static void t_pendulum(f64* m) {
    PhysWorld* w = world_new(4, 16);
    Vec3 pivot = v3(0, 5.0f, 0);
    BodyDesc b = phys_desc_sphere(v3(1.0f, 5.0f, 0), 0.05f, 1.0f);
    u32 id = phys_add_body(w, &b);
    JointDesc j = phys_joint_desc(JOINT_DISTANCE, id, PHYS_NO_BODY, v3(1.0f, 5.0f, 0));
    j.anchor_b = pivot;
    j.min_length = j.max_length = 1.0f;
    phys_add_joint(w, &j);
    f64 e0 = pend_energy(&w->bodies[id], 1.0), dev = 0;
    f32 err = 0;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        dev = dev > m_absf((f32)(pend_energy(&w->bodies[id], 1.0) - e0)) ? dev : m_absf((f32)(pend_energy(&w->bodies[id], 1.0) - e0));
        err = m_maxf(err, m_absf(v3_len(v3_sub(w->bodies[id].pos, pivot)) - 1.0f));
    }
    f64 pct = dev / (1.0 * 9.81 * 1.0) * 100.0;
    m[0] = pct; m[1] = err;
    m[-1] = pct < 2.0;
}

static Vec3 ang_momentum(const Body* b) {
    Quat inv = quat(-b->rot.x, -b->rot.y, -b->rot.z, b->rot.w);
    Vec3 wl = quat_rotate(inv, b->ang_vel);
    Vec3 il = b->inv_inertia_local;
    Vec3 ll = v3(wl.x / il.x, wl.y / il.y, wl.z / il.z);
    return quat_rotate(b->rot, ll);
}

static f32 rot_energy(const Body* b) {
    Quat inv = quat(-b->rot.x, -b->rot.y, -b->rot.z, b->rot.w);
    Vec3 wl = quat_rotate(inv, b->ang_vel);
    Vec3 il = b->inv_inertia_local;
    return 0.5f * (wl.x * wl.x / il.x + wl.y * wl.y / il.y + wl.z * wl.z / il.z);
}

static void t_gyro(f64* m) {
    PhysWorld* w = world_new(4, 16);
    w->gravity = v3(0, 0, 0);
    BodyDesc b = phys_desc_box(v3(0, 10.0f, 0), v3(0.1f, 0.3f, 0.6f), 1.0f);
    /* ara eksen (y) etrafında, küçük sapmayla: kararsız dönme, takla atar */
    b.ang_vel = v3(0.3f, 6.0f, 0.2f);
    u32 id = phys_add_body(w, &b);
    Vec3 l0 = ang_momentum(&w->bodies[id]);
    f32 e0 = rot_energy(&w->bodies[id]);
    f32 dl = 0, de = 0;
    u32 flips = 0;
    f32 prev = quat_rotate(w->bodies[id].rot, v3(0, 1, 0)).y;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        dl = m_maxf(dl, v3_len(v3_sub(ang_momentum(&w->bodies[id]), l0)) / v3_len(l0));
        de = m_maxf(de, m_absf(rot_energy(&w->bodies[id]) - e0) / e0);
        f32 cur = quat_rotate(w->bodies[id].rot, v3(0, 1, 0)).y;
        if ((cur < 0.0f) != (prev < 0.0f)) flips++;
        prev = cur;
    }
    m[0] = dl * 100.0f; m[1] = de * 100.0f; m[2] = flips;
    m[-1] = dl < 0.01f && de < 0.01f && flips >= 2;
}

static u32 pyramid_hash(b32 simd) {
    arena_reset(&g_suite_arena, 0);
    PhysWorld* w = world_new(256, 256 * 8);
    w->simd = simd;
    add_ground(w, 0.6f, 0.0f);
    build_wall(w, 20, NULL);
    for (u32 s = 0; s < 300; s++) step(w);
    return phys_state_hash(w);
}

static void t_determinism(f64* m) {
    u32 a = pyramid_hash(TRUE), b = pyramid_hash(TRUE), c = pyramid_hash(FALSE);
    m[0] = a == b; m[1] = a == c;
    m[-1] = a == b && a == c;
}

static void t_large(f64* m) {
    const u32 n = 2000;
    PhysWorld* w = world_new(n + 8, n * 12 + 64);
    add_ground(w, 0.6f, 0.0f);
    u32 side = 13;
    u32 k = 0;
    for (u32 y = 0; k < n; y++) {
        for (u32 z = 0; z < side && k < n; z++) {
            for (u32 x = 0; x < side && k < n; x++, k++) {
                f32 jx = (f32)((k * 7919u) % 100u) * 0.002f - 0.1f, jz = (f32)((k * 104729u) % 100u) * 0.002f - 0.1f;
                BodyDesc b = phys_desc_box(v3(((f32)x - 6.0f) * 1.3f + jx, 0.7f + (f32)y * 1.3f, ((f32)z - 6.0f) * 1.3f + jz), v3(0.5f, 0.5f, 0.5f), 1.0f);
                b.rot = quat_from_axis_angle(v3_norm(v3(jx, 1.0f, jz)), jx * 3.0f);
                phys_add_body(w, &b);
            }
        }
    }
    f64 act = 0, set = 0;
    for (u32 s = 0; s < 600; s++) {
        step(w);
        if (s < 300) act += w->stats.step_ms;
        else set += w->stats.step_ms;
    }
    m[0] = act / 300.0; m[1] = set / 300.0; m[2] = w->stats.contacts; m[3] = phys_max_penetration(w);
    m[-1] = 1.0;
}

EXPORT(phys_suite_run) f64* phys_suite_run(i32 id) {
    if (!g_suite_ready) {
        if (!arena_init_sys(&g_suite_arena, (usize)64 << 20)) return NULL;
        g_suite_ready = TRUE;
    }
    arena_reset(&g_suite_arena, 0);
    for (u32 i = 0; i < 2 + SUITE_METRICS; i++) g_out[i] = 0.0;
    g_step_ms = 0.0;
    g_steps = 0;
    f64* m = g_out + 1;
    switch (id) {
    case ST_STACK: t_stack(m); break;
    case ST_PYRAMID: t_pyramid(m); break;
    case ST_MASS_RATIO: t_mass_ratio(m); break;
    case ST_HEAVY_CHAIN: t_heavy_chain(m); break;
    case ST_FRICTION: t_friction(m); break;
    case ST_RESTITUTION: t_restitution(m); break;
    case ST_OVERLAP: t_overlap(m); break;
    case ST_DOMINO: t_domino(m); break;
    case ST_BULLET: t_bullet(m); break;
    case ST_PENDULUM: t_pendulum(m); break;
    case ST_GYRO: t_gyro(m); break;
    case ST_DETERMINISM: t_determinism(m); break;
    case ST_LARGE: t_large(m); break;
    default: return NULL;
    }
    g_out[1 + SUITE_METRICS] = g_steps ? g_step_ms / (f64)g_steps : 0.0;
    return g_out;
}
