/* tests/test_gcmd.c — M13 faz 8a-8c: GPU komut akışı (gpu/gcmd). Başlık + kayıt düzeni (web/gpu.mjs bunu okur), mesh ve
 * doku kaydı karede bir kez, köşe uniform'u yuvada C yapısının ham baytları, parça uniform'u programın boyu kadar ve
 * ardışık aynı materyalde paylaşılır, doku kimliği uniform'daki işaretçiden; taşmada akış kapanır ve sayar; mesh sürümü
 * tüm mesh'ler arası tekil (aynı adrese yeni mesh gelse de farklı gen). Gölge geçişi: dokunun GC_TEX'i yazılmaz, yalnız derinlik
 * çizimi parça programsız. Bindirme: siyah + beyaz zeminden çıkarılan önceden çarpılmış renk, yazılım karışımıyla aynı sonucu verir. */
#include "test.h"
#include "core/mem.h"
#include "gpu/gcmd.h"
#include "img/image.h"
#include "raster/ref_shaders.h"

static f32 bits_f32(u32 b) {
    f32 f;
    mem_copy(&f, &b, 4);
    return f;
}

static VertUniforms vu_make(Vec3 t, f32 time) {
    VertUniforms v;
    mem_zero(&v, sizeof(v));
    v.model = mat4_translate(t);
    v.view_proj = mat4_perspective(1.0f, 1.6f, 0.1f, 100.0f);
    v.normal_mat = mat3_identity();
    v.time = time;
    v.params[0] = 7.0f;
    return v;
}

TEST(gcmd_stream_layout_and_dedup) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    Mesh* plane = mesh_plane(a, 4.0f, 4.0f, 1.0f, 2);
    Texture* tex = tex_create(a, 4, 4, 0);
    LitUniforms lit;
    mem_zero(&lit, sizeof(lit));
    lit.tex = tex;
    lit.ambient = 0.25f;
    GpuProgram vert, frag;
    mem_zero(&vert, sizeof(vert));
    mem_zero(&frag, sizeof(frag));
    vert.id = 1;
    frag.id = 3;
    frag.wgsl.uniform_size = 80;
    frag.wgsl.tex_count = 1;
    frag.wgsl.tex_ofs[0] = (u32)__builtin_offsetof(LitUniforms, tex);
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 8, &vert));
    gcmd_frame(&c, 320, 200, 0xFF112233u, 1.0f, 4);
    VertUniforms v1 = vu_make(v3(1, 2, 3), 1.5f), v2 = vu_make(v3(-1, 0, 0), 2.5f);
    gcmd_draw(&c, cube, &v1, NULL, &frag, &lit, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_draw(&c, cube, &v2, &vert, &frag, &lit, CULL_NONE, TRUE, FALSE, FALSE);   /* aynı mesh, doku, materyal */
    gcmd_draw(&c, plane, &v1, NULL, NULL, NULL, CULL_FRONT, FALSE, FALSE, FALSE);  /* düz shader: doku yok */
    gcmd_end(&c);
    const u32* w = c.words;
    /* yuvalar: v0, f0, v1 (f0 paylaşılır), v2, f(düz) */
    CHECK(w[0] == GCMD_MAGIC && w[1] == GCMD_VERSION && w[2] == c.count && w[3] == 3u);
    CHECK(w[4] == (u32)(uptr)c.uniforms && w[5] == 5u * GCMD_SLOT && (w[4] % GCMD_SLOT) == 0u);
    /* FRAME */
    CHECK(w[6] == GC_FRAME && w[7] == 320u && w[8] == 200u && w[9] == 0xFF112233u && bits_f32(w[10]) == 1.0f && w[11] == 4u);
    /* kayıtları yürü: MESH, TEX, DRAW, DRAW, MESH, DRAW, END */
    u32 i = 12, ops[8], n = 0;
    const u32* draws[3];
    u32 nd = 0;
    while (i < c.count && n < 8) {
        u32 op = w[i];
        ops[n++] = op;
        if (op == GC_END) break;
        if (op == GC_MESH) {
            const Mesh* m = (const Mesh*)(uptr)w[i + 1];
            CHECK(w[i + 2] == m->gen && w[i + 3] == m->vertex_count && w[i + 4] == m->index_count);
            CHECK(w[i + 5] == (u32)(uptr)m->px && w[i + 11] == (u32)(uptr)m->u && w[i + 12] == (u32)(uptr)m->v && w[i + 13] == (u32)(uptr)m->indices);
            i += 14;
        } else if (op == GC_TEX) {
            CHECK(w[i + 1] == (u32)(uptr)tex && w[i + 2] == (u32)(uptr)tex->px && w[i + 3] == 16u);
            i += 4;
        } else if (op == GC_DRAW) {
            if (nd < 3) draws[nd++] = &w[i];
            i += 11;
        } else {
            break;
        }
    }
    CHECK(n == 7 && ops[0] == GC_MESH && ops[1] == GC_TEX && ops[2] == GC_DRAW && ops[3] == GC_DRAW && ops[4] == GC_MESH && ops[5] == GC_DRAW && ops[6] == GC_END);
    CHECK(i + 1 == c.count && nd == 3);
    /* mesh, köşe yuvası, parça yuvası, köşe prog, parça prog, dokular, cull, derinlik */
    const u32* d = draws[0];
    CHECK(d[1] == (u32)(uptr)cube && d[2] == 0u && d[3] == 1u && d[4] == 1u && d[5] == 3u && d[6] == (u32)(uptr)tex && d[7] == 0u);
    d = draws[1];
    CHECK(d[2] == 2u && d[3] == 1u && d[8] == CULL_NONE && d[9] == 1u && d[10] == 0u);
    d = draws[2];
    CHECK(d[1] == (u32)(uptr)plane && d[2] == 3u && d[3] == 4u && d[5] == GPROG_FLAT && d[6] == 0u && d[8] == CULL_FRONT && d[9] == 0u);
    /* uniform içerikleri: köşe = VertUniforms ham, parça = programın boyu kadar, kalanı sıfır */
    CHECK(mem_equal(c.uniforms + 2u * GCMD_SLOT, &v2, sizeof(v2)));
    const u8* f0 = c.uniforms + GCMD_SLOT;
    CHECK(mem_equal(f0, &lit, 80));
    b32 tail_zero = TRUE;
    for (u32 k = 80; k < GCMD_SLOT; k++) tail_zero = tail_zero && f0[k] == 0;
    CHECK(tail_zero);
}

/* mip zincirli doku: px sözcük sayısı tüm seviyeleri kapsar */
TEST(gcmd_texture_words_cover_mips) {
    Arena* a = test_arena();
    Texture* t = tex_create(a, 8, 4, TEX_FLAG_MIPMAP);
    tex_build_mips(t);
    CHECK(t->levels == 4u);
    Mesh* cube = mesh_cube(a);
    GpuProgram vert, frag;
    mem_zero(&vert, sizeof(vert));
    mem_zero(&frag, sizeof(frag));
    frag.id = 2;
    frag.wgsl.uniform_size = 4;
    frag.wgsl.tex_count = 1;
    const Texture* ptr = t;
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 2, &vert));
    gcmd_frame(&c, 64, 64, 0, 1.0f, 1);
    VertUniforms v = vu_make(v3(0, 0, 0), 0.0f);
    gcmd_draw(&c, cube, &v, NULL, &frag, &ptr, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_end(&c);
    /* FRAME 6 + MESH 14 → TEX; 8×4 + 4×2 + 2×1 + 1×1 = 43 */
    const u32* r = c.words + 6 + 6 + 14;
    CHECK(r[0] == GC_TEX && r[3] == 43u);
    CHECK(c.words[6 + 6] == GC_MESH);
}

TEST(gcmd_overflow_and_unique_gen) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    GpuProgram vert;
    mem_zero(&vert, sizeof(vert));
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 2, &vert));
    gcmd_frame(&c, 64, 64, 0, 1.0f, 1);
    VertUniforms v = vu_make(v3(0, 0, 0), 0.0f);
    for (u32 k = 0; k < 5; k++) gcmd_draw(&c, cube, &v, NULL, NULL, NULL, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_end(&c);
    CHECK(c.draws == 2u && c.overflow == 3u && c.words[c.count - 1] == GC_END && c.words[3] == 2u);
    /* köşe programı yoksa (derlenemedi) çizim atlanır */
    GpuCmd c0;
    CHECK(gcmd_begin(&c0, a, 2, NULL));
    gcmd_draw(&c0, cube, &v, NULL, NULL, NULL, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_end(&c0);
    CHECK(c0.draws == 0u && c0.overflow == 1u);
    /* gen tekil: değişiklik sürüm verir; aynı adrese yeniden ayrılan mesh öncekinden farklı gen alır */
    u32 g0 = cube->gen;
    mesh_compute_bounds(cube);
    CHECK(cube->gen != g0);
    Arena sub = arena_sub(a, 1 << 16);
    usize mark = arena_mark(&sub);
    Mesh* m1 = mesh_alloc(&sub, 4, 6);
    u32 g1 = m1->gen;
    arena_reset(&sub, mark);
    Mesh* m2 = mesh_alloc(&sub, 4, 6);
    CHECK(m1 == m2 && m2->gen != g1);
}

/* gölge + ana geçiş + bindirme sırası: gölge dokusu bir kez GC_SHADOW ile bildirilir, malzemede geçse de GC_TEX yazılmaz */
TEST(gcmd_shadow_and_overlay_stream) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    Texture* shadow = tex_create(a, 4, 4, TEX_FLAG_CLAMP);
    Texture* tex = tex_create(a, 2, 2, 0);
    LitUniforms lit;
    mem_zero(&lit, sizeof(lit));
    lit.tex = tex;
    lit.shadow = shadow;
    GpuProgram vert, frag;
    mem_zero(&vert, sizeof(vert));
    mem_zero(&frag, sizeof(frag));
    vert.id = 2;
    frag.id = 5;
    frag.wgsl.uniform_size = 160;
    frag.wgsl.tex_count = 2;
    frag.wgsl.tex_ofs[0] = (u32)__builtin_offsetof(LitUniforms, tex);
    frag.wgsl.tex_ofs[1] = (u32)__builtin_offsetof(LitUniforms, shadow);
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 4, &vert));
    VertUniforms v = vu_make(v3(0, 0, 0), 0.0f);
    gcmd_shadow_begin(&c, shadow, 1024, 0, 2);
    gcmd_draw(&c, cube, &v, NULL, NULL, NULL, CULL_FRONT, TRUE, TRUE, TRUE);
    gcmd_shadow_end(&c);
    gcmd_frame(&c, 64, 64, 0, 1.0f, 1);
    gcmd_draw(&c, cube, &v, NULL, &frag, &lit, CULL_BACK, TRUE, TRUE, FALSE);
    u32 ovpx[6] = { 0 };
    gcmd_overlay(&c, 8, 9, 3, 2, ovpx);
    gcmd_end(&c);
    const u32* w = c.words;
    u32 i = 6;
    CHECK(w[i] == GC_SHADOW && w[i + 1] == (u32)(uptr)shadow && w[i + 2] == 1024u && w[i + 3] == 0u && w[i + 4] == 2u);
    i += 5;
    CHECK(w[i] == GC_MESH);
    i += 14;
    CHECK(w[i] == GC_DRAW && w[i + 5] == GPROG_NONE && w[i + 6] == 0u && w[i + 8] == CULL_FRONT);
    i += 11;
    CHECK(w[i] == GC_SHADOW_END);
    i += 1;
    CHECK(w[i] == GC_FRAME);
    i += 6;
    CHECK(w[i] == GC_TEX && w[i + 1] == (u32)(uptr)tex);   /* yalnız renk dokusu */
    i += 4;
    CHECK(w[i] == GC_DRAW && w[i + 5] == 5u && w[i + 6] == (u32)(uptr)tex && w[i + 7] == (u32)(uptr)shadow);
    i += 11;
    CHECK(w[i] == GC_OVERLAY && w[i + 1] == 8u && w[i + 2] == 9u && w[i + 3] == 3u && w[i + 4] == 2u && w[i + 5] == (u32)(uptr)ovpx);
    i += 6;
    CHECK(w[i] == GC_END && i + 1 == c.count);
}

/* raster / hud karışımı (a·s + (255 − a)·d) / 255 ile bindirme: panel + metnin önceden çarpılmış sonucu her zeminde ±2 içinde */
TEST(gcmd_overlay_extract_matches_blend) {
    static const u32 ALPHA[] = { 0, 1, 64, 128, 170, 220, 254, 255 };
    u32 worst = 0, bad_a = 0;
    for (u32 k = 0; k < ARRAY_COUNT(ALPHA); k++) {
        u32 al = ALPHA[k];
        for (u32 s0 = 0; s0 <= 255; s0 += 51) {
            u32 src = s0 * 257u & 0xFF;
            u32 b = (al * src + (255u - al) * 0u + 127u) / 255u, w = (al * src + (255u - al) * 255u + 127u) / 255u;
            u32 on_b = b | (b << 8) | (b << 16) | 0xFF000000u, on_w = w | (w << 8) | (w << 16) | 0xFF000000u, out;
            gcmd_overlay_extract(&on_b, &on_w, &out, 1);
            u32 a2 = out >> 24, c2 = out & 0xFFu;
            if (a2 != al) bad_a++;
            for (u32 d = 0; d <= 255; d += 17) {
                u32 want = (al * src + (255u - al) * d + 127u) / 255u;
                u32 got = c2 + ((255u - a2) * d + 127u) / 255u;
                u32 e = want > got ? want - got : got - want;
                if (e > worst) worst = e;
            }
        }
    }
    test_note("bindirme: en büyük kanal hatası", (f64)worst, 0);
    CHECK(bad_a == 0u);
    CHECK(worst <= 2u);
}

/* yansıma: doku GC_TEX yazılmaz, sonrasında aynı malzeme yeni yuva alır (göz geçişler arasında değişir) */
TEST(gcmd_reflect_stream) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    Texture* refl = tex_create(a, 4, 4, TEX_FLAG_CLAMP);
    LitUniforms lit;
    mem_zero(&lit, sizeof(lit));
    lit.tex = refl;
    GpuProgram vert, frag;
    mem_zero(&vert, sizeof(vert));
    mem_zero(&frag, sizeof(frag));
    vert.id = 2;
    frag.id = 5;
    frag.wgsl.uniform_size = 160;
    frag.wgsl.tex_count = 1;
    frag.wgsl.tex_ofs[0] = (u32)__builtin_offsetof(LitUniforms, tex);
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 4, &vert));
    VertUniforms v = vu_make(v3(0, 0, 0), 0.0f);
    gcmd_reflect_begin(&c, refl, 4, 4);
    gcmd_draw(&c, cube, &v, NULL, &frag, &lit, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_reflect_end(&c);
    gcmd_frame(&c, 64, 64, 0, 1.0f, 1);
    gcmd_draw(&c, cube, &v, NULL, &frag, &lit, CULL_BACK, TRUE, TRUE, FALSE);
    gcmd_end(&c);
    const u32* w = c.words;
    u32 i = 6;
    CHECK(w[i] == GC_REFLECT && w[i + 1] == (u32)(uptr)refl && w[i + 2] == 4u && w[i + 3] == 4u);
    i += 4;
    CHECK(w[i] == GC_MESH);
    i += 14;
    CHECK(w[i] == GC_DRAW);
    u32 slot0 = w[i + 3];
    i += 11;
    CHECK(w[i] == GC_REFLECT_END);
    i += 1;
    CHECK(w[i] == GC_FRAME);
    i += 6;
    CHECK(w[i] == GC_DRAW);
    CHECK(w[i + 3] != slot0);
}
