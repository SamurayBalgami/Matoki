/* tests/test_matoki_lang.c — Matoki dil özellikleri: discard, mat3/mat4 uniform'ları, exp2/log2/exp/log/pow.
 * discard: ir_eval ve JIT aynı kill bitlerini yazar; raster (skaler ve SIMD) atılan pikselde
 * rengi ve derinliği yazmaz. Matris çarpımı mat4_mul_v4 / mat3_mul_v3 ile, üstel/logaritma
 * m_exp2f / m_log2f ile bit-bit aynıdır; m_*f fonksiyonlarının doğruluğu ayrıca ölçülür. */
#include "test.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "matoki/ir_eval.h"
#include "matoki/matoki.h"
#include "img/image.h"
#include "raster/raster.h"
#include "raster/ref_shaders.h"

typedef struct Prog {
    MatokiProgram p;
    Arena scratch;
    b32 ok;
} Prog;

static void prog_init(TestCtx* t_, Prog* r, const char* src) {
    MatokiError e;
    r->ok = matoki_compile(test_arena(), src, (u32)cstr_len(src), NULL, &r->p, &e) && matoki_jit(test_arena(), &r->p, &e);
    if (!r->ok) {
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, "    derleme hatası: ");
        matoki_format_error(&e, &sb);
        log_sb(&sb);
    }
    CHECK(r->ok);
    arena_init(&r->scratch, arena_alloc(test_arena(), 1 << 20, 16), 1 << 20);
}

/* ir_eval ve JIT; kill bitleri de karşılaştırılır (q kopyalarında) */
static b32 prog_run(Prog* r, const ShadeQuad* q, const void* uni, u32 out[4], u32* kill) {
    ShadeQuad qe = *q, qj = *q;
    qe.kill = qj.kill = 0;
    u32 ev[4], jt[4];
    ir_eval(&r->p.ir, &qe, uni, ev, &r->scratch);
    r->p.fn(&qj, uni, jt);
    mem_copy(out, ev, sizeof(ev));
    *kill = qe.kill;
    return mem_equal(ev, jt, sizeof(ev)) && qe.kill == qj.kill;
}

static b32 compile_fails(const char* src, const char* needle) {
    MatokiProgram p;
    MatokiError e;
    if (matoki_compile(test_arena(), src, (u32)cstr_len(src), NULL, &p, &e)) return FALSE;
    usize n = cstr_len(needle), h = cstr_len(e.msg);
    for (usize i = 0; i + n <= h; i++) {
        if (mem_equal(e.msg + i, needle, n)) return TRUE;
    }
    return FALSE;
}

TEST(matoki_discard_kill_bits) {
    /* v.x > 0.5 → discard; aksi halde v.y > 0.5 ve v.z > 0.5 → discard (iç içe + else) */
    const char* src =
        "varying v: vec3;\n"
        "fn cut(x: f32) -> f32 { if (x > 0.75) { discard; } return x; }\n"
        "fn frag() -> vec4 {\n"
        "  var c = 0.0;\n"
        "  if (v.x > 0.5) { discard; c = 1.0; }\n"
        "  else { if (v.y > 0.5) { if (v.z > 0.5) { discard; } } else { c = cut(v.z); } }\n"
        "  return vec4(c, 0.0, 0.0, 1.0);\n"
        "}\n";
    Prog r;
    prog_init(t_, &r, src);
    if (!r.ok) return;
    CHECK(r.p.ir.kill != 0);
    Rng rng = rng_make(77);
    u32 bad = 0, killed = 0;
    for (u32 it = 0; it < 4000; it++) {
        ShadeQuad q;
        mem_zero(&q, sizeof(q));
        q.mask = 0xF;
        u32 want = 0;
        for (u32 l = 0; l < 4; l++) {
            f32 x = rng_f32(&rng), y = rng_f32(&rng), z = rng_f32(&rng);
            q.var[0][l] = x;
            q.var[1][l] = y;
            q.var[2][l] = z;
            b32 k = x > 0.5f || (y > 0.5f && z > 0.5f) || (y <= 0.5f && z > 0.75f);
            want |= (u32)k << l;
        }
        u32 out[4], kill;
        if (!prog_run(&r, &q, NULL, out, &kill) || kill != want) bad++;
        killed += (u32)__builtin_popcount(kill);
    }
    test_note("atılan lane", killed, 0);
    CHECK(bad == 0);

    /* discard yoksa kill çıktısı yok ve alan dokunulmaz */
    Prog r2;
    prog_init(t_, &r2, "fn frag() -> vec4 { return vec4(1.0); }\n");
    if (r2.ok) CHECK(r2.p.ir.kill == 0);
}

TEST(matoki_discard_through_raster) {
    /* sol yarı (x < 8) atılır: renk ve derinlik ilk değerinde kalmalı; iki raster modu */
    Prog r;
    prog_init(t_, &r,
              "fn frag() -> vec4 {\n"
              "  if (frag_coord().x < 8.0) { discard; }\n"
              "  return vec4(1.0, 0.0, 0.0, 1.0);\n"
              "}\n");
    if (!r.ok) return;
    enum { W = 16, H = 16 };
    for (u32 mode = 0; mode < RASTER_MODE_COUNT; mode++) {
        u32 color[W * H];
        f32 depth[W * H];
        Framebuffer fb = { color, depth, W, H };
        fb_clear(&fb, 0xFF00FF00u, 1.0f);
        Raster rs;
        raster_init(&rs, &fb);
        raster_set_mode(&rs, (RasterMode)mode);
        raster_set_cull(&rs, CULL_NONE);
        raster_set_depth(&rs, TRUE, TRUE, DEPTH_LESS);
        raster_set_shader(&rs, r.p.fn, NULL, 0);
        Arena bins;
        arena_init(&bins, arena_alloc(test_arena(), 1 << 20, 16), 1 << 20);
        raster_begin(&rs, &bins);
        RasterVert a, b, c, d;
        mem_zero(&a, sizeof(a));
        b = c = d = a;
        a.x = 0; a.y = 0;
        b.x = W; b.y = 0;
        c.x = W; c.y = H;
        d.x = 0; d.y = H;
        a.z = b.z = c.z = d.z = 0.5f;
        a.inv_w = b.inv_w = c.inv_w = d.inv_w = 1.0f;
        raster_triangle(&rs, &a, &b, &c);
        raster_triangle(&rs, &a, &c, &d);
        raster_flush(&rs);
        u32 bad = 0;
        for (i32 y = 0; y < H; y++) {
            for (i32 x = 0; x < W; x++) {
                b32 kept = x < 8;
                u32 cw = kept ? 0xFF00FF00u : 0xFF0000FFu;
                f32 dw = kept ? 1.0f : 0.5f;
                if (color[y * W + x] != cw || depth[y * W + x] != dw) bad++;
            }
        }
        CHECK(bad == 0);
        CHECK(rs.stats.pixels_written == (W / 2) * H);
    }
}

/* bileşen bit eşitliği: aynı bitler ya da ikisi de NaN */
#define SAME_BITS_FN \
    "fn same(a: f32, b: f32) -> bool { return bitcast_i32(a) == bitcast_i32(b) || (a != a && b != b); }\n"

TEST(matoki_mat_uniforms_match_c) {
    /* v (0..3), C sonucu M*v (4..7), N*v.xyz (8..10); her bileşen bit-bit aynı olmalı */
    const char* src =
        "uniform M: mat4;\nuniform N: mat3;\n"
        "varying v: vec4;\nvarying ra: vec4;\nvarying rb: vec3;\n"
        SAME_BITS_FN
        "fn frag() -> vec4 {\n"
        "  let a = M * v;\n  let b = N * v.xyz;\n"
        "  let ok = same(a.x, ra.x) && same(a.y, ra.y) && same(a.z, ra.z) && same(a.w, ra.w)\n"
        "        && same(b.x, rb.x) && same(b.y, rb.y) && same(b.z, rb.z);\n"
        "  return vec4(select(ok, 1.0, 0.0), 0.0, 0.0, 1.0);\n"
        "}\n";
    Prog r;
    prog_init(t_, &r, src);
    if (!r.ok) return;
    CHECK(matoki_find_uniform(&r.p, "M")->offset == 0);
    CHECK(matoki_find_uniform(&r.p, "N")->offset == 64);
    struct { Mat4 m; Mat3 n; } uni;
    Rng rng = rng_make(5150);
    u32 bad = 0;
    for (u32 it = 0; it < 3000; it++) {
        for (u32 k = 0; k < 16; k++) uni.m.m[k] = rng_range(&rng, -100, 100);
        for (u32 k = 0; k < 9; k++) uni.n.m[k] = rng_range(&rng, -100, 100);
        ShadeQuad q;
        mem_zero(&q, sizeof(q));
        for (u32 l = 0; l < 4; l++) {
            Vec4 v = v4(rng_range(&rng, -1e3f, 1e3f), rng_range(&rng, -1, 1), rng_range(&rng, -1e-3f, 1e-3f), rng_range(&rng, -5, 5));
            Vec4 a = mat4_mul_v4(&uni.m, v);
            Vec3 b = mat3_mul_v3(&uni.n, v3(v.x, v.y, v.z));
            const f32 vals[11] = { v.x, v.y, v.z, v.w, a.x, a.y, a.z, a.w, b.x, b.y, b.z };
            for (u32 k = 0; k < 11; k++) q.var[k][l] = vals[k];
        }
        u32 out[4], kill;
        if (!prog_run(&r, &q, &uni, out, &kill)) bad++;
        for (u32 l = 0; l < 4; l++) bad += (out[l] & 0xFF) != 0xFF;
    }
    CHECK(bad == 0);
    CHECK(compile_fails("uniform M: mat4;\nfn frag() -> vec4 { let m = M; return vec4(1.0); }\n", "yerel değişken"));
    CHECK(compile_fails("uniform M: mat4;\nvarying v: vec3;\nfn frag() -> vec4 { return vec4(M * v, 1.0); }\n", "M * v"));
    CHECK(compile_fails("uniform M: mat3;\nvarying v: vec3;\nfn frag() -> vec4 { return vec4(v * M, 1.0); }\n", "M * v"));
    CHECK(compile_fails("varying M: mat4;\nfn frag() -> vec4 { return vec4(1.0); }\n", "matris yalnızca uniform"));
}

static f32 special_x(Rng* rng) {
    switch (rng_u32(rng) % 24) {
    case 0: return m_bits_f32(0x7FC00000u);
    case 1: return m_bits_f32(0x7F800000u);
    case 2: return -m_bits_f32(0x7F800000u);
    case 3: return 0.0f;
    case 4: return -0.0f;
    case 5: return m_bits_f32(rng_u32(rng) & 0x007FFFFFu);   /* denormal */
    case 6: return m_bits_f32(rng_u32(rng));
    case 7: return rng_range(rng, -200.0f, 200.0f);
    case 8: return (f32)(i32)(rng_u32(rng) % 300) - 150.0f;
    default: return rng_range(rng, 0.0f, 8.0f);
    }
}

TEST(matoki_exp_log_pow_match_c) {
    /* x, y (0, 1); C: exp2(x), log2(x), exp(x), log(x), pow(x, y) (2..6) */
    const char* src =
        "varying x: f32;\nvarying y: f32;\nvarying r: vec4;\nvarying rp: f32;\n"
        SAME_BITS_FN
        "fn frag() -> vec4 {\n"
        "  let ok = same(exp2(x), r.x) && same(log2(x), r.y) && same(exp(x), r.z) && same(log(x), r.w)\n"
        "        && same(pow(x, y), rp);\n"
        "  let v = pow(vec3(x), vec3(y, 2.0, 0.5));\n"
        "  return vec4(select(ok, 1.0, 0.0), select(same(v.x, rp), 1.0, 0.0), 0.0, 1.0);\n"
        "}\n";
    Prog r;
    prog_init(t_, &r, src);
    if (!r.ok) return;
    Rng rng = rng_make(0xE4F);
    u32 bad = 0;
    for (u32 it = 0; it < 20000; it++) {
        ShadeQuad q;
        mem_zero(&q, sizeof(q));
        for (u32 l = 0; l < 4; l++) {
            f32 x = special_x(&rng), y = special_x(&rng);
            const f32 vals[7] = { x, y, m_exp2f(x), m_log2f(x), m_expf(x), m_logf(x), m_powf(x, y) };
            for (u32 k = 0; k < 7; k++) q.var[k][l] = vals[k];
        }
        u32 out[4], kill;
        if (!prog_run(&r, &q, NULL, out, &kill)) bad++;
        for (u32 l = 0; l < 4; l++) bad += (out[l] & 0xFFFF) != 0xFFFF;
    }
    CHECK(bad == 0);
}

static f32 rel_err(f32 got, f32 want) { return m_absf(got - want) / m_maxf(m_absf(want), 1e-30f); }

TEST(math_exp2_log2_accuracy) {
    /* tam değerler */
    for (i32 k = -126; k <= 127; k++) {
        f32 p = m_bits_f32((u32)(k + 127) << 23);
        CHECK(m_exp2f((f32)k) == p);
        CHECK(m_log2f(p) == (f32)k);
    }
    CHECK(m_exp2f(-149.0f) == m_bits_f32(1u));       /* en küçük denormal */
    CHECK(m_exp2f(-150.0f) == 0.0f);
    CHECK(m_exp2f(128.0f) == m_bits_f32(0x7F800000u));
    CHECK(m_exp2f(-1e30f) == 0.0f);
    CHECK(m_log2f(0.0f) == -m_bits_f32(0x7F800000u));
    CHECK(m_log2f(-0.0f) == -m_bits_f32(0x7F800000u));
    CHECK(m_log2f(-1.0f) != m_log2f(-1.0f));
    CHECK(m_log2f(m_bits_f32(0x7F800000u)) == m_bits_f32(0x7F800000u));
    /* gidiş-dönüş ve bilinen değerler */
    f32 worst_rt = 0.0f, worst_log = 0.0f;
    Rng rng = rng_make(99);
    for (u32 it = 0; it < 20000; it++) {
        f32 x = rng_range(&rng, -60.0f, 60.0f);
        f32 e = m_exp2f(x);
        worst_rt = m_maxf(worst_rt, m_absf(m_log2f(e) - x));
        f32 y = m_bits_f32(0x3F800000u | (rng_u32(&rng) & 0x7FFFFF)) * m_exp2f((f32)(i32)(rng_u32(&rng) % 40) - 20.0f);
        worst_log = m_maxf(worst_log, rel_err(m_exp2f(m_log2f(y)), y));
    }
    test_note("|log2(exp2(x)) - x| en büyük", (f64)worst_rt, 9);
    test_note("exp2(log2(y)) göreli hata en büyük", (f64)worst_log, 9);
    CHECK(worst_rt < 2e-5f);    /* |x| ≤ 60 → mutlak hata ~ ulp(60) ölçeğinde */
    CHECK(worst_log < 2e-5f);
    CHECK(rel_err(m_expf(1.0f), 2.71828183f) < 5e-7f);
    CHECK(rel_err(m_logf(10.0f), 2.30258509f) < 5e-7f);
    CHECK(rel_err(m_powf(10.0f, 2.0f), 100.0f) < 2e-6f);
    CHECK(rel_err(m_powf(2.0f, 0.5f), 1.41421356f) < 5e-7f);
    CHECK(rel_err(m_exp2f(0.3f), 1.23114441f) < 3e-7f);
    CHECK(rel_err(m_log2f(3.0f), 1.58496250f) < 3e-7f);
}
TEST(matoki_bilinear_matches_ref) {
    /* @bilinear: sample ↔ ref_sample_bilinear(seviye = ref_mip_level), sample_level ↔ verilen seviye;
     * wrap ve clamp, kare olmayan dokular, uv çok dışarıda ve sınırda */
    static const char* const srcs[2] = {
        "uniform t: texture2d @mipmap @bilinear;\nuniform lod: i32;\nvarying uv: vec2;\nvarying r: vec4;\nvarying rl: vec4;\n"
        SAME_BITS_FN
        "fn frag() -> vec4 {\n  let a = sample(t, uv);\n  let b = sample_level(t, uv, lod);\n"
        "  let ok = same(a.x, r.x) && same(a.y, r.y) && same(a.z, r.z) && same(a.w, r.w)\n"
        "        && same(b.x, rl.x) && same(b.y, rl.y) && same(b.z, rl.z) && same(b.w, rl.w);\n"
        "  return vec4(select(ok, 1.0, 0.0), 0.0, 0.0, 1.0);\n}\n",
        "uniform t: texture2d @bilinear @clamp;\nuniform lod: i32;\nvarying uv: vec2;\nvarying r: vec4;\nvarying rl: vec4;\n"
        SAME_BITS_FN
        "fn frag() -> vec4 {\n  let a = sample(t, uv);\n  let b = sample_level(t, uv, lod);\n"
        "  let ok = same(a.x, r.x) && same(a.y, r.y) && same(a.z, r.z) && same(a.w, r.w)\n"
        "        && same(b.x, rl.x) && same(b.y, rl.y) && same(b.z, rl.z) && same(b.w, rl.w);\n"
        "  return vec4(select(ok, 1.0, 0.0), 0.0, 0.0, 1.0);\n}\n",
    };
    Arena* a = test_arena();
    Texture* texs[2];
    texs[0] = tex_create(a, 64, 16, TEX_FLAG_MIPMAP);
    texs[1] = tex_create(a, 8, 32, TEX_FLAG_MIPMAP | TEX_FLAG_CLAMP);
    for (u32 k = 0; k < 2; k++) {
        usize n = img_mip_total(texs[k]->w, texs[k]->h, texs[k]->levels);
        for (usize i = 0; i < n; i++) texs[k]->px[i] = (u32)(i * 2654435761u) ^ (k * 0x9E3779B9u);
    }
    struct { const Texture* t; i32 lod; } uni;
    Rng rng = rng_make(0xB111);
    u32 bad = 0;
    for (u32 sv = 0; sv < 2; sv++) {
        Prog r;
        prog_init(t_, &r, srcs[sv]);
        if (!r.ok) return;
        uni.t = texs[sv];
        for (u32 it = 0; it < 4000; it++) {
            uni.lod = (i32)(rng_u32(&rng) % 9);
            ShadeQuad q;
            mem_zero(&q, sizeof(q));
            f32 s = m_bits_f32((u32)(127 - 10 + (i32)(rng_u32(&rng) % 12)) << 23);
            f32 u0 = (it % 7 == 0) ? rng_range(&rng, -1e4f, 1e4f) : rng_range(&rng, -2, 2);
            f32 v0 = rng_range(&rng, -2, 2);
            f32 ax = rng_range(&rng, -1, 1) * s, bx = rng_range(&rng, -1, 1) * s;
            for (u32 l = 0; l < 4; l++) {
                q.var[0][l] = u0 + ax * (f32)(l & 1);
                q.var[1][l] = v0 + bx * (f32)(l >> 1);
            }
            u32 lvl = sv == 0 ? ref_mip_level(uni.t, q.var[0], q.var[1]) : 0;
            for (u32 l = 0; l < 4; l++) {
                f32 ra[4], rb[4];
                ref_sample_bilinear(uni.t, q.var[0][l], q.var[1][l], lvl, ra);
                ref_sample_bilinear(uni.t, q.var[0][l], q.var[1][l], (u32)uni.lod, rb);
                for (u32 c = 0; c < 4; c++) {
                    q.var[2 + c][l] = ra[c];
                    q.var[6 + c][l] = rb[c];
                }
            }
            u32 out[4], kill;
            if (!prog_run(&r, &q, &uni, out, &kill)) bad++;
            for (u32 l = 0; l < 4; l++) bad += (out[l] & 0xFF) != 0xFF;
        }
    }
    CHECK(bad == 0);
    /* sabit renkli dokuda filtre aynı rengi verir (ağırlıklar toplamı 1) */
    Texture* flat = tex_create(a, 4, 4, 0);
    for (u32 i = 0; i < 16; i++) flat->px[i] = color_pack(200, 100, 50, 255);
    f32 o[4];
    ref_sample_bilinear(flat, 0.37f, 0.81f, 0, o);
    CHECK_NEAR(o[0], 200.0f / 255.0f, 1e-6f);
    CHECK_NEAR(o[2], 50.0f / 255.0f, 1e-6f);
    /* iki texel ortası: (0 + 255) / 2 */
    Texture* two = tex_create(a, 2, 1, TEX_FLAG_CLAMP);
    two->px[0] = color_pack(0, 0, 0, 255);
    two->px[1] = color_pack(255, 0, 0, 255);
    ref_sample_bilinear(two, 0.5f, 0.5f, 0, o);
    CHECK_NEAR(o[0], 0.5f, 1e-6f);
}