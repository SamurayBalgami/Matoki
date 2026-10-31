/* tests/test_gcmd.c — M13 faz 8a: GPU komut akışı (gpu/gcmd). Başlık + kayıt düzeni (web/gpu.mjs bunu okur), mesh
 * kaydı karede bir kez, uniform yuvaları 256 bayt hizalı ve içerik doğru, taşmada akış kapanır ve sayar; mesh sürümü
 * tüm mesh'ler arası tekil (aynı adrese yeni mesh gelse de farklı gen). */
#include "test.h"
#include "core/mem.h"
#include "gpu/gcmd.h"

static f32 bits_f32(u32 b) {
    f32 f;
    mem_copy(&f, &b, 4);
    return f;
}

TEST(gcmd_stream_layout_and_dedup) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    Mesh* plane = mesh_plane(a, 4.0f, 4.0f, 1.0f, 2);
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 8, 320, 200, 0xFF112233u, 1.0f, GPU_MODE_FLAT));
    Mat4 vp = mat4_perspective(1.0f, 1.6f, 0.1f, 100.0f);
    Mat4 m1 = mat4_translate(v3(1, 2, 3)), m2 = mat4_translate(v3(-1, 0, 0));
    Mat3 nm = mat3_identity();
    gcmd_draw(&c, cube, &m1, &vp, &nm, CULL_BACK, TRUE, TRUE);
    gcmd_draw(&c, cube, &m2, &vp, &nm, CULL_NONE, TRUE, FALSE);   /* aynı mesh: MESH kaydı yok */
    gcmd_draw(&c, plane, &m1, &vp, &nm, CULL_FRONT, FALSE, FALSE);
    gcmd_end(&c);
    const u32* w = c.words;
    CHECK(w[0] == GCMD_MAGIC && w[1] == GCMD_VERSION && w[2] == c.count && w[3] == 3u);
    CHECK(w[4] == (u32)(uptr)c.uniforms && w[5] == 3u * GCMD_SLOT && (w[4] % GCMD_SLOT) == 0u);
    /* FRAME */
    CHECK(w[6] == GC_FRAME && w[7] == 320u && w[8] == 200u && w[9] == 0xFF112233u && bits_f32(w[10]) == 1.0f && w[11] == GPU_MODE_FLAT);
    /* kayıtları yürü: MESH, DRAW, DRAW, MESH, DRAW, END */
    u32 i = 12, ops[8], n = 0;
    while (i < c.count && n < 8) {
        u32 op = w[i];
        ops[n++] = op;
        if (op == GC_END) break;
        if (op == GC_MESH) {
            const Mesh* m = (const Mesh*)(uptr)w[i + 1];
            CHECK(w[i + 2] == m->gen && w[i + 3] == m->vertex_count && w[i + 4] == m->index_count);
            CHECK(w[i + 5] == (u32)(uptr)m->px && w[i + 11] == (u32)(uptr)m->indices);
            i += 12;
        } else if (op == GC_DRAW) {
            i += 6;
        } else {
            break;
        }
    }
    CHECK(n == 6 && ops[0] == GC_MESH && ops[1] == GC_DRAW && ops[2] == GC_DRAW && ops[3] == GC_MESH && ops[4] == GC_DRAW && ops[5] == GC_END);
    CHECK(i + 1 == c.count);
    /* ikinci DRAW: yuva 1, cull NONE, derinlik yazma kapalı; uniform içeriği */
    const u32* d1 = &w[12 + 12 + 6];
    CHECK(d1[0] == GC_DRAW && d1[1] == (u32)(uptr)cube && d1[2] == 1u && d1[3] == CULL_NONE && d1[4] == 1u && d1[5] == 0u);
    const GpuDrawUniforms* u1 = (const GpuDrawUniforms*)(c.uniforms + GCMD_SLOT);
    CHECK(u1->model.m[12] == -1.0f && mem_equal(&u1->view_proj, &vp, sizeof(Mat4)));
    CHECK(u1->normal_mat[0] == 1.0f && u1->normal_mat[5] == 1.0f && u1->normal_mat[10] == 1.0f && u1->normal_mat[3] == 0.0f);
}

TEST(gcmd_overflow_and_unique_gen) {
    Arena* a = test_arena();
    Mesh* cube = mesh_cube(a);
    GpuCmd c;
    CHECK(gcmd_begin(&c, a, 2, 64, 64, 0, 1.0f, GPU_MODE_NORMAL));
    Mat4 id = mat4_identity();
    Mat3 nm = mat3_identity();
    for (u32 k = 0; k < 5; k++) gcmd_draw(&c, cube, &id, &id, &nm, CULL_BACK, TRUE, TRUE);
    gcmd_end(&c);
    CHECK(c.draws == 2u && c.overflow == 3u && c.words[c.count - 1] == GC_END && c.words[3] == 2u);
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
