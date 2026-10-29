/* tests/test_m13_port.c — M13 faz 1: MatokiEngine'den alınan çekirdek ve modüllerin bizim tarafta sınanması.
 * mat3 2D / mat4_inverse (onların test_math'ından), rng_range_i32, arena_sub, profiler, base64 (RFC 4648 vektörleri),
 * görüntü filtrelerinin kesin özellikleri, mesh_compute_normals ve batch2d'nin bizim rasterizer'da piksel çıktısı. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/profiler.h"
#include "core/rng.h"
#include "asset/base64.h"
#include "gfx2d/batch2d.h"
#include "gfx3d/mesh.h"
#include "img/filter.h"
#include "img/image.h"

TEST(m13_mat3_2d_and_perspective) {
    /* (1, 0): (2, 3) ölçek, 90° dönme, (10, 20) öteleme → (10, 22) */
    Mat3 m2 = mat3_2d(10.0f, 20.0f, M_HALF_PI_F, 2.0f, 3.0f);
    Vec2 p = mat3_mul_point(&m2, v2(1.0f, 0.0f));
    CHECK_NEAR(p.x, 10.0f, 1e-5);
    CHECK_NEAR(p.y, 22.0f, 1e-5);
    /* mat3_mul: (A·B)·p = A·(B·p) */
    Mat3 a = mat3_2d(1.0f, -2.0f, 0.3f, 1.5f, 0.5f), b = mat3_2d(-4.0f, 7.0f, -1.1f, 2.0f, 3.0f);
    Mat3 ab = mat3_mul(&a, &b);
    Vec2 q = v2(0.7f, -1.3f), bq = mat3_mul_point(&b, q);
    Vec2 l = mat3_mul_point(&ab, q), r = mat3_mul_point(&a, bq);
    CHECK_NEAR(l.x, r.x, 1e-5);
    CHECK_NEAR(l.y, r.y, 1e-5);
    Mat3 id = mat3_identity(), ida = mat3_mul(&id, &a);
    for (u32 i = 0; i < 9; i++) CHECK(ida.m[i] == a.m[i]);
    /* perspektif derinliği 0 (yakın) .. 1 (uzak): onların testiyle aynı sözleşme */
    Mat4 proj = mat4_perspective(60.0f * M_DEG2RAD_F, 16.0f / 9.0f, 0.5f, 100.0f);
    Vec4 cn = mat4_mul_v4(&proj, v4(0.0f, 0.0f, -0.5f, 1.0f));
    Vec4 cf = mat4_mul_v4(&proj, v4(0.0f, 0.0f, -100.0f, 1.0f));
    CHECK_NEAR(cn.z / cn.w, 0.0f, 1e-5);
    CHECK_NEAR(cf.z / cf.w, 1.0f, 1e-5);
}

TEST(m13_mat4_inverse_random_100) {
    Rng r = rng_make(42);
    f64 worst = 0.0;
    for (u32 n = 0; n < 100; n++) {
        Mat4 m;
        for (u32 i = 0; i < 16; i++) m.m[i] = rng_f32(&r) * 2.0f - 1.0f;
        m.m[0] += 3.0f;
        m.m[5] += 3.0f;
        m.m[10] += 3.0f;
        m.m[15] += 3.0f;
        Mat4 prod = mat4_mul(mat4_inverse(m), m);
        for (u32 i = 0; i < 16; i++) {
            f64 e = (f64)prod.m[i] - ((i % 5 == 0) ? 1.0 : 0.0);
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
    }
    test_note("mat4_inverse: en buyuk |inv*m - I|", worst, 8);
    CHECK(worst < 1e-5);
    /* afin dönüşüm: ters, öteleme ve dönmeyi geri alır */
    Mat4 t = mat4_mul(mat4_translate(v3(3.0f, -2.0f, 5.0f)), mat4_rotate_y(0.7f));
    Mat4 ti = mat4_inverse(t);
    Vec3 p = v3(1.0f, 2.0f, 3.0f), back = mat4_mul_point(&ti, mat4_mul_point(&t, p));
    CHECK_NEAR(back.x, p.x, 1e-5);
    CHECK_NEAR(back.y, p.y, 1e-5);
    CHECK_NEAR(back.z, p.z, 1e-5);
    /* tekil ve NaN → birim matris (çökme yok) */
    Mat4 z;
    mem_zero(&z, sizeof(z));
    Mat4 zi = mat4_inverse(z);
    CHECK(zi.m[0] == 1.0f && zi.m[1] == 0.0f && zi.m[15] == 1.0f);
    z = mat4_identity();
    z.m[5] = m_bits_f32(0x7FC00000u);
    zi = mat4_inverse(z);
    CHECK(zi.m[5] == 1.0f);
}

TEST(m13_rng_range_i32_and_arena_sub) {
    Rng r = rng_make(7);
    u32 hist[5] = { 0 }, out = 0;
    for (u32 i = 0; i < 50000; i++) {
        i32 v = rng_range_i32(&r, -2, 3);
        if (v >= -2 && v < 3) hist[v + 2]++;
        else out++;
    }
    CHECK(out == 0);
    for (u32 k = 0; k < 5; k++) CHECK(hist[k] > 9500 && hist[k] < 10500);   /* düzgün dağılım ±%5 */
    CHECK(rng_range_i32(&r, 4, 4) == 4);
    CHECK(rng_range_i32(&r, 5, 1) == 5);

    Arena* p = test_arena();
    usize before = p->used;
    Arena c = arena_sub(p, 4096);
    CHECK(p->used >= before + 4096);
    CHECK(((uptr)c.base & 15u) == 0);
    u8* x = (u8*)arena_alloc(&c, 4000, 1);
    CHECK(x >= c.base && x + 4000 <= c.base + 4096);
    CHECK(arena_try(&c, 200, 1) == NULL && c.fails == 1);   /* alt arena kendi sınırında durur */
}

TEST(m13_profiler_nesting) {
    prof_reset();
    for (u32 f = 0; f < 3; f++) {
        prof_begin("dis");
        prof_begin("ic");
        prof_end();
        prof_begin("ic");
        prof_end();
        prof_end();
        prof_frame_end();
    }
    CHECK(prof_zone_count() == 2);
    CHECK(prof_frame_count() == 3);
    const ProfZone* dis = prof_zone_get(0);
    const ProfZone* ic = prof_zone_get(1);
    CHECK(dis && ic && dis->last_calls == 1 && ic->last_calls == 2 && ic->total_calls == 6);
    CHECK(dis->last_ms >= ic->last_ms);
    CHECK(prof_zone_get(2) == NULL);
    /* derinlik aşımı ve fazladan prof_end: çökme yok, eşleşme korunur */
    for (u32 i = 0; i < PROF_MAX_STACK + 8; i++) prof_begin("derin");
    for (u32 i = 0; i < PROF_MAX_STACK + 20; i++) prof_end();
    prof_frame_end();
    CHECK(prof_zone_count() == 3);
    CHECK(prof_zone_get(2)->last_calls == PROF_MAX_STACK);
    prof_reset();
    CHECK(prof_zone_count() == 0);
}

static b32 b64(const char* text, const char* expect) {
    u8 out[64];
    u32 n = 0;
    if (!base64_decode(text, (u32)cstr_len(text), out, sizeof(out), &n)) return FALSE;
    u32 el = (u32)cstr_len(expect);
    return n == el && mem_equal(out, expect, el);
}

TEST(m13_base64_rfc4648) {
    /* RFC 4648 §10 test vektörleri */
    CHECK(b64("", ""));
    CHECK(b64("Zg==", "f"));
    CHECK(b64("Zm8=", "fo"));
    CHECK(b64("Zm9v", "foo"));
    CHECK(b64("Zm9vYg==", "foob"));
    CHECK(b64("Zm9vYmE=", "fooba"));
    CHECK(b64("Zm9vYmFy", "foobar"));
    CHECK(b64("Zm9v\nYmFy", "foobar"));   /* boşluk atlanır */
    u8 out[16];
    u32 n;
    CHECK(!base64_decode("Zm9v!", 5, out, sizeof(out), &n));   /* geçersiz karakter */
    CHECK(!base64_decode("Zg=a", 4, out, sizeof(out), &n));    /* dolgudan sonra veri */
    CHECK(base64_max_decoded(8) >= 6);
}

/* düz renk: tüm filtreler değiştirmemeli (bulanıklık normalize, boyutlandırma ağırlıkları 1) */
TEST(m13_image_filters_exact_properties) {
    Arena* a = test_arena();
    const u32 W = 32, H = 16, C = color_pack(200, 100, 50, 255);
    u32* px = ARENA_ARRAY(a, u32, W * H);
    for (u32 i = 0; i < W * H; i++) px[i] = C;
    img_blur_box(px, W, H, 3, a);
    img_blur_gaussian(px, W, H, 5, 2.0f, a);
    b32 same = TRUE;
    for (u32 i = 0; i < W * H; i++) same &= px[i] == C;
    CHECK(same);
    u32* dst = ARENA_ARRAY(a, u32, 64 * 64);
    img_resize_bilinear(px, W, H, dst, 57, 23);
    for (u32 i = 0; i < 57 * 23; i++) same &= dst[i] == C;
    img_resize_box(px, W, H, dst, 7, 5);
    for (u32 i = 0; i < 7 * 5; i++) same &= dst[i] == C;
    CHECK(same);

    /* kutu küçültme: 2×2 dama tahtası (0 / 200) → her piksel ortalama 100 */
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) px[y * W + x] = ((x ^ y) & 1) ? color_pack(200, 200, 200, 255) : color_pack(0, 0, 0, 255);
    img_resize_box(px, W, H, dst, W / 2, H / 2);
    CHECK(dst[0] == color_pack(100, 100, 100, 255) && dst[(H / 2) * (W / 2) - 1] == color_pack(100, 100, 100, 255));
    /* 1:1 çift doğrusal = kopya */
    img_resize_bilinear(px, W, H, dst, W, H);
    same = TRUE;
    for (u32 i = 0; i < W * H; i++) same &= dst[i] == px[i];
    CHECK(same);

    /* kutu bulanıklık toplamı korur (iç bölgede tek parlak piksel → 7×7 kare, her biri 255/49 ≈ 5) */
    for (u32 i = 0; i < W * H; i++) px[i] = 0;
    px[8 * W + 16] = color_pack(255, 255, 255, 255);
    img_blur_box(px, W, H, 3, a);
    u32 sum = 0;
    for (u32 i = 0; i < W * H; i++) sum += px[i] & 0xFFu;
    CHECK(sum >= 49 * 5 && sum <= 49 * 6);
    CHECK((px[5 * W + 13] & 0xFFu) == 5 && (px[4 * W + 16] & 0xFFu) == 0);

    /* normal haritası: düz → (128, 128, 255); x'te artan yükseklik → nx < 128 (yüzey sola eğik) */
    for (u32 i = 0; i < W * H; i++) px[i] = color_pack(90, 90, 90, 255);
    img_normal_from_height(px, dst, W, H, 2.0f, TRUE);
    CHECK(dst[5 * W + 5] == color_pack(128, 128, 255, 255));
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) px[y * W + x] = color_pack(x * 8, x * 8, x * 8, 255);
    img_normal_from_height(px, dst, W, H, 2.0f, FALSE);
    u32 c = dst[5 * W + 10];
    CHECK((c & 0xFFu) < 128 && ((c >> 8) & 0xFFu) == 128 && ((c >> 16) & 0xFFu) > 128);

    /* alfa ön-çarpma ve kimlik renk düzeltmesi */
    px[0] = color_pack(200, 100, 50, 0);
    px[1] = color_pack(200, 100, 50, 255);
    px[2] = color_pack(200, 100, 50, 128);
    img_premultiply_alpha(px, 3, 1);
    CHECK(px[0] == color_pack(0, 0, 0, 0));
    CHECK(px[1] == color_pack(200, 100, 50, 255));
    CHECK(px[2] == color_pack(100, 50, 25, 128));
    px[0] = color_pack(10, 128, 250, 77);
    img_color_grade(px, 1, 1, 0.0f, 1.0f, 1.0f, 1.0f);
    CHECK(px[0] == color_pack(10, 128, 250, 77));
}

TEST(m13_mesh_compute_normals) {
    Mesh* m = mesh_cube(test_arena());
    u32 nv = m->vertex_count;
    f32* ref = ARENA_ARRAY(test_arena(), f32, nv * 3);
    for (u32 i = 0; i < nv; i++) {
        ref[i * 3] = m->nx[i];
        ref[i * 3 + 1] = m->ny[i];
        ref[i * 3 + 2] = m->nz[i];
    }
    mesh_compute_normals(m);
    f32 worst = 0.0f;
    for (u32 i = 0; i < nv; i++) {
        worst = m_maxf(worst, m_absf(m->nx[i] - ref[i * 3]));
        worst = m_maxf(worst, m_absf(m->ny[i] - ref[i * 3 + 1]));
        worst = m_maxf(worst, m_absf(m->nz[i] - ref[i * 3 + 2]));
    }
    test_note("kup: hesaplanan ↔ uretecin normali en buyuk fark", worst, 7);
    CHECK(worst < 1e-6f);   /* sargı yönü dışa doğru (CCW) ve yüz başına ayrı köşeler */
    /* bozuk indeks atlanır, dokunulmayan köşe 0 kalır */
    m->indices[0] = 99999;
    mesh_compute_normals(m);
    CHECK(m->nx[0] == m->nx[0]);   /* NaN değil */
}

/* 16×16 hedef, 2×2 doku (A B / C D), merkez (8, 8) boyut 8 → piksel 4..11 */
typedef struct SpriteEnv {
    Framebuffer fb;
    Raster r;
    Arena bins, frame;
    Texture* tex;
} SpriteEnv;

static const u32 K_A = 0xFF0000FFu, K_B = 0xFF00FF00u, K_C = 0xFFFF0000u, K_D = 0x00FFFFFFu;   /* D: alfa 0 */
static const u32 K_BG = 0xFF101010u;

static void sprite_env(SpriteEnv* e) {
    Arena* a = test_arena();
    mem_zero(e, sizeof(*e));
    e->fb.w = 16;
    e->fb.h = 16;
    e->fb.color = ARENA_ARRAY(a, u32, 256);
    e->fb.depth = ARENA_ARRAY(a, f32, 256);
    fb_clear(&e->fb, K_BG, 1.0f);
    arena_init(&e->bins, arena_alloc(a, 1 << 20, 16), 1 << 20);
    arena_init(&e->frame, arena_alloc(a, 1 << 16, 16), 1 << 16);
    raster_init(&e->r, &e->fb);
    raster_set_depth(&e->r, FALSE, FALSE, DEPTH_ALWAYS);
    raster_set_cull(&e->r, CULL_NONE);
    raster_begin(&e->r, &e->bins);
    e->tex = tex_create(a, 2, 2, TEX_FLAG_CLAMP);
    e->tex->px[0] = K_A;
    e->tex->px[1] = K_B;
    e->tex->px[2] = K_C;
    e->tex->px[3] = K_D;
}

static u32 at(const SpriteEnv* e, u32 x, u32 y) { return e->fb.color[y * 16 + x]; }

TEST(m13_batch2d_sprite_pixels) {
    SpriteEnv e;
    sprite_env(&e);
    Batch2D b;
    b2d_begin(&b, &e.r, &e.frame, mat3_identity());
    b2d_sprite(&b, e.tex, rect(0, 0, 1, 1), v2(8, 8), v2(8, 8), 0.0f, v4(1, 1, 1, 1));
    b2d_end(&b);
    CHECK(at(&e, 4, 4) == K_A && at(&e, 7, 7) == K_A);
    CHECK(at(&e, 8, 4) == K_B && at(&e, 11, 7) == K_B);
    CHECK(at(&e, 4, 8) == K_C);
    CHECK(at(&e, 11, 11) == K_D);   /* opak: alfa 0 da yazılır */
    CHECK(at(&e, 3, 3) == K_BG && at(&e, 12, 12) == K_BG && at(&e, 12, 4) == K_BG);
    u32 drawn = 0;
    for (u32 i = 0; i < 256; i++) drawn += e.fb.color[i] != K_BG;
    CHECK(drawn == 64);
    CHECK(b.sprites == 1);

    /* kesik karışım + yarım renk + 90° dönme + kamera ötelemesi */
    sprite_env(&e);
    b2d_begin(&b, &e.r, &e.frame, mat3_2d(1.0f, 0.0f, 0.0f, 1.0f, 1.0f));
    b2d_set_blend(&b, B2D_CUTOUT);
    b2d_sprite(&b, e.tex, rect(0, 0, 2, 2), v2(7, 8), v2(8, 8), M_HALF_PI_F, v4(0.5f, 1, 1, 1));
    b2d_end(&b);
    /* dönmüş: A sağ üstte, D (alfa 0) sol altta → atılır */
    CHECK(at(&e, 10, 5) == ((K_A & 0xFFFFFF00u) | 0x80u));   /* R 255 × 0,5 = 127,5 → 128 */
    CHECK(at(&e, 5, 10) == K_BG);
    CHECK(at(&e, 5, 5) == K_B || at(&e, 5, 5) == K_C);
}
