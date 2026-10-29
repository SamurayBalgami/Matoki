/* tests/test_phys_drop.c — üst üste bırakılan toplar (Galileo topu / Newton beşiği), M11 Faz 1.
 * Referans: tek boyutlu olay tabanlı kesin çözüm — aynı ivmeli toplar arası göreli hareket doğrusal,
 * zemin çarpması ikinci derece; her çarpışma iki cisim formülüyle (restitüsyon e). Motor (soft çözücü,
 * 3D küreler) her topun ilk zemin çarpmasından sonraki DROP_WINDOW saniyedeki en yüksek noktasında
 * referansla karşılaştırılır; eşit kütlede en üstteki en çok, en ağır üstteyse en üstteki en az sıçrar. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "phys/phys.h"

#define DROP_MAX    5
#define DROP_G      9.81
#define DROP_WINDOW 1.5

typedef struct DropCase {
    u32 n;
    f32 mass[DROP_MAX];   /* alttan üste */
    f32 radius;
    f32 gap;              /* toplar arası boşluk (yüzeyden yüzeye) */
    f32 bottom;           /* alttaki topun başlangıç yüzey yüksekliği */
    f32 e;
} DropCase;

static void note_case(const char* tag, const char* kind, u32 i, f64 v);

static f64 start_y(const DropCase* c, u32 i) {
    return (f64)c->bottom + c->radius + i * (2.0 * c->radius + c->gap);
}

static void drop_reference(const DropCase* c, f64* peak) {
    f64 y[DROP_MAX], v[DROP_MAX];
    u32 n = c->n;
    f64 r = c->radius, t = 0.0, t_end = 1e30;
    for (u32 i = 0; i < n; i++) { y[i] = start_y(c, i); v[i] = 0.0; peak[i] = 0.0; }
    for (u32 ev = 0; ev < 100000 && t < t_end; ev++) {
        /* en yakın olay: zemin (top 0) ya da komşu çift */
        f64 tmin = 1e30;
        i32 kind = -2;   /* -1 zemin, k ≥ 0: çift (k, k+1) */
        {   /* y0 - r + v0 τ - g τ²/2 = 0, ileri kök */
            f64 disc = v[0] * v[0] + 2.0 * DROP_G * (y[0] - r);
            if (disc >= 0.0) {
                f64 tau = (v[0] + __builtin_sqrt(disc)) / DROP_G;
                if (tau > 1e-12) { tmin = tau; kind = -1; }
            }
        }
        for (u32 k = 0; k + 1 < n; k++) {
            f64 sep = y[k + 1] - y[k] - 2.0 * r, rv = v[k + 1] - v[k];
            if (rv < 0.0) {
                f64 tau = sep > 0.0 ? -sep / rv : 0.0;
                if (tau < tmin) { tmin = tau; kind = (i32)k; }
            }
        }
        if (kind == -2) break;
        if (t + tmin > t_end) { tmin = t_end - t; kind = -3; }
        /* olaya kadar ilerle; aralıktaki tepe (çarpmadan sonra) en yüksek noktaya katılır */
        for (u32 i = 0; i < n; i++) {
            if (t_end < 1e29 && v[i] > 0.0 && v[i] / DROP_G <= tmin) {
                f64 apex = y[i] + v[i] * v[i] / (2.0 * DROP_G);
                if (apex > peak[i]) peak[i] = apex;
            }
            y[i] += v[i] * tmin - 0.5 * DROP_G * tmin * tmin;
            v[i] -= DROP_G * tmin;
            if (t_end < 1e29 && y[i] > peak[i]) peak[i] = y[i];
        }
        t += tmin;
        if (kind == -1) {
            y[0] = r;
            v[0] = -c->e * v[0];
            if (t_end > 1e29) {   /* ilk zemin çarpması: pencere başlar */
                t_end = t + DROP_WINDOW;
                for (u32 i = 0; i < n; i++) peak[i] = y[i];
            }
        } else if (kind >= 0) {
            u32 a = (u32)kind, b = a + 1;
            f64 ma = c->mass[a], mb = c->mass[b];
            f64 rel = v[b] - v[a], p = ma * v[a] + mb * v[b];
            /* ma va' + mb vb' = p,  vb' - va' = -e (vb - va) */
            v[a] = (p + mb * c->e * rel) / (ma + mb);
            v[b] = (p - ma * c->e * rel) / (ma + mb);
            y[b] = y[a] + 2.0 * r;
        }
    }
}

static PhysWorld* drop_world(const DropCase* c, u32* id) {
    PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
    phys_world_init(w, test_arena(), 16, 64);
    w->allow_sleep = FALSE;
    BodyDesc g = phys_desc_plane(v3(0, 1, 0), 0.0f);
    g.restitution = c->e;
    g.friction = 0.0f;
    phys_add_body(w, &g);
    for (u32 i = 0; i < c->n; i++) {
        BodyDesc d = phys_desc_sphere(v3(0, (f32)start_y(c, i), 0), c->radius, c->mass[i]);
        d.restitution = c->e;
        d.friction = 0.0f;
        id[i] = phys_add_body(w, &d);
    }
    return w;
}

static void drop_engine(const DropCase* c, f32 hz, f64* peak) {
    u32 id[DROP_MAX];
    PhysWorld* w = drop_world(c, id);
    const f32 dt = 1.0f / hz;
    i32 left = -1;   /* pencerede kalan adım; -1 = zemin çarpması bekleniyor */
    for (u32 i = 0; i < c->n; i++) peak[i] = 0.0;
    for (u32 s = 0; s < (u32)hz * 10 && left != 0; s++) {
        phys_step(w, dt);
        if (left < 0 && w->bodies[id[0]].vel.y > 0.0f) left = (i32)(DROP_WINDOW / dt + 0.5);
        if (left < 0) continue;
        left--;
        for (u32 i = 0; i < c->n; i++) {
            const Body* b = &w->bodies[id[i]];
            /* adım içindeki tepe: yukarı hız kalan süre içinde sıfırlanıyorsa parabol tepesi */
            f64 y = b->pos.y;
            if (b->vel.y > 0.0f && b->vel.y < DROP_G * dt) y += (f64)b->vel.y * b->vel.y / (2.0 * DROP_G);
            if (y > peak[i]) peak[i] = y;
        }
    }
}

static void note_case(const char* tag, const char* kind, u32 i, f64 v) {
    char buf[96];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    sb_cstr(&sb, tag);
    sb_cstr(&sb, kind);
    sb_u64(&sb, i);
    buf[sb.len] = 0;
    test_note(buf, v, 3);
}

static void run_case(TestCtx* t_, const char* tag, const DropCase* c, f32 hz, f64 tol, f64* eng) {
    f64 ref[DROP_MAX];
    drop_reference(c, ref);
    drop_engine(c, hz, eng);
    for (u32 i = 0; i < c->n; i++) {
        note_case(tag, " ref ", i, ref[i]);
        note_case(tag, " motor ", i, eng[i]);
        CHECK_NEAR(eng[i], ref[i], tol * ref[i]);
    }
}

TEST(phys_drop_single_ball) {
    DropCase one = { 1, { 1 }, 0.25f, 0.0f, 1.5f, 0.9f };
    f64 p[DROP_MAX];
    run_case(t_, "tek", &one, 60.0f, 0.02, p);
}

TEST(phys_drop_pair_zero_g) {
    /* yerçekimsiz iki cisim, e = 0.9: kesin sonuç (geri sekme hızları) */
    static const f32 M[][2] = { { 1, 1 }, { 4, 2 }, { 1, 4 } };
    for (u32 k = 0; k < 3; k++) {
        PhysWorld* w = ARENA_NEW(test_arena(), PhysWorld);
        phys_world_init(w, test_arena(), 4, 16);
        w->gravity = v3(0, 0, 0);
        BodyDesc a = phys_desc_sphere(v3(0, 0, 0), 0.25f, M[k][0]);
        BodyDesc b = phys_desc_sphere(v3(0, 1.0f, 0), 0.25f, M[k][1]);
        a.vel = v3(0, 5.0f, 0);
        b.vel = v3(0, -5.0f, 0);
        a.restitution = b.restitution = 0.9f;
        a.friction = b.friction = 0.0f;
        u32 ia = phys_add_body(w, &a), ib = phys_add_body(w, &b);
        for (u32 i = 0; i < 60; i++) phys_step(w, 1.0f / 60.0f);
        f32 ma = M[k][0], mb = M[k][1], rel = -10.0f, p = ma * 5.0f - mb * 5.0f;
        f32 va = (p + mb * 0.9f * rel) / (ma + mb), vb = (p - ma * 0.9f * rel) / (ma + mb);
        note_case("cift", " va ", k, w->bodies[ia].vel.y);
        note_case("cift", " vb ", k, w->bodies[ib].vel.y);
        CHECK_NEAR(w->bodies[ia].vel.y, va, 1e-3);
        CHECK_NEAR(w->bodies[ib].vel.y, vb, 1e-3);
    }
}

/* sıçrama oranı: topun çarpmadan sonraki tepesinin bırakıldığı yüksekliğe oranı (yüzey, zeminden) */
static f64 bounce_ratio(const DropCase* c, const f64* peak, u32 i) {
    return (peak[i] - c->radius) / (start_y(c, i) - c->radius);
}

TEST(phys_drop_stack_matches_event_reference) {
    /* 240 Hz: hafif alt top bir 60 Hz adımında 2–3 kez çarpışabilir (adım başına tek restitüsyon) → kesin
     * referansla %2 içinde ancak bu hızda. 60 Hz yalnızca kare başına ≤ 1 çarpışmalı dizilerde (D31). */
    f64 eq[DROP_MAX], top[DROP_MAX], bot[DROP_MAX], tmp[DROP_MAX];
    DropCase c_eq = { 3, { 1, 1, 1 }, 0.25f, 0.3f, 1.5f, 0.9f };
    DropCase c_top = { 3, { 1, 2, 4 }, 0.25f, 0.3f, 1.5f, 0.9f };
    DropCase c_bot = { 3, { 4, 2, 1 }, 0.25f, 0.3f, 1.5f, 0.9f };
    run_case(t_, "240 Hz esit", &c_eq, 240.0f, 0.02, eq);
    run_case(t_, "240 Hz agir ustte", &c_top, 240.0f, 0.02, top);
    run_case(t_, "240 Hz agir altta", &c_bot, 240.0f, 0.02, bot);
    run_case(t_, "60 Hz esit", &c_eq, 60.0f, 0.05, tmp);
    run_case(t_, "60 Hz agir altta", &c_bot, 60.0f, 0.05, tmp);
    /* eşit kütle: yukarı çıktıkça sıçrama oranı artar, en üstteki en çok */
    CHECK(bounce_ratio(&c_eq, eq, 2) > bounce_ratio(&c_eq, eq, 1));
    CHECK(bounce_ratio(&c_eq, eq, 1) > bounce_ratio(&c_eq, eq, 0));
    /* en üstteki topun sıçraması: ağır altta (katapult) > eşit > ağır üstte */
    test_note("oran ust: agir altta", bounce_ratio(&c_bot, bot, 2), 3);
    test_note("oran ust: esit", bounce_ratio(&c_eq, eq, 2), 3);
    test_note("oran ust: agir ustte", bounce_ratio(&c_top, top, 2), 3);
    CHECK(bounce_ratio(&c_bot, bot, 2) > 2.0);
    CHECK(bounce_ratio(&c_eq, eq, 2) > bounce_ratio(&c_top, top, 2));
}

