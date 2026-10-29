/* tests/test_asset_gltf.c — base64 ve glTF/GLB yükleyici testleri. Varlıklar tests/assets/make_assets.py
 * ile üretilir: gömülü base64 .gltf (iç içe byteStride, u16 indeks, TRS hiyerarşisi, data: PNG),
 * GLB (BIN parçası, u32 indeks, normalized u16 UV, matrix node, bufferView PNG), u8 indeks + üretilen normal,
 * bozuk dosyalar ve GLB bayt mutasyon fuzz'u (ASan altında sınır dışı okuma yok).
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_asset_gltf.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "test.h"
#include "core/sys.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/embed.h"
#include "core/arena.h"
#include "core/rng.h"
#include "core/math.h"
#include "raster/raster.h"
#include "asset/base64.h"
#include "asset/gltf.h"


/* gömülü varlık (build.py test, tests/assets altı) → dst; yoksa 0 */
static u32 read_bin_file(const char* path, u8* dst, u32 cap) {
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/') name = p + 1;
    const EmbedFile* f = embed_find(name);
    if (!f || f->len > cap) return 0u;
    mem_copy(dst, f->data, f->len);
    return f->len;
}

static b32 t_streq(const char* a, const char* b) {
    usize n = cstr_len(a);
    return n == cstr_len(b) && mem_equal(a, b, n);
}

static u8* g_mem;   /* MB(24), her TEST başında test_arena'dan */
static u8* g_file;   /* MB(1), her TEST başında test_arena'dan */

static u32 read_file(const char* path) { return read_bin_file(path, g_file, MB(1)); }

static b32 load(TestCtx* t_, const char* path, GltfModel* m, Arena* out, Arena* scratch) {
    u32 n = read_file(path);
    CHECK(n > 0);
    arena_init(out, g_mem, MB(8));
    arena_init(scratch, g_mem + MB(8), MB(16));
    return gltf_load(g_file, n, out, scratch, NULL, NULL, m);
}

TEST(base64_vectors) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    static const char* in[]  = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy", "Zm9v\nYmFy" };
    static const char* out[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar", "foobar" };
    u8 buf[16];
    for (u32 i = 0; i < ARRAY_COUNT(in); i++) {
        u32 n = 0;
        CHECK(base64_decode(in[i], (u32)cstr_len(in[i]), buf, sizeof(buf), &n));
        CHECK(n == cstr_len(out[i]) && mem_equal(buf, out[i], n));
    }
    u32 n;
    CHECK(!base64_decode("Zm9v!", 5, buf, sizeof(buf), &n));
    CHECK(!base64_decode("Zg=a", 4, buf, sizeof(buf), &n));
    CHECK(!base64_decode("Zh==", 4, buf, sizeof(buf), &n));     /* artık bitler sıfır değil */
}

static Vec3 world_of(const GltfDraw* d, u32 i) {
    Vec4 p = mat4_mul_v4(&d->world, v4(d->mesh->px[i], d->mesh->py[i], d->mesh->pz[i], 1.0f));
    return v3(p.x, p.y, p.z);
}

TEST(gltf_embedded_hierarchy_texture) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    Arena o, s;
    GltfModel m;
    b32 ok = load(t_, "tests/assets/cube_embedded.gltf", &m, &o, &s);
    if (!ok) log_printf("gltf hata: %s", m.err);
    CHECK(ok);
    if (!ok) return;
    CHECK(m.draw_count == 1 && m.triangles == 12);
    CHECK(m.draws[0].mesh->vertex_count == 24);
    /* T(0,1,0) * RotY(90°) * S(2): (0.5,0.5,0.5) → (1,1,1) → (1,1,-1) → (1,2,-1) */
    const Mesh* me = m.draws[0].mesh;
    u32 found = 0;
    for (u32 i = 0; i < me->vertex_count; i++) {
        if (me->px[i] == 0.5f && me->py[i] == 0.5f && me->pz[i] == 0.5f) {
            Vec3 w = world_of(&m.draws[0], i);
            CHECK_NEAR(w.x, 1.0f, 1e-5); CHECK_NEAR(w.y, 2.0f, 1e-5); CHECK_NEAR(w.z, -1.0f, 1e-5);
            found++;
        }
    }
    CHECK(found == 3);
    CHECK_NEAR(m.bounds_min.y, 0.0f, 1e-5);
    CHECK_NEAR(m.bounds_max.y, 2.0f, 1e-5);
    /* Materyal ve doku */
    CHECK(m.material_count == 1);
    CHECK_NEAR(m.materials[0].base_color.y, 0.5f, 1e-6);
    CHECK(m.materials[0].base_texture == 0);
    const Texture* t = &m.textures[0];
    CHECK(t->w == 4 && t->h == 4 && t->levels == 3);
    CHECK(t->flags & TEX_FLAG_CLAMP);
    CHECK(t->px[0] == color_pack(255, 40, 40, 255));
    CHECK(t->px[1] == color_pack(40, 40, 255, 255));
    /* İç içe normaller: köşe 0 +x yüzünde */
    CHECK_NEAR(me->nx[0], 1.0f, 1e-6);
}

TEST(gltf_glb_binary) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    Arena o, s;
    GltfModel m;
    b32 ok = load(t_, "tests/assets/cube_binary.glb", &m, &o, &s);
    if (!ok) log_printf("glb hata: %s", m.err);
    CHECK(ok);
    if (!ok) return;
    CHECK(m.triangles == 12);
    CHECK_NEAR(m.bounds_min.x, 2.0f, 1e-5);   /* matrix: x+3, ölçek 2 → [2, 4] */
    CHECK_NEAR(m.bounds_max.x, 4.0f, 1e-5);
    const Mesh* me = m.draws[0].mesh;
    /* normalized u16 UV: (0,1) köşesi */
    CHECK_NEAR(me->u[0], 0.0f, 1e-6);
    CHECK_NEAR(me->v[0], 1.0f, 1e-6);
    CHECK(m.textures[0].px != NULL && m.textures[0].px[0] == color_pack(255, 40, 40, 255));
}

TEST(gltf_u8_indices_generated_normals) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    Arena o, s;
    GltfModel m;
    b32 ok = load(t_, "tests/assets/cube_u8.gltf", &m, &o, &s);
    if (!ok) log_printf("u8 hata: %s", m.err);
    CHECK(ok);
    if (!ok) return;
    const Mesh* me = m.draws[0].mesh;
    CHECK(me->index_count == 36);
    f32 l = m_sqrtf(me->nx[0] * me->nx[0] + me->ny[0] * me->ny[0] + me->nz[0] * me->nz[0]);
    CHECK_NEAR(l, 1.0f, 1e-5);
    CHECK_NEAR(me->nx[0], 1.0f, 1e-5);      /* +x yüzü: üretilen normal de +x */
    CHECK_NEAR(me->u[2], 1.0f, 1e-6);       /* normalized u8 */
}

TEST(gltf_bad_files) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    Arena o, s;
    GltfModel m;
    CHECK(!load(t_, "tests/assets/bad_truncated.glb", &m, &o, &s) && m.err[0]);
    CHECK(!load(t_, "tests/assets/bad_accessor.gltf", &m, &o, &s) && m.err[0]);
    CHECK(!load(t_, "tests/assets/bad_version.gltf", &m, &o, &s) && m.err[0]);
}

TEST(gltf_glb_fuzz) {
    g_mem = (u8*)arena_alloc_nozero(test_arena(), MB(24), 16);
    g_file = (u8*)arena_alloc_nozero(test_arena(), MB(1), 16);
    static u8 orig[4096], buf[4096];
    u32 n = read_file("tests/assets/cube_binary.glb");
    CHECK(n > 0 && n <= sizeof(orig));
    mem_copy(orig, g_file, n);
    Rng r;
    (*(&r) = rng_make(4242));
    Arena o, s;
    u32 ok = 0;
    for (u32 it = 0; it < 20000; it++) {
        mem_copy(buf, orig, n);
        u32 muts = 1u + rng_u32(&r) % 6u;
        for (u32 k = 0; k < muts; k++) buf[rng_u32(&r) % n] = (u8)rng_u32(&r);
        arena_init(&o, g_mem, MB(8));
        arena_init(&s, g_mem + MB(8), MB(16));
        GltfModel m;
        if (gltf_load(buf, n, &o, &s, NULL, NULL, &m)) {
            ok++;
            for (u32 d = 0; d < m.draw_count; d++) {
                const Mesh* me = m.draws[d].mesh;
                for (u32 i = 0; i < me->index_count; i++) CHECK(me->indices[i] < me->vertex_count);
            }
        }
    }
    CHECK(ok > 0);
}
