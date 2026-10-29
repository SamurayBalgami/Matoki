/* tests/test_matoki_vert.c — Matoki vertex aşaması ve paralel geometri.
 * std_vert.mtk ↔ vert_std_c bit-bit (ir_eval, JIT, önsöz + hızlı gövde); aşama hataları; varying'lerin if ile
 * birleşmesi; vertex doku okuması; boru hattında Matoki vert = C vert ve tek thread = tüm thread'ler. */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/jobs.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "gfx3d/pipeline.h"
#include "matoki/ir_eval.h"
#include "matoki/matoki.h"

static b32 compile_stage(const char* src, u32 len, u32 stage, MatokiProgram* p, MatokiError* e) {
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    o.stage = stage;
    return matoki_compile(test_arena(), src, len, &o, p, e) && matoki_jit(test_arena(), p, e);
}

static b32 compile_vert_src(TestCtx* t_, const char* src, MatokiProgram* p) {
    MatokiError e;
    b32 ok = compile_stage(src, (u32)cstr_len(src), MATOKI_STAGE_VERT, p, &e);
    if (!ok) {
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, "    derleme hatası: ");
        matoki_format_error(&e, &sb);
        log_sb(&sb);
    }
    CHECK(ok);
    return ok;
}

static b32 compile_vert_file(TestCtx* t_, const char* name, MatokiProgram* p) {
    const EmbedFile* ef = embed_find(name);
    CHECK(ef != NULL);
    if (!ef) return FALSE;
    MatokiError e;
    b32 ok = compile_stage((const char*)ef->data, ef->len, MATOKI_STAGE_VERT, p, &e);
    if (!ok) {
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, "    ");
        sb_cstr(&sb, name);
        sb_cstr(&sb, ": ");
        matoki_format_error(&e, &sb);
        log_sb(&sb);
    }
    CHECK(ok);
    return ok;
}

static void random_vu(Rng* r, VertUniforms* u) {
    mem_zero(u, sizeof(*u));
    Mat4 rot = mat4_mul(mat4_rotate_y(rng_range(r, -3, 3)), mat4_rotate_x(rng_range(r, -1, 1)));
    u->model = mat4_mul(mat4_translate(v3(rng_range(r, -20, 20), rng_range(r, -5, 5), rng_range(r, -20, 20))),
                        mat4_mul(rot, mat4_scale(v3(rng_range(r, 0.2f, 3), rng_range(r, 0.2f, 3), rng_range(r, 0.2f, 3)))));
    Mat4 view = mat4_look_at(v3(rng_range(r, -30, 30), rng_range(r, 1, 20), rng_range(r, -30, 30)), v3(0, 0, 0), v3(0, 1, 0));
    u->view_proj = mat4_mul(mat4_perspective(1.0f, 16.0f / 9.0f, 0.1f, 500.0f), view);
    u->normal_mat = mat3_normal_from_mat4(&u->model);
    u->time = rng_range(r, 0, 100);
}

static void random_vin(Rng* r, VertexIn* in) {
    mem_zero(in, sizeof(*in));
    in->count = 4;
    for (u32 k = 0; k < ATTR3D_COUNT; k++) {
        for (u32 l = 0; l < 4; l++) in->attr[k][l] = rng_range(r, -4, 4);
    }
}

/* VertexOut'un pos + ilk n varying'i bit-bit eşit mi */
static b32 vout_equal(const VertexOut* a, const VertexOut* b, u32 nvar) {
    if (!mem_equal(a->pos, b->pos, sizeof(a->pos))) return FALSE;
    return mem_equal(a->var, b->var, sizeof(f32) * 4 * nvar);
}

TEST(matoki_std_vert_matches_c) {
    MatokiProgram p;
    if (!compile_vert_file(t_, "std_vert.mtk", &p)) return;
    CHECK(matoki_find_uniform(&p, "model")->offset == __builtin_offsetof(VertUniforms, model));
    CHECK(matoki_find_uniform(&p, "view_proj")->offset == __builtin_offsetof(VertUniforms, view_proj));
    CHECK(matoki_find_uniform(&p, "normal_mat")->offset == __builtin_offsetof(VertUniforms, normal_mat));
    CHECK(matoki_find_uniform(&p, "time")->offset == __builtin_offsetof(VertUniforms, time));
    CHECK(matoki_find_attribute(&p, "position")->offset == ATTR3D_POS);
    CHECK(matoki_find_attribute(&p, "normal")->offset == ATTR3D_NRM);
    CHECK(matoki_find_attribute(&p, "texcoord")->offset == ATTR3D_UV);
    Arena scratch;
    arena_init(&scratch, arena_alloc(test_arena(), 1 << 20, 16), 1 << 20);
    Rng r = rng_make(0x7E57);
    u32 bad_eval = 0, bad_jit = 0, bad_fast = 0;
    VertUniforms u;
    for (u32 it = 0; it < 5000; it++) {
        if (it % 50 == 0) {
            random_vu(&r, &u);
            p.prelude(&u);
        }
        VertexIn in;
        random_vin(&r, &in);
        VertexOut ref, ev, jt, ft;
        vert_std_c(&in, &u, &ref);
        ir_eval_io(&p.ir, &in, &u, &ev, &scratch);
        p.fn((const ShadeQuad*)&in, &u, (u32*)&jt);
        ((VertexFn)p.fn_fast)(&in, &u, &ft);
        bad_eval += !vout_equal(&ref, &ev, VAR3D_COUNT);
        bad_jit += !vout_equal(&ref, &jt, VAR3D_COUNT);
        bad_fast += !vout_equal(&ref, &ft, VAR3D_COUNT);
    }
    test_note("IR komutu (canlı)", p.ir_live, 0);
    test_note("önsöze alınan", p.wasm.hoisted, 0);
    CHECK(bad_eval == 0);
    CHECK(bad_jit == 0);
    CHECK(bad_fast == 0);
}

typedef struct StageErr {
    const char* src;
    u32 stage;
    const char* msg;
} StageErr;

TEST(matoki_vert_stage_errors) {
    static const StageErr cases[] = {
        { "attribute p: vec3;\nvarying v: f32;\nfn vert() -> vec4 { v = dFdx(p.x); return vec4(p, 1.0); }\n", MATOKI_STAGE_VERT, "vert aşamasında kullanılamaz" },
        { "attribute p: vec3;\nfn vert() -> vec4 { if (p.x > 0.0) { discard; } return vec4(p, 1.0); }\n", MATOKI_STAGE_VERT, "discard yalnızca frag" },
        { "attribute p: vec3;\nfn frag() -> vec4 { return vec4(p, 1.0); }\n", MATOKI_STAGE_FRAG, "attribute yalnızca vert" },
        { "varying v: f32;\nfn frag() -> vec4 { v = 1.0; return vec4(v); }\n", MATOKI_STAGE_FRAG, "varying'e yalnızca vert" },
        { "fn frag() -> vec4 { return vec4(1.0); }\n", MATOKI_STAGE_VERT, "giriş noktası yok: fn vert" },
        { "attribute p: vec3;\nfn vert() -> vec3 { return p; }\n", MATOKI_STAGE_VERT, "fn vert() -> vec4" },
        { "uniform t: texture2d @mipmap;\nattribute p: vec3;\nvarying c: vec4;\nfn vert() -> vec4 { c = sample(t, p.xy); return vec4(p, 1.0); }\n", MATOKI_STAGE_VERT, "sample_level" },
        { "attribute p: mat4;\nfn vert() -> vec4 { return vec4(1.0); }\n", MATOKI_STAGE_VERT, "attribute f32/vecN" },
    };
    for (u32 i = 0; i < ARRAY_COUNT(cases); i++) {
        MatokiProgram p;
        MatokiError e;
        MatokiOptions o;
        mem_zero(&o, sizeof(o));
        o.stage = cases[i].stage;
        b32 ok = matoki_compile(test_arena(), cases[i].src, (u32)cstr_len(cases[i].src), &o, &p, &e);
        b32 found = FALSE;
        usize n = cstr_len(cases[i].msg), h = cstr_len(e.msg);
        for (usize k = 0; k + n <= h && !ok; k++) found |= mem_equal(e.msg + k, cases[i].msg, n);
        CHECK(!ok);
        CHECK(found);
        if (ok || !found) {
            char buf[300];
            StrBuf sb;
            sb_init(&sb, buf, sizeof(buf));
            sb_cstr(&sb, "    vaka ");
            sb_u64(&sb, i);
            sb_cstr(&sb, " → ");
            matoki_format_error(&e, &sb);
            log_sb(&sb);
        }
    }
}

TEST(matoki_vert_control_flow_and_texture) {
    /* varying if ile birleşir; yazılmayan dal 0; bileşik atama swizzle'da; doku okuması (sample_level) */
    const char* src =
        "uniform h: texture2d @clamp;\nuniform s: f32;\n"
        "attribute p: vec3;\n"
        "varying a: vec2;\nvarying b: f32;\nvarying c: f32;\n"
        "fn vert() -> vec4 {\n"
        "  if (p.x > 0.0) { a = vec2(1.0, p.y); } else { a.y = 2.0; }\n"
        "  a.x += 0.5;\n"
        "  if (p.z > 1.0) { b = p.z * s; }\n"
        "  c = sample_level(h, vec2(p.x, 0.5), 0).r;\n"
        "  return vec4(p, 1.0);\n"
        "}\n";
    MatokiProgram p;
    if (!compile_vert_src(t_, src, &p)) return;
    Texture* tex = tex_create(test_arena(), 4, 1, TEX_FLAG_CLAMP);
    for (u32 i = 0; i < 4; i++) tex->px[i] = color_pack(i * 60, 0, 0, 255);
    struct { const Texture* t; f32 s; } uni = { tex, 3.0f };
    Arena scratch;
    arena_init(&scratch, arena_alloc(test_arena(), 1 << 20, 16), 1 << 20);
    Rng r = rng_make(5);
    u32 bad = 0;
    for (u32 it = 0; it < 2000; it++) {
        VertexIn in;
        mem_zero(&in, sizeof(in));
        in.count = 4;
        for (u32 l = 0; l < 4; l++) {
            in.attr[0][l] = rng_range(&r, -1, 1);
            in.attr[1][l] = rng_range(&r, -1, 1);
            in.attr[2][l] = rng_range(&r, 0, 2);
        }
        VertexOut ev, jt;
        ir_eval_io(&p.ir, &in, &uni, &ev, &scratch);
        p.fn((const ShadeQuad*)&in, &uni, (u32*)&jt);
        if (!vout_equal(&ev, &jt, 4)) bad++;
        for (u32 l = 0; l < 4; l++) {
            f32 x = in.attr[0][l], y = in.attr[1][l], z = in.attr[2][l];
            f32 ax = (x > 0.0f ? 1.0f : 0.0f) + 0.5f, ay = x > 0.0f ? y : 2.0f;
            f32 b = z > 1.0f ? z * 3.0f : 0.0f;
            i32 ix = CLAMP(m_f2i_sat(m_floorf(x * 4.0f)), 0, 3);
            f32 c = (f32)(ix * 60) * (1.0f / 255.0f);
            if (ev.var[0][l] != ax || ev.var[1][l] != ay || ev.var[2][l] != b || ev.var[3][l] != c) bad++;
            if (ev.pos[0][l] != x || ev.pos[3][l] != 1.0f) bad++;
        }
    }
    CHECK(bad == 0);
}

TEST(matoki_wave_vert_compiles_and_moves) {
    MatokiProgram p;
    if (!compile_vert_file(t_, "wave_vert.mtk", &p)) return;
    VertUniforms u;
    mem_zero(&u, sizeof(u));
    u.model = mat4_identity();
    u.view_proj = mat4_identity();
    u.normal_mat = mat3_normal_from_mat4(&u.model);
    u.params[0] = 0.4f;   /* amp */
    u.params[1] = 8.0f;   /* wavelen */
    u.params[2] = 1.0f;   /* speed */
    u.params[3] = 0.6f;   /* steep */
    VertexIn in;
    mem_zero(&in, sizeof(in));
    in.count = 4;
    for (u32 l = 0; l < 4; l++) {
        in.attr[0][l] = (f32)l * 1.3f;
        in.attr[2][l] = (f32)l * -0.7f;
        in.attr[4][l] = 1.0f;
    }
    VertexOut a, b;
    u.time = 0.0f;
    p.prelude(&u);
    ((VertexFn)p.fn_fast)(&in, &u, &a);
    u.time = 1.7f;
    p.prelude(&u);
    ((VertexFn)p.fn_fast)(&in, &u, &b);
    u32 moved = 0, bounded = 0, normal_up = 0;
    for (u32 l = 0; l < 4; l++) {
        moved += a.pos[1][l] != b.pos[1][l];
        bounded += m_absf(b.pos[1][l]) < 0.4f + 0.4f / 1.9f + 0.4f / 2.8f + 0.4f / 3.7f + 1e-4f;
        normal_up += b.var[VAR3D_NRM + 1][l] > 0.0f;
    }
    CHECK(moved == 4);
    CHECK(bounded == 4);
    CHECK(normal_up == 4);
}

/* ---- boru hattı: Matoki vert = C vert, tek thread = tüm thread'ler ---- */

static void shade_nrm(const ShadeQuad* q, const void* u, u32 out[4]) {
    UNUSED(u);
    for (u32 l = 0; l < 4; l++) {
        u32 r = (u32)m_f2i_sat(m_clampf(q->var[VAR3D_NRM][l] * 0.5f + 0.5f, 0, 1) * 255.0f);
        u32 g = (u32)m_f2i_sat(m_clampf(m_absf(m_sinf(q->var[VAR3D_WPOS][l])), 0, 1) * 255.0f);
        u32 b = (u32)m_f2i_sat(m_clampf(q->var[VAR3D_UV][l] - m_floorf(q->var[VAR3D_UV][l]), 0, 1) * 255.0f);
        out[l] = color_pack(r, g, b, 255);
    }
}

static void render_cubes(u32* color, f32* depth, i32 w, i32 h, const Material* mat, u32 threads) {
    Framebuffer fb = { color, depth, w, h, 1, NULL, NULL };
    fb_clear(&fb, 0xFF101010u, 1.0f);
    Raster r;
    raster_init(&r, &fb);
    Arena bins, frame;
    arena_init(&bins, arena_alloc(test_arena(), 2 << 20, 16), 2 << 20);   /* küçük: birden çok parti */
    arena_init(&frame, arena_alloc(test_arena(), 4 << 20, 16), 4 << 20);
    raster_begin(&r, &bins);
    raster_set_depth(&r, TRUE, TRUE, DEPTH_LESS);
    Mat4 view = mat4_look_at(v3(6, 7, 16), v3(0, 0, -6), v3(0, 1, 0));
    Mat4 proj = mat4_perspective(1.1f, (f32)w / (f32)h, 0.1f, 300.0f);
    Pipeline p;
    jobs_set_limit(threads);
    pipe_begin(&p, &r, &frame, view, proj, v3(6, 7, 16));
    Mesh* cube = mesh_cube(test_arena());
    Rng rng = rng_make(9);
    for (u32 i = 0; i < 600; i++) {
        Mat4 m = mat4_mul(mat4_translate(v3(rng_range(&rng, -12, 12), rng_range(&rng, -4, 6), rng_range(&rng, -30, 14))),
                          mat4_mul(mat4_rotate_y(rng_range(&rng, -3, 3)), mat4_scale(v3(1.3f, 1.3f, 1.3f))));
        DrawItem d = { cube, mat, m };
        pipe_draw(&p, &d);
    }
    pipe_end(&p);
    jobs_set_limit(0);
    test_note("parti", p.batches, 0);
}

TEST(pipeline_matoki_vert_equals_c_and_threads) {
    MatokiProgram p;
    if (!compile_vert_file(t_, "std_vert.mtk", &p)) return;
    enum { W = 320, H = 180 };
    Material mc;
    mem_zero(&mc, sizeof(mc));
    mc.shade = shade_nrm;
    mc.cull = CULL_BACK;
    Material mm = mc;
    mm.vert = (VertexFn)p.fn_fast;
    mm.vert_prelude = p.prelude;
    u32* c[3];
    f32* d[3];
    for (u32 k = 0; k < 3; k++) {
        c[k] = ARENA_ARRAY(test_arena(), u32, W * H);
        d[k] = ARENA_ARRAY(test_arena(), f32, W * H);
    }
    render_cubes(c[0], d[0], W, H, &mc, 1);   /* C vert, tek thread */
    render_cubes(c[1], d[1], W, H, &mm, 1);   /* Matoki vert, tek thread */
    render_cubes(c[2], d[2], W, H, &mm, 0);   /* Matoki vert, tüm thread'ler */
    u32 painted = 0;
    for (u32 i = 0; i < W * H; i++) painted += c[0][i] != 0xFF101010u;
    test_note("boyanan piksel", painted, 0);
    CHECK(painted > W * H / 5);
    CHECK(mem_equal(c[0], c[1], sizeof(u32) * W * H));
    CHECK(mem_equal(d[0], d[1], sizeof(f32) * W * H));
    CHECK(mem_equal(c[1], c[2], sizeof(u32) * W * H));
    CHECK(mem_equal(d[1], d[2], sizeof(f32) * W * H));
}
