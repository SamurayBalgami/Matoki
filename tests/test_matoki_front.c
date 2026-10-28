/* tests/test_matoki_front.c — Matoki ön uç ve uçtan uca testleri.
 * Lexer, sayı ayrıştırma, konumlu hata mesajları, uniform/varying yansıtması,
 * lit.mtk ↔ shade_lit_tex (C) bit-bit eşitliği (ir_eval ve JIT), sin/cos ↔ m_sinf/m_cosf,
 * kontrol akışı (if/else if/else, for, swizzle ataması, satır içi çağrı). */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "img/procgen.h"
#include "matoki/ir_eval.h"
#include "matoki/matoki.h"
#include "raster/ref_shaders.h"

static b32 compile(const char* src, MatokiProgram* p, MatokiError* e) {
    return matoki_compile(test_arena(), src, (u32)cstr_len(src), NULL, p, e);
}

static b32 contains(const char* hay, const char* needle) {
    usize n = cstr_len(needle), h = cstr_len(hay);
    for (usize i = 0; i + n <= h; i++) {
        if (mem_equal(hay + i, needle, n)) return TRUE;
    }
    return FALSE;
}

TEST(matoki_lexer_tokens) {
    const char* src = "fn frag() -> vec4 { let x = 1.5e2 + 0x10; for i in 0..4 {} } // yorum\n/* blok */";
    TokenList t;
    CHECK(lex_source(test_arena(), src, (u32)cstr_len(src), &t));
    static const TokKind want[] = {
        T_FN, T_IDENT, T_LPAREN, T_RPAREN, T_ARROW, T_IDENT, T_LBRACE, T_LET, T_IDENT, T_ASSIGN, T_FLOAT, T_PLUS,
        T_INT, T_SEMI, T_FOR, T_IDENT, T_IN, T_INT, T_DOTDOT, T_INT, T_LBRACE, T_RBRACE, T_RBRACE, T_EOF,
    };
    CHECK(t.count == ARRAY_COUNT(want));
    for (u32 i = 0; i < ARRAY_COUNT(want) && i < t.count; i++) CHECK(t.toks[i].kind == want[i]);
    CHECK(t.toks[10].fval == 150.0f);
    CHECK(t.toks[12].ival == 16);
    CHECK(t.toks[17].ival == 0 && t.toks[19].ival == 4);
}

TEST(matoki_parse_f32_literals) {
    CHECK(lex_parse_f32("0.000001", 8) == 1e-6f);
    CHECK(lex_parse_f32("3.14159265", 10) == 3.14159265f);
    CHECK(lex_parse_f32("0.1", 3) == 0.1f);
    CHECK(lex_parse_f32("255.0", 5) == 255.0f);
    CHECK(lex_parse_f32("1e-3", 4) == 1e-3f);
    CHECK(lex_parse_f32("1e38", 4) == 1e38f);
    CHECK(lex_parse_f32("1.17549435e-38", 14) == 1.17549435e-38f);
    CHECK(lex_parse_f32("0.75", 4) == 0.75f);
}

typedef struct ErrCase {
    const char* src;
    i32 line, col;
    const char* msg;
} ErrCase;

TEST(matoki_errors_have_positions) {
    static const ErrCase cases[] = {
        { "fn frag() -> vec4 {\n  return vec4(1.0);\n}\n$", 4, 1, "beklenmeyen karakter" },
        { "fn frag() -> vec4 {\n  let a = 1.0\n  return vec4(a);\n}", 3, 3, "';' bekleniyor" },
        { "fn frag() -> vec4 {\n  return vec4(foo);\n}", 2, 15, "tanımsız isim 'foo'" },
        { "fn frag() -> vec4 {\n  let a = vec3(1.0) + vec2(1.0);\n  return vec4(a, 1.0);\n}", 2, 21, "vektör boyutları" },
        { "fn f(x: f32) -> f32 { return g(x); }\nfn g(x: f32) -> f32 { return f(x); }\nfn frag() -> vec4 { return vec4(f(1.0)); }", 2, 30, "özyineleme" },
        { "fn frag() -> vec4 {\n  return vec4(1.0);\n  let a = 1.0;\n}", 2, 3, "return yalnızca fonksiyonun son deyimi" },
        { "fn frag() -> vec4 {\n  let a = 1.0;\n}", 1, 4, "'return <ifade>;' ile bitmeli" },
        { "fn frag() -> vec4 {\n  let a = 1.0;\n  a = 2.0;\n  return vec4(a);\n}", 3, 3, "değiştirilemez" },
        { "fn main() -> vec4 { return vec4(1.0); }", 1, 1, "giriş noktası yok" },
        { "uniform t: texture2d;\nfn frag() -> vec4 { return sample(t, 1.0); }", 2, 28, "uv vec2" },
        { "fn frag() -> vec4 {\n  if (1.0) { }\n  return vec4(1.0);\n}", 2, 7, "if koşulu bool" },
        { "fn frag() -> vec4 {\n  let i = 3 / 2;\n  return vec4(1.0);\n}", 2, 13, "tamsayı bölme" },
        { "varying v: vec4;\nfn frag() -> vec4 { return v.xyzq; }", 2, 30, "geçersiz swizzle" },
    };
    for (u32 i = 0; i < ARRAY_COUNT(cases); i++) {
        MatokiProgram p;
        MatokiError e;
        b32 ok = compile(cases[i].src, &p, &e);
        CHECK(!ok);
        CHECK(e.line == cases[i].line);
        CHECK(e.col == cases[i].col);
        CHECK(contains(e.msg, cases[i].msg));
        if (ok || e.line != cases[i].line || e.col != cases[i].col || !contains(e.msg, cases[i].msg)) {
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

static b32 compile_lit(MatokiProgram* p, MatokiError* e) {
    const EmbedFile* ef = embed_find("lit.mtk");
    if (!ef) return FALSE;
    return matoki_compile(test_arena(), (const char*)ef->data, ef->len, NULL, p, e);
}

TEST(matoki_lit_reflection_matches_c_struct) {
    MatokiProgram p;
    MatokiError e;
    CHECK(compile_lit(&p, &e));
    if (e.failed) {
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        matoki_format_error(&e, &sb);
        log_sb(&sb);
        return;
    }
    CHECK(matoki_find_uniform(&p, "tex")->offset == __builtin_offsetof(LitUniforms, tex));
    CHECK(matoki_find_uniform(&p, "to_light")->offset == __builtin_offsetof(LitUniforms, to_light));
    CHECK(matoki_find_uniform(&p, "ambient")->offset == __builtin_offsetof(LitUniforms, ambient));
    CHECK(matoki_find_uniform(&p, "light_color")->offset == __builtin_offsetof(LitUniforms, light_color));
    CHECK(matoki_find_uniform(&p, "fog_start")->offset == __builtin_offsetof(LitUniforms, fog_start));
    CHECK(matoki_find_uniform(&p, "fog_color")->offset == __builtin_offsetof(LitUniforms, fog_color));
    CHECK(matoki_find_uniform(&p, "fog_end")->offset == __builtin_offsetof(LitUniforms, fog_end));
    CHECK(matoki_find_uniform(&p, "eye")->offset == __builtin_offsetof(LitUniforms, eye));
    CHECK(matoki_find_uniform(&p, "tint")->offset == __builtin_offsetof(LitUniforms, tint));
    CHECK(matoki_find_varying(&p, "wpos")->offset == VAR3D_WPOS);
    CHECK(matoki_find_varying(&p, "nrm")->offset == VAR3D_NRM);
    CHECK(matoki_find_varying(&p, "uv")->offset == VAR3D_UV);
    test_note("lit.mtk IR komutu (toplam)", p.ir_total, 0);
    test_note("lit.mtk IR komutu (canlı)", p.ir_live, 0);
    test_note("katlanan / CSE isabeti", p.ir.folded, 0);
    test_note("CSE isabeti", p.ir.cse_hits, 0);
    CHECK(matoki_codegen(test_arena(), &p, &e));
    test_note("lit.mtk wasm bayt", p.wasm.len, 0);
}

static f32 special_f(Rng* rng, f32 lo, f32 hi) {
    u32 r = rng_u32(rng) % 64;
    if (r == 0) return m_bits_f32(0x7FC00000u);
    if (r == 1) return m_bits_f32(0x7F800000u);
    if (r == 2) return -m_bits_f32(0x7F800000u);
    if (r == 3) return 0.0f;
    return rng_range(rng, lo, hi);
}

static void lit_bit_exact(TestCtx* t_, b32 scalar);

TEST(matoki_lit_bit_exact_vs_c) { lit_bit_exact(t_, TRUE); }

/* quad-sabit değerler v128'de (codegen ölçüm anahtarı): türev / tek okuma / kaydırma vektör yolları */
TEST(matoki_lit_bit_exact_vs_c_vector_uniforms) { lit_bit_exact(t_, FALSE); }

static void lit_bit_exact(TestCtx* t_, b32 scalar) {
    MatokiProgram p;
    MatokiError e;
    Arena* a = test_arena();
    codegen_set_scalar(scalar);
    b32 ok = compile_lit(&p, &e) && matoki_jit(a, &p, &e);
    codegen_set_scalar(TRUE);
    CHECK(ok);
    if (e.failed) return;
    Texture* texs[2];
    texs[0] = tex_create(a, 128, 128, TEX_FLAG_MIPMAP);
    procgen_bricks(texs[0], 5);
    texs[1] = tex_create(a, 64, 16, 0);
    for (u32 i = 0; i < 64 * 16; i++) texs[1]->px[i] = hash_u32(i * 31 + 7);
    Arena scratch;
    arena_init(&scratch, arena_alloc(a, 1 << 20, 16), 1 << 20);

    Rng rng = rng_make(0x11F7);
    u32 diff_eval = 0, diff_jit = 0, diff_fast = 0, lanes = 0;
    for (u32 it = 0; it < 30000; it++) {
        LitUniforms u;
        mem_zero(&u, sizeof(u));
        u.tex = texs[it & 1];
        u.to_light = v3_norm(v3(rng_range(&rng, -1, 1), rng_range(&rng, -1, 1), rng_range(&rng, -1, 1)));
        u.ambient = rng_range(&rng, 0, 1);
        u.light_color = v3(rng_range(&rng, 0, 2), rng_range(&rng, 0, 2), rng_range(&rng, 0, 2));
        u.fog_start = rng_range(&rng, 0, 50);
        u.fog_end = (it % 17 == 0) ? u.fog_start : u.fog_start + rng_range(&rng, 1, 500);
        u.fog_color = v3(rng_f32(&rng), rng_f32(&rng), rng_f32(&rng));
        u.eye = v3(rng_range(&rng, -50, 50), rng_range(&rng, -50, 50), rng_range(&rng, -50, 50));
        u.use_mip = it % 3 != 0;
        u.tint = v3(rng_range(&rng, 0, 1.5f), rng_range(&rng, 0, 1.5f), rng_range(&rng, 0, 1.5f));
        lit_uniforms_prepare(&u);

        ShadeQuad q;
        mem_zero(&q, sizeof(q));
        q.x = (i32)(rng_u32(&rng) % 640);
        q.y = (i32)(rng_u32(&rng) % 360);
        q.mask = 0xF;
        for (u32 l = 0; l < 4; l++) {
            q.z[l] = rng_f32(&rng);
            for (u32 k = 0; k < 3; k++) q.var[VAR3D_WPOS + k][l] = special_f(&rng, -200, 200);
            for (u32 k = 0; k < 3; k++) q.var[VAR3D_NRM + k][l] = (it % 23 == 0) ? 0.0f : special_f(&rng, -2, 2);
            q.var[VAR3D_UV][l] = special_f(&rng, -20, 20);
            q.var[VAR3D_UV + 1][l] = special_f(&rng, -20, 20);
        }
        if (it & 1) {
            /* tutarlı uv: quad içinde doğrusal, ölçek 2^-10..2^2 → tüm mip seviyeleri */
            f32 s = m_bits_f32((u32)(127 - 10 + (i32)(rng_u32(&rng) % 13)) << 23) * rng_range(&rng, 1.0f, 2.0f);
            f32 u0 = rng_range(&rng, -4, 4), v0 = rng_range(&rng, -4, 4);
            f32 ax = rng_range(&rng, -1, 1) * s, ay = rng_range(&rng, -1, 1) * s;
            f32 bx = rng_range(&rng, -1, 1) * s, by = rng_range(&rng, -1, 1) * s;
            for (u32 l = 0; l < 4; l++) {
                f32 lx = (f32)(l & 1), ly = (f32)(l >> 1);
                q.var[VAR3D_UV][l] = u0 + ax * lx + bx * ly;
                q.var[VAR3D_UV + 1][l] = v0 + ay * lx + by * ly;
            }
        }
        u32 ref[4], ev[4], jt[4], ft[4];
        shade_lit_tex(&q, &u, ref);
        ir_eval(&p.ir, &q, &u, ev, &scratch);
        p.fn(&q, &u, jt);
        p.prelude(&u);           /* uniform'lar her turda değişiyor (aynı adres): önsöz yeniden */
        p.fn_fast(&q, &u, ft);
        for (u32 l = 0; l < 4; l++) {
            diff_eval += ref[l] != ev[l];
            diff_jit += ref[l] != jt[l];
            diff_fast += ref[l] != ft[l];
            lanes++;
        }
    }
    test_note("lane", lanes, 0);
    test_note("önsöze alınan değer", p.wasm.hoisted, 0);
    CHECK(p.wasm.hoisted > 0);
    CHECK(diff_eval == 0);
    CHECK(diff_jit == 0);
    CHECK(diff_fast == 0);
}

/* Tek bir test shader'ını hem ir_eval hem JIT ile çalıştırır. */
typedef struct Runner {
    MatokiProgram p;
    Arena scratch;
    b32 ok;
} Runner;

static void runner_init(TestCtx* t_, Runner* r, const char* src) {
    MatokiError e;
    r->ok = compile(src, &r->p, &e) && matoki_jit(test_arena(), &r->p, &e);
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

static void runner_run(Runner* r, const ShadeQuad* q, const void* uni, u32 ev[4], u32 jt[4]) {
    ir_eval(&r->p.ir, q, uni, ev, &r->scratch);
    r->p.fn(q, uni, jt);
}

TEST(matoki_sin_cos_bit_exact_vs_c) {
    /* r = sin(a) == s, g = cos(a) == c → 255; aksi 0 */
    const char* src =
        "varying a: f32;\nvarying s: f32;\nvarying c: f32;\n"
        "fn frag() -> vec4 {\n"
        "  return vec4(select(sin(a) == s, 1.0, 0.0), select(cos(a) == c, 1.0, 0.0), 0.0, 1.0);\n"
        "}\n";
    Runner r;
    runner_init(t_, &r, src);
    if (!r.ok) return;
    ShadeQuad q;
    mem_zero(&q, sizeof(q));
    u32 bad = 0;
    for (i32 i = -8000; i < 8000; i += 4) {
        for (u32 l = 0; l < 4; l++) {
            f32 x = (f32)(i + (i32)l) * 0.003f;
            q.var[0][l] = x;
            q.var[1][l] = m_sinf(x);
            q.var[2][l] = m_cosf(x);
        }
        u32 ev[4], jt[4];
        runner_run(&r, &q, NULL, ev, jt);
        for (u32 l = 0; l < 4; l++) {
            if ((ev[l] & 0xFFFF) != 0xFFFF || ev[l] != jt[l]) bad++;
        }
    }
    CHECK(bad == 0);
}

static u32 pack_c(f32 r, f32 g, f32 b, f32 a) {
    const f32 c[4] = { r, g, b, a };
    u32 o = 0;
    for (u32 i = 0; i < 4; i++) {
        f32 x = m_clampf(c[i], 0.0f, 1.0f);
        o |= (u32)m_f2i_sat(x * 255.0f + 0.5f) << (8 * i);
    }
    return o;
}

TEST(matoki_control_flow_and_inlining) {
    const char* src =
        "varying v: vec4;\n"
        "fn f(x: f32) -> f32 { return x * 2.0 + 1.0; }\n"
        "fn twice(x: f32) -> f32 { return f(f(x)); }\n"
        "fn frag() -> vec4 {\n"
        "  var acc = 0.0;\n"
        "  for i in 0..5 { acc += f(f32(i)) * v.x; }\n"
        "  var c = vec3(0.0);\n"
        "  if (v.y > 0.5) { c.x = 1.0; c.yz = vec2(0.25, v.z); }\n"
        "  else if (v.y > 0.25) { c = vec3(0.5); }\n"
        "  else { c.z = acc; }\n"
        "  let k = select(v.w < 0.0, twice(v.w), v.w * 0.5);\n"
        "  return vec4(c, clamp(acc + k, 0.0, 1.0));\n"
        "}\n";
    Runner r;
    runner_init(t_, &r, src);
    if (!r.ok) return;
    Rng rng = rng_make(4242);
    ShadeQuad q;
    mem_zero(&q, sizeof(q));
    u32 bad = 0;
    for (u32 it = 0; it < 4000; it++) {
        for (u32 l = 0; l < 4; l++) {
            for (u32 k = 0; k < 4; k++) q.var[k][l] = rng_range(&rng, -1.0f, 1.0f);
        }
        u32 ev[4], jt[4];
        runner_run(&r, &q, NULL, ev, jt);
        for (u32 l = 0; l < 4; l++) {
            f32 vx = q.var[0][l], vy = q.var[1][l], vz = q.var[2][l], vw = q.var[3][l];
            f32 acc = 0.0f;
            for (i32 i = 0; i < 5; i++) acc = acc + ((f32)i * 2.0f + 1.0f) * vx;
            f32 c0 = 0, c1 = 0, c2 = 0;
            if (vy > 0.5f) { c0 = 1.0f; c1 = 0.25f; c2 = vz; }
            else if (vy > 0.25f) { c0 = c1 = c2 = 0.5f; }
            else { c2 = acc; }
            f32 k = vw < 0.0f ? ((vw * 2.0f + 1.0f) * 2.0f + 1.0f) : vw * 0.5f;
            f32 al = m_minf(m_maxf(acc + k, 0.0f), 1.0f);
            u32 want = pack_c(c0, c1, c2, al);
            if (ev[l] != want || jt[l] != want) bad++;
        }
    }
    CHECK(bad == 0);
}

TEST(matoki_derivatives_and_frag_coord) {
    /* dFdx(frag_coord.x) = 1, dFdy(frag_coord.y) = 1, fwidth(uniform) = 0; v'nin türevi lane farkı */
    const char* src =
        "uniform k: f32;\n"
        "varying v: f32;\n"
        "fn frag() -> vec4 {\n"
        "  let p = frag_coord();\n"
        "  let ok = dFdx(p.x) == 1.0 && dFdy(p.y) == 1.0 && dFdx(p.y) == 0.0 && fwidth(k) == 0.0;\n"
        "  return vec4(select(ok, 1.0, 0.0), dFdx(v), dFdy(v), 1.0);\n"
        "}\n";
    Runner r;
    runner_init(t_, &r, src);
    if (!r.ok) return;
    f32 uni_k = 3.5f;
    ShadeQuad q;
    mem_zero(&q, sizeof(q));
    q.x = 100;
    q.y = 50;
    q.var[0][0] = 0.1f;
    q.var[0][1] = 0.6f;   /* dFdx = 0.5 → 128 */
    q.var[0][2] = 0.35f;  /* dFdy = 0.25 → 64 */
    q.var[0][3] = 9.0f;
    u32 ev[4], jt[4];
    runner_run(&r, &q, &uni_k, ev, jt);
    for (u32 l = 0; l < 4; l++) {
        CHECK(ev[l] == jt[l]);
        CHECK((ev[l] & 0xFF) == 255);
        CHECK(((ev[l] >> 8) & 0xFF) == 128);
        CHECK(((ev[l] >> 16) & 0xFF) == 64);
    }
}

/* @mipmap sample ↔ ref_sample_tex_mip, sample_level ↔ ref_sample_level, mip_level ↔ ref_mip_level;
 * kare olmayan, wrap / clamp dokular. Çıktı: r = seviye, g/b/a = texel'in düşük 24 biti eşleşmesi. */
TEST(matoki_mip_sampling_matches_ref) {
    static const char* const srcs[2] = {
        "uniform t: texture2d @mipmap;\nuniform lod: i32;\nvarying uv: vec2;\n"
        "fn frag() -> vec4 {\n"
        "  let a = sample(t, uv);\n  let b = sample_level(t, uv, lod);\n"
        "  return vec4(f32(mip_level(t, uv)) / 255.0, a.r, a.g, b.r);\n}\n",
        "uniform t: texture2d @mipmap @clamp;\nuniform lod: i32;\nvarying uv: vec2;\n"
        "fn frag() -> vec4 {\n"
        "  let a = sample(t, uv);\n  let b = sample_level(t, uv, lod);\n"
        "  return vec4(f32(mip_level(t, uv)) / 255.0, a.r, a.g, b.r);\n}\n",
    };
    Arena* a = test_arena();
    Texture* texs[3];
    texs[0] = tex_create(a, 256, 16, TEX_FLAG_MIPMAP);
    texs[1] = tex_create(a, 8, 64, TEX_FLAG_MIPMAP | TEX_FLAG_CLAMP);
    texs[2] = tex_create(a, 32, 32, 0);
    for (u32 k = 0; k < 3; k++) {
        usize n = img_mip_total(texs[k]->w, texs[k]->h, texs[k]->levels);
        for (usize i = 0; i < n; i++) texs[k]->px[i] = hash_u32((u32)i * 2654435761u + k);
    }
    struct { const Texture* t; i32 lod; } uni;
    Rng rng = rng_make(0x3A9);
    u32 bad = 0, levels_seen = 0;
    for (u32 sv = 0; sv < 2; sv++) {
        Runner r;
        runner_init(t_, &r, srcs[sv]);
        if (!r.ok) return;
        for (u32 it = 0; it < 6000; it++) {
            const Texture* t = texs[it % 3];
            if (((t->flags & TEX_FLAG_CLAMP) != 0) != (sv == 1)) continue;
            uni.t = t;
            uni.lod = (i32)(rng_u32(&rng) % 12);
            ShadeQuad q;
            mem_zero(&q, sizeof(q));
            f32 s = m_bits_f32((u32)(127 - 12 + (i32)(rng_u32(&rng) % 14)) << 23) * rng_range(&rng, 1.0f, 2.0f);
            f32 u0 = rng_range(&rng, -3, 3), v0 = rng_range(&rng, -3, 3);
            f32 ax = rng_range(&rng, -1, 1) * s, ay = rng_range(&rng, -1, 1) * s;
            f32 bx = rng_range(&rng, -1, 1) * s, by = rng_range(&rng, -1, 1) * s;
            for (u32 l = 0; l < 4; l++) {
                q.var[0][l] = u0 + ax * (f32)(l & 1) + bx * (f32)(l >> 1);
                q.var[1][l] = v0 + ay * (f32)(l & 1) + by * (f32)(l >> 1);
            }
            u32 ref_a[4];
            ref_sample_tex_mip(t, q.var[0], q.var[1], ref_a);
            u32 lvl = (t->flags & TEX_FLAG_MIPMAP) ? ref_mip_level(t, q.var[0], q.var[1]) : 0;
            levels_seen |= 1u << lvl;
            u32 ev[4], jt[4];
            runner_run(&r, &q, &uni, ev, jt);
            for (u32 l = 0; l < 4; l++) {
                u32 ref_b = ref_sample_level(t, q.var[0][l], q.var[1][l], (u32)uni.lod);
                u32 want = pack_c((f32)lvl / 255.0f, (f32)(ref_a[l] & 0xFF) / 255.0f, (f32)((ref_a[l] >> 8) & 0xFF) / 255.0f,
                                  (f32)(ref_b & 0xFF) / 255.0f);
                if (ev[l] != want || jt[l] != want) bad++;
            }
        }
    }
    test_note("görülen seviye maskesi", levels_seen, 0);
    CHECK(bad == 0);
    CHECK((levels_seen & 0x1FF) == 0x1FF);   /* 0..8 seviyelerinin hepsi denendi */
}
