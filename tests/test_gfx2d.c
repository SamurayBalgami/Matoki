/* tests/test_gfx2d.c — 2D temel testleri: karo çarpışması (zemin/duvar/tavan, tünelleme yok,
 * tek yönlü platform, rastgele haritada "katının içine asla girmez" değişmezi), Camera2D
 * (sınır, ölü bölge, kare hızından bağımsız yumuşatma, ters dönüşüm) ve animasyon zamanlaması.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_gfx2d.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "test.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/arena.h"
#include "core/math.h"
#include "core/rng.h"
#include "gfx2d/tilemap.h"
#include "gfx2d/camera2d.h"
#include "gfx2d/anim.h"

#define G_MEM_SIZE (MB(2))
static u8* g_mem;   /* her TEST başında test_arena'dan */

static const TileDef k_defs[] = {
    { 0, 0 },                      /* 0 boş */
    { 1, TILE_SOLID },             /* 1 duvar */
    { 2, TILE_ONEWAY },            /* 2 tek yönlü */
    { 3, TILE_HAZARD },            /* 3 diken */
};

static void map_from(Tilemap* m, Arena* a, const char* txt, u32 w, u32 h) {
    tilemap_init(m, a, w, h, 16.0f);
    m->defs = k_defs;
    m->def_count = ARRAY_COUNT(k_defs);
    m->border_tile = 1;
    u16 legend[128] = {0};
    legend['#'] = 1;
    legend['='] = 2;
    legend['^'] = 3;
    tilemap_load_ascii(m, txt, legend);
}

static const char* k_room =
    "################\n"
    "#..............#\n"
    "#..............#\n"
    "#......===.....#\n"
    "#..............#\n"
    "#....#.........#\n"
    "#....#.....^^..#\n"
    "################\n";

TEST(tilemap_floor_wall_ceiling) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    arena_init(&a, g_mem, G_MEM_SIZE);
    Tilemap m;
    map_from(&m, &a, k_room, 16, 8);
    Vec2 size = v2(10.0f, 14.0f);

    /* Zemine düşüş: alt kenar tam karo üstüne yaslanır */
    Vec2 p = v2(40.0f, 20.0f);
    TileHit h = tilemap_move(&m, &p, size, v2(0.0f, 500.0f), FALSE);
    CHECK(h.on_ground);
    CHECK_NEAR(p.y + size.y, 7.0f * 16.0f, 1e-4);

    /* Sağa koşu: x=5 sütunundaki duvara (y 5..6) yaslanır */
    p = v2(20.0f, 7.0f * 16.0f - size.y);
    h = tilemap_move(&m, &p, size, v2(300.0f, 0.0f), FALSE);
    CHECK(h.hit_right);
    CHECK_NEAR(p.x + size.x, 5.0f * 16.0f, 1e-4);

    /* Tavan */
    p = v2(40.0f, 40.0f);
    h = tilemap_move(&m, &p, size, v2(0.0f, -300.0f), FALSE);
    CHECK(h.hit_ceiling);
    CHECK_NEAR(p.y, 16.0f, 1e-4);

    /* Çapraz hareket: zemine değince x'te kaymaya devam eder */
    p = v2(100.0f, 80.0f);
    h = tilemap_move(&m, &p, size, v2(30.0f, 200.0f), FALSE);
    CHECK(h.on_ground);
    CHECK_NEAR(p.x, 130.0f, 1e-4);

    /* Diken: çarpışma çözmez, raporlar */
    p = v2(11.0f * 16.0f + 2.0f, 6.0f * 16.0f - size.y - 1.0f);
    h = tilemap_move(&m, &p, size, v2(0.0f, 4.0f), FALSE);
    CHECK(h.touched & TILE_HAZARD);
}

TEST(tilemap_no_tunneling) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    arena_init(&a, g_mem, G_MEM_SIZE);
    Tilemap m;
    /* 1 karo kalınlığında duvar; tek karede 20 karo hız */
    map_from(&m, &a,
             "##########################################\n"
             "#....................#...................#\n"
             "##########################################\n", 42, 3);
    Vec2 size = v2(6.0f, 6.0f);
    Vec2 p = v2(20.0f, 20.0f);
    TileHit h = tilemap_move(&m, &p, size, v2(320.0f, 0.0f), FALSE);
    CHECK(h.hit_right);
    CHECK(p.x + size.x <= 21.0f * 16.0f + 1e-4f);
}

TEST(tilemap_oneway_platform) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    arena_init(&a, g_mem, G_MEM_SIZE);
    Tilemap m;
    map_from(&m, &a, k_room, 16, 8);
    Vec2 size = v2(10.0f, 14.0f);
    f32 plat_top = 3.0f * 16.0f;
    /* Alttan zıplayınca geçer */
    Vec2 p = v2(7.0f * 16.0f + 2.0f, 4.0f * 16.0f + 2.0f);
    TileHit h = tilemap_move(&m, &p, size, v2(0.0f, -40.0f), FALSE);
    CHECK(!h.hit_ceiling);
    CHECK(p.y < plat_top);
    /* Yukarıdan inerken üstüne konar */
    p = v2(7.0f * 16.0f + 2.0f, plat_top - size.y - 10.0f);
    h = tilemap_move(&m, &p, size, v2(0.0f, 30.0f), FALSE);
    CHECK(h.on_ground);
    CHECK_NEAR(p.y + size.y, plat_top, 1e-4);
    /* Aşağı + zıpla: içinden düşer */
    h = tilemap_move(&m, &p, size, v2(0.0f, 8.0f), TRUE);
    CHECK(!h.on_ground);
    CHECK(p.y + size.y > plat_top);
}

/* Rastgele haritalarda rastgele hareketler: AABB hiçbir zaman katı karoyla kesişmez */
TEST(tilemap_fuzz_never_inside_solid) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    Rng rng = rng_make(1234);
    u32 violations = 0, moves = 0;
    for (u32 map_i = 0; map_i < 20; map_i++) {
        arena_init(&a, g_mem, G_MEM_SIZE);
        Tilemap m;
        tilemap_init(&m, &a, 24, 16, 16.0f);
        m.defs = k_defs;
        m.def_count = ARRAY_COUNT(k_defs);
        m.border_tile = 1;
        for (u32 i = 0; i < 24u * 16u; i++) {
            u32 r = rng_u32(&rng) % 100u;
            m.tiles[i] = (r < 22u) ? 1u : (r < 28u) ? 2u : 0u;
        }
        for (u32 t = 0; t < 50; t++) {
            Vec2 size = v2(4.0f + rng_f32(&rng) * 20.0f, 4.0f + rng_f32(&rng) * 20.0f);
            Vec2 p = v2(rng_f32(&rng) * 360.0f, rng_f32(&rng) * 230.0f);
            if (tilemap_query(&m, p, size) & TILE_SOLID) continue;   /* geçerli başlangıç */
            for (u32 k = 0; k < 40; k++) {
                Vec2 d = v2((rng_f32(&rng) - 0.5f) * 200.0f, (rng_f32(&rng) - 0.5f) * 200.0f);
                tilemap_move(&m, &p, size, d, rng_f32(&rng) < 0.2f);
                moves++;
                if (tilemap_query(&m, p, size) & TILE_SOLID) violations++;
            }
        }
    }
    CHECK(moves > 10000);
    CHECK(violations == 0);
}

/* gerileme (fuzz'ın bulduğu): genişliği karo boyunu eps'ten az aşan kutu sağ duvara yaslanınca
 * sol kenarı soldaki katı karoya 0,0002 girer; dört kenar aynı toleransla değerlendirilmeli */
TEST(tilemap_width_just_over_tile) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Arena a;
    arena_init(&a, g_mem, G_MEM_SIZE);
    Tilemap m;
    map_from(&m, &a,
             "##########\n"
             "#.#.#....#\n"
             "##########\n", 10, 3);
    Vec2 size = v2(16.0002f, 10.0f);
    Vec2 p = v2(48.0f, 20.0f);   /* 3. sütun; solda (2) ve sağda (4) duvar */
    CHECK(!(tilemap_query(&m, p, size) & TILE_SOLID));
    TileHit h = tilemap_move(&m, &p, size, v2(40.0f, 0.0f), FALSE);
    CHECK(h.hit_right);
    CHECK(!(tilemap_query(&m, p, size) & TILE_SOLID));
    h = tilemap_move(&m, &p, size, v2(-40.0f, 0.0f), FALSE);
    CHECK(h.hit_left);
    CHECK(!(tilemap_query(&m, p, size) & TILE_SOLID));
}

TEST(camera2d_follow_and_bounds) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    Camera2D c;
    cam2d_init(&c, 320.0f, 180.0f);
    c.bounds = rect(0.0f, 0.0f, 1000.0f, 400.0f);
    c.deadzone = v2(20.0f, 10.0f);

    /* Ölü bölge içindeki hedef kamerayı oynatmaz */
    c.pos = v2(500.0f, 200.0f);
    cam2d_follow(&c, v2(515.0f, 205.0f), 1.0f / 60.0f);
    CHECK_NEAR(c.pos.x, 500.0f, 1e-5);
    CHECK_NEAR(c.pos.y, 200.0f, 1e-5);

    /* Sınıra kırpma */
    cam2d_follow(&c, v2(-500.0f, -500.0f), 1.0f);
    CHECK_NEAR(c.pos.x, 160.0f, 1e-4);
    CHECK_NEAR(c.pos.y, 90.0f, 1e-4);

    /* Kare hızından bağımsız yumuşatma: 60x(1/60) ≈ 30x(1/30) */
    Camera2D c1 = c, c2 = c;
    c1.follow_rate = c2.follow_rate = 3.0f;
    c1.deadzone = c2.deadzone = v2(0.0f, 0.0f);
    for (u32 i = 0; i < 60; i++) cam2d_follow(&c1, v2(700.0f, 300.0f), 1.0f / 60.0f);
    for (u32 i = 0; i < 30; i++) cam2d_follow(&c2, v2(700.0f, 300.0f), 1.0f / 30.0f);
    CHECK_NEAR(c1.pos.x, c2.pos.x, 1e-2);
    CHECK_NEAR(c1.pos.y, c2.pos.y, 1e-2);

    /* Matris ↔ screen_to_world ters */
    c.zoom = 2.0f;
    c.offset = v2(0.0f, 20.0f);
    Mat3 mtx = cam2d_matrix(&c, FALSE);
    Vec2 w = v2(123.0f, 45.0f);
    Vec2 s = mat3_mul_point(&mtx, w);
    Vec2 back = cam2d_screen_to_world(&c, s);
    CHECK_NEAR(back.x, w.x, 1e-3);
    CHECK_NEAR(back.y, w.y, 1e-3);
}

TEST(anim_timing) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), G_MEM_SIZE, 16);
    static const u16 frames[4] = { 10, 11, 12, 13 };
    AnimClip run = { frames, 4, 10.0f, TRUE };
    AnimClip once = { frames, 3, 10.0f, FALSE };
    AnimPlayer p = { NULL, 0.0f };
    anim_play(&p, &run);
    CHECK(anim_frame(&p) == 10);
    anim_update(&p, 0.25f);
    CHECK(anim_frame(&p) == 12);
    anim_update(&p, 0.2f);          /* 0.45 s → 4. kare = döngüde 0 */
    CHECK(anim_frame(&p) == 10);
    anim_play(&p, &run);            /* aynı klip: devam eder */
    CHECK(anim_frame(&p) == 10);
    anim_play(&p, &once);
    for (u32 i = 0; i < 10; i++) anim_update(&p, 0.05f);
    CHECK(anim_finished(&p));
    CHECK(anim_frame(&p) == 12);
}
