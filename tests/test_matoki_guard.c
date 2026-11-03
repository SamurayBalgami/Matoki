/* tests/test_matoki_guard.c — quad-sabit dallanma (matoki/ir_guard, D43): dal seçilmeyen kolu atlar, sonuç değişmez. JIT tam
 * gövde ve önsöz + hızlı gövde, kâhin ir_eval ile bit bit aynı olmalı; koşulun iki yönü de, kollar arası paylaşılan ara
 * değer, iç içe dal, kolda türev ve doku, uzun if zinciri (derinlik sınırı), gerçek lit_hq.mtk. Plan da denetlenir: kısmi
 * maske ve lane'e bağlı koşul dallanmaz, küçük kol dal açmaz, vec bileşenleri tek dal paylaşır. */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/math.h"
#include "core/mem.h"
#include "core/rng.h"
#include "matoki/ir_eval.h"
#include "matoki/ir_guard.h"
#include "matoki/matoki.h"
#include "raster/ref_shaders.h"

static b32 guard_compile(TestCtx* t_, const char* src, u32 len, MatokiProgram* p) {
    MatokiError e;
    b32 ok = matoki_compile(test_arena(), src, len, NULL, p, &e) && matoki_jit(test_arena(), p, &e);
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

static u32 branch_count(const MatokiProgram* p) {
    IrGuards g;
    ir_guard_build(test_arena(), &p->ir, NULL, FALSE, &g);
    u32 n = 0;
    for (u32 k = 0; k < g.ngroups; k++) n += g.branch[k];
    return n;
}

static void quad_random(ShadeQuad* q, Rng* r, u32 nvar) {
    mem_zero(q, sizeof(*q));
    q->x = (i32)(rng_u32(r) % 640) & ~1;
    q->y = (i32)(rng_u32(r) % 360) & ~1;
    q->mask = 0xF;
    f32 sx = rng_range(r, 0.0f, 0.02f), sy = rng_range(r, 0.0f, 0.2f);
    for (u32 s = 0; s < nvar; s++) {
        f32 b = rng_range(r, -3.0f, 3.0f);
        for (u32 l = 0; l < 4; l++) q->var[s][l] = b + (f32)(l & 1) * sx + (f32)(l >> 1) * sy;
    }
    for (u32 l = 0; l < 4; l++) q->z[l] = 0.5f;
}

/* eval == tam gövde == önsöz + hızlı gövde; dönüş: farklı çıktı üreten quad sayısı (koşul iki yöne de gitti mi) */
static u32 guard_compare(TestCtx* t_, const MatokiProgram* p, const void* uni, u32 nvar, u32 quads, u64 seed) {
    Arena* a = test_arena();
    Rng r = rng_make(seed);
    usize mark = arena_mark(a);
    u32 first[4], varied = 0;
    b32 same = TRUE;
    if (p->prelude) p->prelude(uni);
    for (u32 k = 0; k < quads; k++) {
        ShadeQuad q;
        quad_random(&q, &r, nvar);
        _Alignas(16) u32 o0[4], o1[4], o2[4];
        ir_eval_io(&p->ir, &q, uni, o0, a);
        p->fn(&q, uni, o1);
        p->fn_fast(&q, uni, o2);
        same = same && mem_equal(o0, o1, 16) && mem_equal(o0, o2, 16);
        if (k == 0) mem_copy(first, o0, 16);
        else if (!mem_equal(first, o0, 16)) varied++;
    }
    arena_reset(a, mark);
    CHECK(same);
    return varied;
}

#define SRC(s) s, (u32)sizeof(s) - 1u

/* dFdx quad-sabit: koşul lane'e değil quad'a bağlı. Kollar pahalı, ara değer paylaşımlı */
static const char SHADOWED_ARMS[] =
    "uniform a: f32;\nuniform k: f32;\nvarying v: vec2;\n"
    "fn heavy(x: f32) -> f32 { return pow(abs(sin(x * 3.0)) + 0.5, 2.5) * cos(x); }\n"
    "fn frag() -> vec4 {\n"
    "  let d = dFdx(v.x) + dFdy(v.y);\n"
    "  let shared = heavy(v.x + a);\n"
    "  var c = vec3(0.1);\n"
    "  if (d > k) {\n"
    "    c = vec3(heavy(v.y), shared, heavy(v.x * 2.0));\n"
    "  } else {\n"
    "    c = vec3(shared * 0.5, heavy(v.y + 1.0), 0.25);\n"
    "  }\n"
    "  return vec4(clamp(c, vec3(0.0), vec3(1.0)), 1.0);\n"
    "}\n";

TEST(guard_quad_uniform_if_matches_oracle) {
    MatokiProgram p;
    if (!guard_compile(t_, SRC(SHADOWED_ARMS), &p)) return;
    CHECK(branch_count(&p) >= 1u);
    const f32 uni[2] = { 0.3f, 0.1f };   /* d ∈ [0, 0.22]: iki yöne de */
    u32 varied = guard_compare(t_, &p, uni, 2, 400, 11);
    CHECK(varied > 100u);
    /* iki kolu da zorla: k uçlarda */
    const f32 all_true[2] = { 0.3f, -1.0f }, all_false[2] = { 0.3f, 9.0f };
    guard_compare(t_, &p, all_true, 2, 100, 12);
    guard_compare(t_, &p, all_false, 2, 100, 13);
}

/* maske değişince önsöz yeniden çalışır: çizim-sabit koşul (uniform) ile quad-sabit koşul bir arada, iç içe */
static const char NESTED[] =
    "uniform m: i32;\nuniform k: f32;\nvarying v: vec2;\n"
    "fn heavy(x: f32) -> f32 { return exp2(sin(x) * 2.0) + log2(abs(cos(x)) + 1.0); }\n"
    "fn frag() -> vec4 {\n"
    "  let d = fwidth(v.x);\n"
    "  let r = select(m != 0,\n"
    "              select(d > k, heavy(v.x), heavy(v.y)),\n"
    "              select(d > k * 2.0, select(d > k * 0.5, heavy(v.x + 1.0), heavy(v.y + 1.0)), heavy(v.x + v.y)));\n"
    "  return vec4(fract(r), fract(r * 3.0), 0.5, 1.0);\n"
    "}\n";

TEST(guard_nested_and_draw_constant_masks) {
    MatokiProgram p;
    if (!guard_compile(t_, SRC(NESTED), &p)) return;
    CHECK(branch_count(&p) >= 2u);
    for (i32 m = 0; m < 2; m++) {
        struct { i32 m; f32 k; } uni = { m, 0.05f };
        CHECK(guard_compare(t_, &p, &uni, 2, 300, 20u + (u64)m) > 50u);
    }
}

/* kolda türev ve doku okuması: quad-sabit koşul; kol atlanırsa türev de atlanır, sonuç aynı */
static const char TEX_ARM[] =
    "uniform t: texture2d @mipmap @bilinear;\nuniform k: f32;\nvarying v: vec2;\n"
    "fn frag() -> vec4 {\n"
    "  let d = fwidth(v.y);\n"
    "  let a = select(d > k, sample_level(t, v * 0.5, mip_level(t, v * 0.5)), sample_level(t, v, 0) + vec4(dFdx(v.x)));\n"
    "  return vec4(a.rgb, 1.0);\n"
    "}\n";

TEST(guard_texture_and_derivative_in_arm) {
    Arena* a = test_arena();
    MatokiProgram p;
    if (!guard_compile(t_, SRC(TEX_ARM), &p)) return;
    CHECK(branch_count(&p) >= 1u);
    Texture* tex = tex_create(a, 16, 16, TEX_FLAG_MIPMAP | TEX_FLAG_BILINEAR);
    Rng r = rng_make(5);
    for (u32 i = 0; i < 16u * 16u; i++) tex->px[i] = rng_u32(&r);
    tex_build_mips(tex);
    struct { const Texture* t; f32 k; } uni = { tex, 0.1f };
    CHECK(guard_compare(t_, &p, &uni, 2, 400, 31) > 100u);
    /* WGSL: türevli kol dal olmaz, türevsiz olur */
    IrGuards g;
    ir_guard_build(a, &p.ir, NULL, TRUE, &g);
    u32 with_deriv = 0;
    for (u32 k = 0; k < g.ngroups; k++) with_deriv += g.branch[k] && (g.nodes[ir_guard_true(k)].deriv + g.nodes[ir_guard_false(k)].deriv);
    CHECK(with_deriv == 0u);
}

/* lane'e bağlı koşul dal olmaz (her lane kendi kolunu ister) */
TEST(guard_lane_varying_mask_stays_flat) {
    static const char S[] =
        "varying v: vec2;\n"
        "fn heavy(x: f32) -> f32 { return exp2(sin(x) * 2.0) + log2(abs(cos(x)) + 1.0) + sqrt(abs(x)); }\n"
        "fn frag() -> vec4 { let r = select(v.x > 0.0, heavy(v.y), heavy(v.y * 2.0)); return vec4(fract(r), 0.0, 0.0, 1.0); }\n";
    MatokiProgram p;
    if (!guard_compile(t_, SRC(S), &p)) return;
    CHECK(branch_count(&p) == 0u);
    const f32 uni[1] = { 0.0f };
    guard_compare(t_, &p, uni, 2, 200, 41);
}

/* elle kurulan IR: kısmi maske (partial) dallanmaz: select iki kolu da bit bit okur; tam maske dallanır; küçük kol dal açmaz */
static IrRef long_chain(IrFunc* f, IrRef x, u32 n) {
    for (u32 k = 0; k < n; k++) x = ir_op2(f, IR_FMUL, IR_T_F32, ir_op2(f, IR_FADD, IR_T_F32, x, x), x);
    return x;
}

TEST(guard_plan_rules) {
    Arena* a = test_arena();
    IrFunc* f = ARENA_NEW(a, IrFunc);
    ir_init(f, a, 512, TRUE);
    IrRef v = ir_emit(f, IR_VARYING, IR_T_F32, 0, 0, 0, 0);
    IrRef u0 = ir_emit(f, IR_UNIFORM, IR_T_F32, 0, 0, 0, 0);
    IrRef partial = ir_emit(f, IR_UNIFORM, IR_T_I32, 0, 0, 0, 4);
    IrRef full = ir_emit(f, IR_FLT, IR_T_I32, u0, u0, 0, 0);
    IrRef s0 = ir_emit(f, IR_SELECT, IR_T_F32, partial, long_chain(f, v, 8), long_chain(f, v, 9), 0);
    IrRef big_t = long_chain(f, s0, 8), big_e = long_chain(f, s0, 9);
    IrRef s1 = ir_emit(f, IR_SELECT, IR_T_F32, full, big_t, big_e, 0);
    IrRef s2 = ir_emit(f, IR_SELECT, IR_T_F32, full, ir_op2(f, IR_FADD, IR_T_F32, s1, s0), s1, 0);   /* küçük kol, aynı maske ama bitişik değil */
    f->out = ir_op1(f, IR_BITCAST_F2I, IR_T_I32, s2);
    ir_mark_live(f);
    IrGuards g;
    ir_guard_build(a, f, NULL, FALSE, &g);
    u32 n = 0;
    for (u32 k = 0; k < g.ngroups; k++) n += g.branch[k];
    CHECK(n == 1u);                       /* yalnız s1; s0 kısmi maske, s2 küçük */
    CHECK(g.node[s0] == 0u);              /* s0 iki kolda da kullanılıyor: koşulsuz */
    CHECK(g.node[big_t] != 0u && g.node[big_e] != 0u && g.node[big_t] != g.node[big_e]);
    CHECK(g.node[partial] == 0u && g.node[full] == 0u);
    /* skip: select dallanmaz, kolları üste çıkar */
    u8* skip = ARENA_ARRAY(a, u8, f->count);
    for (u32 i = 0; i < f->count; i++) skip[i] = 1;
    ir_guard_build(a, f, skip, FALSE, &g);
    CHECK(g.ngroups == 0u && g.node[big_t] == 0u);
}

/* vec bileşenleri tek dal: 3 ayrı dal olsaydı paylaşılan ara değerler koşulsuza kaçardı */
TEST(guard_vector_select_shares_one_branch) {
    static const char S[] =
        "uniform k: f32;\nvarying v: vec2;\n"
        "fn heavy(x: f32) -> vec3 { let s = exp2(sin(x)); return vec3(s, s * 2.0, s + 1.0); }\n"
        "fn frag() -> vec4 { let c = select(fwidth(v.x) > k, heavy(v.x), heavy(v.y)); return vec4(fract(c), 1.0); }\n";
    MatokiProgram p;
    if (!guard_compile(t_, SRC(S), &p)) return;
    CHECK(branch_count(&p) == 1u);
    const f32 uni[1] = { 0.02f };
    CHECK(guard_compare(t_, &p, uni, 2, 300, 51) > 50u);
}

/* uzun if zinciri (açılmış döngü): derinlik sınırı; JIT yüklenir ve aynı sonuç */
TEST(guard_long_if_chain_depth_limit) {
    static const char S[] =
        "uniform k: f32;\nvarying v: vec2;\n"
        "fn frag() -> vec4 {\n"
        "  var x = v.x;\n"
        "  let d = fwidth(v.y);\n"
        "  for i in 0..200 {\n"
        "    if (d > k + f32(i) * 0.001) { x = exp2(sin(x)); } else { x = x; }\n"
        "  }\n"
        "  var y = v.y;\n"
        "  for i in 0..60 {\n"
        "    y = select(d > f32(i) * 0.003, exp2(sin(y) + f32(i)), y + 0.5);\n"
        "  }\n"
        "  return vec4(fract(x), fract(y), 0.0, 1.0);\n"
        "}\n";
    MatokiProgram p;
    if (!guard_compile(t_, SRC(S), &p)) return;
    IrGuards g;
    ir_guard_build(test_arena(), &p.ir, NULL, FALSE, &g);
    u32 deep = 0;
    for (u32 k = 0; k < g.ngroups * 2u + 1u; k++) if (g.nodes[k].depth > deep) deep = g.nodes[k].depth;
    CHECK(deep <= IR_GUARD_MAX_DEPTH);
    const f32 uni[1] = { 0.0f };
    CHECK(guard_compare(t_, &p, uni, 2, 200, 61) > 20u);
}

/* gerçek shader: gölgeli lit_hq; gerçek mip zinciri, eğik ve dik ayak izleri (örnek sayısının tüm kolları) */
TEST(guard_lit_hq_matches_oracle) {
    Arena* a = test_arena();
    const EmbedFile* ef = embed_find("lit_hq.mtk");
    CHECK(ef != NULL);
    MatokiProgram p;
    if (!guard_compile(t_, (const char*)ef->data, ef->len, &p)) return;
    CHECK(branch_count(&p) >= 6u);   /* seviye + 3 örnek sayısı dalı × iki seviye + parlaklık + kabartı */
    Texture* tex = tex_create(a, 64, 64, TEX_FLAG_MIPMAP | TEX_FLAG_BILINEAR);
    Rng r = rng_make(7);
    for (u32 i = 0; i < 64u * 64u; i++) tex->px[i] = rng_u32(&r) | 0xFF000000u;
    tex_build_mips(tex);
    Texture* sh = tex_create(a, 32, 64, TEX_FLAG_CLAMP);
    for (u32 i = 0; i < 32u * 64u; i++) sh->px[i] = m_f32_bits(0.3f + 0.4f * rng_f32(&r));
    LitUniforms u;
    mem_zero(&u, sizeof(u));
    u.tex = tex;
    u.shadow = sh;
    u.field = tex;
    u.rough = 1.0f;
    u.to_light = v3(0.3f, 0.8f, 0.5f);
    u.ambient = 0.3f;
    u.light_color = v3(1.0f, 0.95f, 0.9f);
    u.fog_start = 2.0f;
    u.fog_end = 30.0f;
    u.fog_color = v3(0.6f, 0.7f, 0.8f);
    u.use_mip = TRUE;
    u.tint = v3(1.0f, 0.9f, 0.8f);
    u.wrap = 0.5f;
    u.translucency = 0.2f;
    u.shadow_mat = mat4_mul(mat4_translate(v3(0.5f, 0.5f, 0.5f)), mat4_scale(v3(0.25f, 0.25f, 0.25f)));   /* ±2 yakın kademede */
    u.shadow_bias = 0.001f;
    u.shadow_mat2 = mat4_mul(mat4_translate(v3(0.5f, 0.5f, 0.5f)), mat4_scale(v3(0.125f, 0.125f, 0.125f)));
    u.shadow_bias2 = 0.002f;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 1000, 71) > 500u);
    u.use_mip = FALSE;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 300, 72) > 100u);
    /* parlaklık + kabartı kolları (D45): 0'dayken yukarıdaki iki koşu kolları atlar, burada çalışırlar */
    u.use_mip = TRUE;
    u.metal = 0.5f;
    u.rough = 0.3f;
    u.bump = 0.03f;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 1000, 73) > 500u);
    u.bump = 0.0f;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 300, 74) > 100u);
    u.rough = 1.0f;
    u.bump = 0.03f;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 300, 75) > 100u);
    /* fizik alanı kolu (D49) */
    u.field_scale = 0.1f;
    CHECK(guard_compare(t_, &p, &u, VAR3D_COUNT, 300, 76) > 100u);
}

/* gölge çarpanı [0, 1]'den çıkmaz (D45): beş noktalı ön bakış quad'da sabit değilken q anlamsız, kenar dalına gitmeli; gitmezse çarpan
 * piksel x'i kadar büyür. Beyaz doku, ortamsız ışık → çıktı en çok ışığın ton eğrisinden geçmiş hali; rastgele derinlikte iki dal da çalışır */
TEST(lit_hq_shadow_factor_in_range) {
    Arena* a = test_arena();
    const EmbedFile* ef = embed_find("lit_hq.mtk");
    CHECK(ef != NULL);
    MatokiProgram p;
    if (!guard_compile(t_, (const char*)ef->data, ef->len, &p)) return;
    Texture* tex = tex_create(a, 2, 2, TEX_FLAG_BILINEAR);
    for (u32 i = 0; i < 4u; i++) tex->px[i] = 0xFFFFFFFFu;
    Texture* sh = tex_create(a, 32, 64, TEX_FLAG_CLAMP);
    Rng r = rng_make(91);
    for (u32 i = 0; i < 32u * 64u; i++) sh->px[i] = m_f32_bits(rng_range(&r, 0.0f, 1.0f));
    LitUniforms u;
    mem_zero(&u, sizeof(u));
    u.tex = tex;
    u.shadow = sh;
    u.field = tex;
    u.rough = 1.0f;
    u.to_light = v3(0.0f, 1.0f, 0.0f);
    u.light_color = v3(0.4f, 0.4f, 0.4f);
    u.fog_start = 1000.0f;
    u.fog_end = 2000.0f;
    u.tint = v3(1.0f, 1.0f, 1.0f);
    u.shadow_mat = mat4_mul(mat4_translate(v3(0.5f, 0.5f, 0.5f)), mat4_scale(v3(0.25f, 0.25f, 0.25f)));
    u.shadow_mat2 = mat4_mul(mat4_translate(v3(0.5f, 0.5f, 0.5f)), mat4_scale(v3(0.125f, 0.125f, 0.125f)));
    if (p.prelude) p.prelude(&u);
    u32 top = 0;
    for (u32 k = 0; k < 4000u; k++) {
        ShadeQuad q;
        quad_random(&q, &r, VAR3D_COUNT);
        _Alignas(16) u32 o[4];
        p.fn_fast(&q, &u, o);
        for (u32 l = 0; l < 4; l++) if ((o[l] & 0xFFu) > top) top = o[l] & 0xFFu;
    }
    test_note("gölge aralığı: en parlak kanal", (f64)top, 0);
    CHECK(top > 0u);
    CHECK(top <= 168u);   /* tone(0,4² · 1,75) · 255 + yuvarlama; taşan çarpan 255'e doyar */
}

/* aynı maskeli iki select arasında koşulsuz bir değer var ve ikincinin kolu onu okuyor: tek dal olsalardı kol, değer
 * hesaplanmadan çalışırdı. Plan bu yüzden yalnız bitişik select'leri gruplar */
TEST(guard_same_mask_not_adjacent) {
    static const char S[] =
        "uniform k: f32;\nvarying v: vec2;\n"
        "fn heavy(x: f32) -> f32 { return exp2(sin(x) * 2.0) + log2(abs(cos(x)) + 1.0); }\n"
        "fn frag() -> vec4 {\n"
        "  let c = fwidth(v.x) > k;\n"
        "  let s1 = select(c, heavy(v.x), heavy(v.y));\n"
        "  let x = s1 * 2.0 + 1.0;\n"
        "  let s2 = select(c, heavy(x), heavy(x + 1.0));\n"
        "  return vec4(fract(s2), fract(x), 0.0, 1.0);\n"
        "}\n";
    MatokiProgram p;
    if (!guard_compile(t_, SRC(S), &p)) return;
    CHECK(branch_count(&p) == 2u);
    const f32 uni[1] = { 0.02f };
    CHECK(guard_compare(t_, &p, uni, 2, 300, 81) > 50u);
}
