/* tests/test_matoki_wgsl.c — Matoki → WGSL arka ucu (matoki/wgsl, D42). Sabitler birebir (onaltılık ↔ bit), demo
 * shader'larının metni yapısal olarak sağlam (her vN kullanımdan önce tanımlı, giriş noktası, doku bağlamaları), IR'daki
 * her komutun karşılığı var (yeni komut eklenince test kırılır), GPU sınırını aşan program hata verir. Anlam denkliği
 * burada değil: JIT ↔ WGSL görüntü karşılaştırması tarayıcıda (web/gpu_check.html). */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "core/sys.h"
#include "matoki/matoki.h"
#include "matoki/wgsl.h"
#include "raster/ref_shaders.h"

static b32 str_has(const char* s, u32 n, const char* key) {
    u32 k = (u32)cstr_len(key);
    for (u32 i = 0; i + k <= n; i++) {
        if (mem_equal(s + i, key, k)) return TRUE;
    }
    return FALSE;
}

static i32 hex_val(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* wgsl_f32_literal çıktısını çözer: [-]0x(1|0).HHHHHHp(+|-)D+f ya da [-]0.0f */
static u32 literal_bits(const char* s) {
    u32 sign = 0;
    if (*s == '-') {
        sign = 1u << 31;
        s++;
    }
    if (s[0] == '0' && s[1] == '.') return sign;
    u32 lead = (u32)(s[2] - '0'), m = 0;
    s += 4;
    for (u32 i = 0; i < 6; i++) m = (m << 4) | (u32)hex_val(*s++);
    s++;   /* p */
    i32 es = *s == '-' ? -1 : 1;
    if (*s == '+' || *s == '-') s++;
    i32 x = 0;
    while (*s >= '0' && *s <= '9') x = x * 10 + (*s++ - '0');
    u32 exp = lead ? (u32)(es * x + 127) : 0u;
    return sign | (exp << 23) | (m >> 1);
}

TEST(wgsl_float_literals_are_exact) {
    static const u32 BITS[] = { 0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x40490FDBu, 0x7F7FFFFFu, 0x00800000u,
                                0x00000001u, 0x807FFFFFu, 0x3EAAAAABu, 0x358637BDu, 0x4F000000u };
    for (u32 i = 0; i < ARRAY_COUNT(BITS); i++) {
        char buf[32];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf) - 1);
        CHECK(wgsl_f32_literal(&sb, BITS[i]));
        buf[sb.len] = 0;
        CHECK(literal_bits(buf) == BITS[i]);
    }
    char b2[32];
    StrBuf s2;
    sb_init(&s2, b2, sizeof(b2));
    CHECK(!wgsl_f32_literal(&s2, 0x7F800000u));
    CHECK(!wgsl_f32_literal(&s2, 0xFFC00000u));
}

/* her vN / kN kullanımı önce `let` ile tanımlı olmalı; dönüş: tanımlı let sayısı, bozuksa 0 */
static u32 check_ssa(const char* s, u32 n, Arena* a) {
    u8* seen_v = (u8*)arena_alloc(a, 70000, 1);
    u8* seen_k = (u8*)arena_alloc(a, 70000, 1);
    u32 lets = 0, start = 0;
    while (start + 3 < n && !mem_equal(s + start, "fn mask", 7)) start++;   /* VOut alanları (v0, v1) değişken değil */
    for (u32 i = start; i < n; i++) {
        char c = s[i];
        b32 word_start = i == 0 || !((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                                     (s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '_' || s[i - 1] == '.');
        if (!word_start || (c != 'v' && c != 'k')) continue;
        u32 j = i + 1, num = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') num = num * 10 + (u32)(s[j++] - '0');
        if (j == i + 1 || num >= 70000) continue;
        b32 is_decl = i >= 4 && (mem_equal(s + i - 4, "let ", 4) || mem_equal(s + i - 4, "var ", 4));
        u8* seen = c == 'v' ? seen_v : seen_k;
        if (is_decl) {
            if (seen[num]) return 0;   /* iki kez tanım */
            seen[num] = 1;
            lets++;
        } else if (!seen[num]) {
            return 0;   /* tanımsız kullanım */
        }
        i = j - 1;
    }
    return lets;
}

typedef struct WgslCase {
    const char* file;
    u32 stage, tex_count, uniform_size;
} WgslCase;

TEST(wgsl_demo_shaders_structure) {
    const WgslCase CASES[] = {
        { "lit.mtk", MATOKI_STAGE_FRAG, 1, (u32)__builtin_offsetof(LitUniforms, wrap) },
        { "lit_hq.mtk", MATOKI_STAGE_FRAG, 3, (u32)__builtin_offsetof(LitUniforms, lv_count) },
        { "water.mtk", MATOKI_STAGE_FRAG, 2, (u32)__builtin_offsetof(LitUniforms, field) },
        { "sky.mtk", MATOKI_STAGE_FRAG, 0, (u32)__builtin_offsetof(LitUniforms, shadow) },
        { "std_vert.mtk", MATOKI_STAGE_VERT, 0, 168 },
        { "wave_vert.mtk", MATOKI_STAGE_VERT, 0, 184 },
    };
    Arena* a = test_arena();
    for (u32 i = 0; i < ARRAY_COUNT(CASES); i++) {
        const WgslCase* k = &CASES[i];
        const EmbedFile* f = embed_find(k->file);
        CHECK(f != NULL);
        MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
        MatokiOptions o;
        mem_zero(&o, sizeof(o));
        o.stage = k->stage;
        MatokiError e;
        CHECK(matoki_compile(a, (const char*)f->data, f->len, &o, p, &e));
        MatokiWgsl w;
        f64 t0 = sys_now_ms();
        usize mark = arena_mark(a);
        CHECK(matoki_wgsl(a, p, &w, &e));
        test_note(k->file, sys_now_ms() - t0, 3);
        CHECK(arena_mark(a) == mark + w.len);   /* yalnız metin kadar bellek kalır */
        test_note("  WGSL bayt", (f64)w.len, 0);
        CHECK(w.tex_count == k->tex_count);
        CHECK(w.uniform_size == k->uniform_size);
        for (u32 t = 0; t < w.tex_count; t++) {
            const char* decl = t ? "@group(1) @binding(1) var<storage, read> t1: array<u32>;" : "@group(1) @binding(0) var<storage, read> t0: array<u32>;";
            CHECK(str_has(w.text, w.len, decl));
        }
        CHECK(!str_has(w.text, w.len, "t-1"));   /* eşlenmemiş doku */
        b32 vert = k->stage == MATOKI_STAGE_VERT;
        CHECK(str_has(w.text, w.len, vert ? "@vertex fn vs(" : "@fragment fn fs(in: VOut)"));
        CHECK(str_has(w.text, w.len, vert ? "@group(0) @binding(0) var<uniform> u" : "@group(0) @binding(1) var<uniform> u"));
        if (vert) {
            /* konum ve tüm varying'ler yazılır */
            CHECK(str_has(w.text, w.len, "o.pos.x") && str_has(w.text, w.len, "o.pos.w") && str_has(w.text, w.len, "o.v1.y"));
        }
        u32 lets = check_ssa(w.text, w.len, a);
        CHECK(lets > 0u);
        /* bilinear sample_level donanımda (D50); JIT IR'ı aynı kalır, GPU IR'ı onu içermez */
        if (k->uniform_size && k->tex_count > 1u) CHECK(str_has(w.text, w.len, "textureSampleLevel"));
    }
}

/* hangi işlemler F32 işlenen alır (tablo: ir.h sırası) */
static b32 op_float_operands(u32 op) {
    return (op >= IR_FADD && op <= IR_FGE) || op == IR_F2I || op == IR_BITCAST_F2I || op == IR_DERIV_X || op == IR_DERIV_Y;
}

/* her komutun ifadesi: görüntü testi anlamı neredeyse koruyan yanlışları görmez (exp2'de round → floor: ortalama fark aynı) */
typedef struct OpExpr {
    u32 op;
    const char* expr;
} OpExpr;

static const OpExpr OP_EXPR[] = {
    { IR_FADD, " + " }, { IR_FSUB, " - " }, { IR_FMUL, " * " }, { IR_FDIV, " / " }, { IR_FMIN, "min(" }, { IR_FMAX, "max(" },
    { IR_FNEG, "= -v" }, { IR_FABS, "abs(" }, { IR_FSQRT, "sqrt(" }, { IR_FFLOOR, "floor(" }, { IR_FCEIL, "ceil(" },
    { IR_FTRUNC, "trunc(" }, { IR_FNEAREST, "round(" }, { IR_FEQ, " == " }, { IR_FNE, " != " }, { IR_FLT, " < " },
    { IR_FLE, " <= " }, { IR_FGT, " > " }, { IR_FGE, " >= " }, { IR_IADD, " + " }, { IR_ISUB, " - " }, { IR_IMUL, " * " },
    { IR_INEG, "= -v" }, { IR_IAND, " & " }, { IR_IOR, " | " }, { IR_IXOR, " ^ " }, { IR_ISHL, " << " }, { IR_ISHR, " >> (" },
    { IR_ISHRU, "bitcast<u32>(v" }, { IR_IMIN, "min(" }, { IR_IMAX, "max(" }, { IR_IEQ, " == " }, { IR_INE, " != " },
    { IR_ILT, " < " }, { IR_ILE, " <= " }, { IR_IGT, " > " }, { IR_IGE, " >= " }, { IR_F2I, "i32(v" }, { IR_I2F, "f32(v" },
    { IR_BITCAST_F2I, "bitcast<i32>(v" }, { IR_BITCAST_I2F, "bitcast<f32>(v" }, { IR_DERIV_X, "dpdxCoarse(" }, { IR_DERIV_Y, "dpdyCoarse(" },
};

/* `let vN:` satırında key geçiyor mu */
static b32 line_has(const MatokiWgsl* w, IrRef r, const char* key) {
    char head[24];
    StrBuf sb;
    sb_init(&sb, head, sizeof(head) - 1);
    sb_cstr(&sb, "let v");
    sb_u64(&sb, r);
    sb_char(&sb, ':');
    head[sb.len] = 0;
    for (u32 i = 0; i + sb.len <= w->len; i++) {
        if (!mem_equal(w->text + i, head, sb.len)) continue;
        u32 e = i;
        while (e < w->len && w->text[e] != 10) e++;
        return str_has(w->text + i, e - i, key);
    }
    return FALSE;
}

TEST(wgsl_every_ir_op_has_translation) {
    Arena* a = test_arena();
    MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
    mem_zero(p, sizeof(*p));
    IrFunc* f = &p->ir;
    ir_init(f, a, 1024, TRUE);
    IrRef fv = ir_emit(f, IR_VARYING, IR_T_F32, 0, 0, 0, 0);
    IrRef iv = ir_emit(f, IR_UNIFORM, IR_T_I32, 0, 0, 0, 4);
    IrRef fu = ir_emit(f, IR_UNIFORM, IR_T_F32, 0, 0, 0, 8);
    ir_emit(f, IR_ATTR, IR_T_F32, 0, 0, 0, 0);   /* frag'da anlamsız ama çeviri var */
    ir_emit(f, IR_CONST, IR_T_F32, 0, 0, 0, 0x7F800000u);   /* inf: let k üzerinden */
    ir_emit(f, IR_CONST, IR_T_F32, 0, 0, 0, 0x3F800000u);
    ir_emit(f, IR_CONST, IR_T_I32, 0, 0, 0, 0xFFFFFFFFu);
    ir_emit(f, IR_FRAG_X, IR_T_F32, 0, 0, 0, 0);
    ir_emit(f, IR_FRAG_Y, IR_T_F32, 0, 0, 0, 0);
    ir_emit(f, IR_FRAG_Z, IR_T_F32, 0, 0, 0, 0);
    ir_emit(f, IR_TEX_FIELD, IR_T_I32, 0, 0, 0, 0u | (4u << 16));
    ir_emit(f, IR_TEXEL_LOAD, IR_T_I32, iv, 0, 0, 0);
    ir_emit(f, IR_TEX_WORD, IR_T_I32, iv, 0, 0, 0);
    ir_emit(f, IR_HW_SAMPLE, IR_T_F32, fv, fu, iv, 0u | (2u << 16) | (1u << 18));
    IrRef refs[IR_OP_COUNT];
    mem_zero(refs, sizeof(refs));
    for (u32 op = IR_FADD; op < IR_OP_COUNT; op++) {
        if (op == IR_SELECT) {
            ir_emit(f, IR_SELECT, IR_T_F32, iv, fv, fu, 0);
            ir_emit(f, IR_SELECT, IR_T_I32, iv, iv, iv, 0);
            continue;
        }
        b32 fl = op_float_operands(op);
        IrRef x = fl ? fv : iv, y = fl ? fu : iv;
        u32 type = ir_result_type((IrOp)op, fl ? IR_T_F32 : IR_T_I32);
        u32 ar = ir_op_arity((IrOp)op);
        refs[op] = ir_emit(f, (IrOp)op, type, x, ar > 1 ? y : 0, 0, 0);
    }
    f->out = iv;
    f->kill = iv;
    for (u32 i = 1; i < f->count; i++) f->ins[i].live = 1;
    p->stage = MATOKI_STAGE_FRAG;
    MatokiWgsl w;
    MatokiError e;
    CHECK(matoki_wgsl(a, p, &w, &e));
    CHECK(w.tex_count == 1u && w.tex_ofs[0] == 0u);
    CHECK(check_ssa(w.text, w.len, a) == f->count);   /* f->count − 1 komut + 1 k */
    for (u32 k = 0; k < ARRAY_COUNT(OP_EXPR); k++) CHECK(line_has(&w, refs[OP_EXPR[k].op], OP_EXPR[k].expr));
    CHECK(str_has(w.text, w.len, "discard"));
    CHECK(str_has(w.text, w.len, "dpdyCoarse"));
    CHECK(str_has(w.text, w.len, "0x7F800000u"));
    /* köşe aşamasında türev ve doku yok */
    p->stage = MATOKI_STAGE_VERT;
    CHECK(!matoki_wgsl(a, p, &w, &e));
}

static b32 wgsl_of(const char* src, u32 stage, MatokiError* e) {
    Arena* a = test_arena();
    MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    o.stage = stage;
    if (!matoki_compile(a, src, (u32)cstr_len(src), &o, p, e)) {
        mem_zero(e, sizeof(*e));   /* çağıran: msg boş → derleyici reddetti, GPU sınırı değil */
        return FALSE;
    }
    MatokiWgsl w;
    return matoki_wgsl(a, p, &w, e);
}

TEST(wgsl_rejects_programs_over_gpu_limits) {
    MatokiError e;
    CHECK(wgsl_of("fn frag() -> vec4 { return vec4(1.0); }", MATOKI_STAGE_FRAG, &e));
    /* uniform > 256 bayt */
    CHECK(!wgsl_of("uniform a: mat4; uniform b: mat4; uniform c: mat4; uniform d: mat4; uniform e: f32;\n"
                   "fn frag() -> vec4 { return vec4(1.0); }", MATOKI_STAGE_FRAG, &e));
    CHECK(e.msg[0] != 0);
    /* varying > 8 yuva */
    CHECK(!wgsl_of("varying a: vec4; varying b: vec4; varying c: f32;\nfn frag() -> vec4 { return vec4(a.x + b.x + c); }", MATOKI_STAGE_FRAG, &e));
    CHECK(e.msg[0] != 0);
    /* 4 doku */
    CHECK(!wgsl_of("uniform a: texture2d; uniform b: texture2d; uniform c: texture2d; uniform d: texture2d; varying v: vec2;\n"
                   "fn frag() -> vec4 { return sample(a, v) + sample(b, v) + sample(c, v) + sample(d, v); }", MATOKI_STAGE_FRAG, &e));
    CHECK(e.msg[0] != 0);
    /* ölü dokular sayılmaz */
    CHECK(wgsl_of("uniform a: texture2d; uniform b: texture2d; uniform c: texture2d; uniform d: texture2d; varying v: vec2;\n"
                  "fn frag() -> vec4 { return sample(c, v); }", MATOKI_STAGE_FRAG, &e));
    /* köşe aşamasında doku */
    CHECK(!wgsl_of("uniform t: texture2d; attribute p: vec2;\nfn vert() -> vec4 { return vec4(sample(t, p).x); }", MATOKI_STAGE_VERT, &e));
    CHECK(e.msg[0] != 0);
}
