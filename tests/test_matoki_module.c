/* tests/test_matoki_module.c — Matoki modül sistemi (D57): const, const değiştirme (override), import.
 * const yerinde katlanır: literal yazılmış eşiyle aynı IR ve aynı çıktı. import bellek içi çözücüyle: bağımlılık önce,
 * aynı uniform iki dosyada bir kez, döngü / eksik dosya / çakışan bildirim hata, import'taki hata o dosyanın adı ve satırıyla.
 * Gerçek shader'lar: lit_hq'nun gölgesiz türevi SHADOWS = false ile gölge kodunu hiç taşımaz. */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "matoki/ir_eval.h"
#include "matoki/matoki.h"
#include "gfx3d/lit_layout.h"

static b32 mod_compile(const char* src, const MatokiOptions* o, MatokiProgram* p, MatokiError* e) {
    return matoki_compile(test_arena(), src, (u32)cstr_len(src), o, p, e);
}

static b32 msg_has(const MatokiError* e, const char* needle) {
    usize n = cstr_len(needle), h = cstr_len(e->msg);
    for (usize i = 0; i + n <= h; i++) {
        if (mem_equal(e->msg + i, needle, n)) return TRUE;
    }
    return FALSE;
}

static b32 fails_with(const char* src, const MatokiOptions* o, const char* needle) {
    MatokiProgram p;
    MatokiError e;
    if (mod_compile(src, o, &p, &e)) return FALSE;
    return msg_has(&e, needle);
}

/* uniform'suz, varying'siz programın dört lane çıktısı (paketlenmiş renk) */
static void run_const(const MatokiProgram* p, u32 out[4]) {
    ShadeQuad q;
    mem_zero(&q, sizeof(q));
    q.mask = 0xF;
    u32 uni[16];
    mem_zero(uni, sizeof(uni));
    Arena scratch;
    arena_init(&scratch, arena_alloc(test_arena(), 1 << 16, 16), 1 << 16);
    ir_eval(&p->ir, &q, uni, out, &scratch);
}

TEST(matoki_const_same_as_literal) {
    static const char A[] =
        "const HALF = 0.5;\n"
        "const TINT = vec3(0.25, HALF, 0.75) * 0.5;\n"
        "const N: i32 = 3;\n"
        "fn frag() -> vec4 { var s = 0.0; for i in 0..N { s += 0.125; } return vec4(TINT + vec3(s * HALF), 1.0); }\n";
    static const char B[] =
        "fn frag() -> vec4 { var s = 0.0; for i in 0..3 { s += 0.125; } return vec4(vec3(0.25, 0.5, 0.75) * 0.5 + vec3(s * 0.5), 1.0); }\n";
    MatokiProgram pa, pb;
    MatokiError e;
    CHECK(mod_compile(A, NULL, &pa, &e));
    CHECK(mod_compile(B, NULL, &pb, &e));
    if (e.failed) return;
    CHECK(pa.ir_live == pb.ir_live);
    u32 oa[4], ob[4];
    run_const(&pa, oa);
    run_const(&pb, ob);
    CHECK(mem_equal(oa, ob, sizeof(oa)));
}

TEST(matoki_const_rules) {
    CHECK(fails_with("uniform u: f32;\nconst K = u * 2.0;\nfn frag() -> vec4 { return vec4(K); }", NULL, "derleme zamanında"));
    CHECK(fails_with("varying v: f32;\nconst K = v;\nfn frag() -> vec4 { return vec4(K); }", NULL, "derleme zamanında"));
    CHECK(fails_with("fn h() -> f32 { return 1.0; }\nconst K = h();\nfn frag() -> vec4 { return vec4(K); }", NULL, "derleme zamanında"));
    CHECK(fails_with("const K = frag_coord().x;\nfn frag() -> vec4 { return vec4(K); }", NULL, "derleme zamanında"));
    CHECK(fails_with("const A = B;\nconst B = 1.0;\nfn frag() -> vec4 { return vec4(A); }", NULL, "tanımsız isim"));
    CHECK(fails_with("const K = 1.0;\nfn frag() -> vec4 { K = 2.0; return vec4(K); }", NULL, "değiştirilemez"));
    CHECK(fails_with("const K: f32 = true;\nfn frag() -> vec4 { return vec4(1.0); }", NULL, "bildirilen tiple"));
    CHECK(fails_with("const K = 1.0;\nconst K = 2.0;\nfn frag() -> vec4 { return vec4(K); }", NULL, "yinelenen global"));
    CHECK(fails_with("const K = 1.5;\nfn frag() -> vec4 { var s = 0.0; for i in 0..K { s += 1.0; } return vec4(s); }", NULL, "tamsayı değerli"));
    CHECK(fails_with("uniform n: i32;\nfn frag() -> vec4 { var s = 0.0; for i in 0..n { s += 1.0; } return vec4(s); }", NULL, "for sınırı"));
    /* tamsayı literali f32 bildirilmiş const'ta f32 olur */
    MatokiProgram p;
    MatokiError e;
    CHECK(mod_compile("const K: f32 = 2;\nfn frag() -> vec4 { return vec4(K * 0.25); }", NULL, &p, &e));
}

TEST(matoki_override_replaces_value) {
    static const char SRC[] = "const K = 0.5;\nconst ON = true;\nfn frag() -> vec4 { return vec4(select(ON, K, 0.0)); }\n";
    static const MatokiOverride OV[] = { { "K", 0.25f } };
    static const MatokiOverride OFF[] = { { "ON", 0.0f } };
    static const MatokiOverride BAD[] = { { "YOK", 1.0f } };
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    MatokiProgram pk, pl, poff, plz;
    MatokiError e;
    o.overrides.items = OV;
    o.overrides.count = 1;
    CHECK(mod_compile(SRC, &o, &pk, &e));
    CHECK(mod_compile("fn frag() -> vec4 { return vec4(0.25); }", NULL, &pl, &e));
    o.overrides.items = OFF;
    CHECK(mod_compile(SRC, &o, &poff, &e));
    CHECK(mod_compile("fn frag() -> vec4 { return vec4(0.0); }", NULL, &plz, &e));
    if (e.failed) return;
    u32 a[4], b[4];
    run_const(&pk, a);
    run_const(&pl, b);
    CHECK(mem_equal(a, b, sizeof(a)));
    run_const(&poff, a);
    run_const(&plz, b);
    CHECK(mem_equal(a, b, sizeof(a)));
    o.overrides.items = BAD;
    CHECK(fails_with(SRC, &o, "kaynakta yok"));
    static const MatokiOverride VEC[] = { { "V", 1.0f } };
    o.overrides.items = VEC;
    CHECK(fails_with("const V = vec2(1.0, 2.0);\nfn frag() -> vec4 { return vec4(V, 0.0, 1.0); }", &o, "skaler"));
}

/* bellek içi dosyalar */
typedef struct MemFile {
    const char* name;
    const char* src;
} MemFile;

static const MemFile MEM_FILES[] = {
    { "a.mtk", "uniform u: f32;\nconst SCALE = 2.0;\nfn twice(x: f32) -> f32 { return x * SCALE * u; }\n" },
    { "b.mtk", "import \"a.mtk\";\nuniform u: f32;\nuniform w: f32;\nfn plus(x: f32) -> f32 { return twice(x) + w; }\n" },
    { "c.mtk", "import \"d.mtk\";\nfn fc() -> f32 { return 1.0; }\n" },
    { "d.mtk", "import \"c.mtk\";\nfn fd() -> f32 { return 1.0; }\n" },
    { "bad.mtk", "fn ok() -> f32 { return 1.0; }\n\nfn broken() -> f32 { return tanimsiz; }\n" },
    { "clash.mtk", "uniform u: vec2;\n" },
};

static b32 mem_resolve(void* user, const char* name, u32 name_len, const char** src, u32* len) {
    u32* calls = (u32*)user;
    (*calls)++;
    for (u32 i = 0; i < ARRAY_COUNT(MEM_FILES); i++) {
        if (cstr_len(MEM_FILES[i].name) == name_len && mem_equal(MEM_FILES[i].name, name, name_len)) {
            *src = MEM_FILES[i].src;
            *len = (u32)cstr_len(MEM_FILES[i].src);
            return TRUE;
        }
    }
    return FALSE;
}

TEST(matoki_import_resolves_once) {
    u32 calls = 0;
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    o.resolve = mem_resolve;
    o.resolve_user = &calls;
    MatokiProgram p;
    MatokiError e;
    /* b, a'yı da çeker; a ikinci kez okunmaz; u üç dosyada aynı bildirim → tek uniform */
    static const char MAIN[] = "import \"b.mtk\";\nimport \"a.mtk\";\nuniform u: f32;\nfn frag() -> vec4 { return vec4(plus(0.5) + twice(0.25)); }\n";
    CHECK(mod_compile(MAIN, &o, &p, &e));
    CHECK(calls == 2);
    if (e.failed) return;
    /* bildirim sırası: a'nın u'su (bağımlılık önce), sonra b'nin w'si */
    CHECK(matoki_find_uniform(&p, "u")->offset == 0u);
    CHECK(matoki_find_uniform(&p, "w")->offset == 4u);
    CHECK(p.sema.uniform_size == 8u);
}

TEST(matoki_import_errors) {
    u32 calls = 0;
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    o.resolve = mem_resolve;
    o.resolve_user = &calls;
    MatokiProgram p;
    MatokiError e;
    CHECK(fails_with("import \"c.mtk\";\nfn frag() -> vec4 { return vec4(fc()); }", &o, "döngüsel"));
    CHECK(fails_with("import \"yok.mtk\";\nfn frag() -> vec4 { return vec4(1.0); }", &o, "bulunamadı"));
    CHECK(fails_with("uniform x: f32;\nimport \"a.mtk\";\nfn frag() -> vec4 { return vec4(x); }", &o, "başında"));
    /* farklı tipte aynı ad birleşmez */
    CHECK(fails_with("import \"a.mtk\";\nimport \"clash.mtk\";\nfn frag() -> vec4 { return vec4(1.0); }", &o, "yinelenen global"));
    /* aynı dosyada iki kez aynı bildirim hâlâ hata (import varken de: birleştirme yalnız dosyalar arası) */
    CHECK(fails_with("import \"a.mtk\";\nuniform k: f32;\nuniform k: f32;\nfn frag() -> vec4 { return vec4(k); }", &o, "yinelenen global"));
    /* import'taki hata: dosya adı + o dosyadaki satır; ana kaynakta zinciri başlatan import satırı */
    static const char MAIN[] = "// açıklama\nimport \"bad.mtk\";\nfn frag() -> vec4 { return vec4(ok()); }\n";
    CHECK(!mod_compile(MAIN, &o, &p, &e));
    CHECK(e.line == 3 && e.via_line == 2);
    CHECK(mem_equal(e.file, "bad.mtk", 8));
    char buf[256];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    matoki_format_error(&e, &sb);
    CHECK(sb.len > 10 && mem_equal(buf, "bad.mtk:3:", 10));
    /* ana kaynaktaki hata dosya adı taşımaz */
    CHECK(!mod_compile("import \"a.mtk\";\nfn frag() -> vec4 { return vec4(nope); }", &o, &p, &e));
    CHECK(e.file[0] == 0 && e.line == 2);
}

/* gerçek shader'lar gömülü kütüphanelerle derlenir; gölgesiz türev gölge kodunu taşımaz */
TEST(matoki_lit_hq_shadows_override) {
    const EmbedFile* ef = embed_find("lit_hq.mtk");
    CHECK(ef != NULL);
    if (!ef) return;
    MatokiOptions o;
    mem_zero(&o, sizeof(o));
    o.layout = &LIT_LAYOUT;
    MatokiProgram on, off;
    MatokiError e;
    CHECK(matoki_compile(test_arena(), (const char*)ef->data, ef->len, &o, &on, &e));
    static const MatokiOverride OFF[] = { { "SHADOWS", 0.0f } };
    o.overrides.items = OFF;
    o.overrides.count = 1;
    CHECK(matoki_compile(test_arena(), (const char*)ef->data, ef->len, &o, &off, &e));
    if (e.failed) return;
    /* gölge haritasının ofsetinden okuma kalmamalı */
    u32 shadow_ofs = matoki_find_uniform(&on, "shadow")->offset, reads_on = 0, reads_off = 0;
    for (u32 i = 1; i < on.ir.count; i++) reads_on += on.ir.ins[i].live && on.ir.ins[i].op == IR_TEXEL_LOAD && on.ir.ins[i].imm == shadow_ofs;
    for (u32 i = 1; i < off.ir.count; i++) reads_off += off.ir.ins[i].live && off.ir.ins[i].op == IR_TEXEL_LOAD && off.ir.ins[i].imm == shadow_ofs;
    CHECK(reads_on > 0 && reads_off == 0);
    CHECK(off.ir_live + 100u < on.ir_live);
    test_note("lit_hq canlı IR (gölgeli)", on.ir_live, 0);
    test_note("lit_hq canlı IR (gölgesiz)", off.ir_live, 0);
}
