/* tests/test_matoki_fuzz.c — Matoki derleyicisi sağlamlığı (M12 Faz 4).
 * Gerçek shader'ların rastgele mutasyonları ve rastgele jeton çorbası: derleyici hiçbir girdide çökmez; hata verirse
 * konum kaynak içinde, mesaj boş değil. Aşırı derinlik (iç içe parantez / blok / tekli eksi) ve çok büyük program. */
#include "test.h"
#include "core/embed.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "core/rng.h"
#include "matoki/matoki.h"

#define FUZZ_ARENA (16u << 20)

/* derleme arenası: her TEST başında yeniden kurulur (test_arena testler arası sıfırlanır; static tutmak sonraki
 * testin tamponlarıyla çakışır) */
static Arena g_fuzz;

static void fuzz_begin(void) { arena_init(&g_fuzz, arena_alloc(test_arena(), FUZZ_ARENA, 16), FUZZ_ARENA); }

static Arena* fuzz_arena(void) {
    arena_reset(&g_fuzz, 0);
    return &g_fuzz;
}

/* çakışan aralıklar için bayt taşıma */
static void bmove(char* dst, const char* src, u32 n) {
    if (dst < src) for (u32 i = 0; i < n; i++) dst[i] = src[i];
    else for (u32 i = n; i > 0; i--) dst[i - 1] = src[i - 1];
}

static u32 line_count(const char* s, u32 n) {
    u32 l = 1;
    for (u32 i = 0; i < n; i++) l += s[i] == '\n';
    return l;
}

typedef struct FuzzStats {
    u32 ok, fail, bad_err;
} FuzzStats;

static void fuzz_compile(const char* src, u32 len, u32 stage, FuzzStats* st) {
    Arena* a = fuzz_arena();
    MatokiOptions opt;
    mem_zero(&opt, sizeof(opt));
    opt.stage = stage;
    MatokiProgram p;
    MatokiError e;
    mem_zero(&e, sizeof(e));
    if (matoki_compile(a, src, len, &opt, &p, &e)) {
        st->ok++;
        return;
    }
    st->fail++;
    /* hata: konum kaynak içinde (1 tabanlı), mesaj dolu */
    if (!e.failed || e.msg[0] == 0 || e.line < 1 || (u32)e.line > line_count(src, len) + 1 || e.col < 1) st->bad_err++;
}

static const char* const VOCAB[] = {
    "fn", "frag", "vert", "let", "var", "if", "else", "for", "in", "return", "uniform", "varying", "attribute",
    "discard", "vec2", "vec3", "vec4", "f32", "i32", "mat4", "mat3", "texture2d", "sample", "dot", "normalize",
    "mix", "clamp", "sqrt", "(", ")", "{", "}", "[", "]", ";", ":", ",", ".", "..", "->", "=", "+=", "+", "-", "*",
    "/", "<", ">", "<=", "==", "!=", "&&", "||", "!", "?", "0", "1.0", "1e38", "1e-45", "0x7fffffff", "99999999999",
    "x", "y", "xyz", "wpos", "nrm", "uv", "tex", "\n", " ", "//c\n", "/*", "*/", "\"", "@", "#", "\t",
};

/* rastgele mutasyon: bayt değiştir / sil / jeton ekle / bölüm kopyala / kes */
static u32 mutate(Rng* r, const char* src, u32 len, char* out, u32 cap) {
    u32 n = len < cap ? len : cap;
    mem_copy(out, src, n);
    u32 ops = 1 + rng_u32(r) % 4;
    for (u32 o = 0; o < ops && n > 0; o++) {
        u32 at = rng_u32(r) % n;
        switch (rng_u32(r) % 5) {
        case 0: out[at] = (char)(32 + rng_u32(r) % 95); break;
        case 1: {
            u32 del = 1 + rng_u32(r) % 16;
            if (at + del > n) del = n - at;
            bmove(out + at, out + at + del, n - at - del);
            n -= del;
            break;
        }
        case 2: {
            const char* t = VOCAB[rng_u32(r) % ARRAY_COUNT(VOCAB)];
            u32 tl = (u32)cstr_len(t);
            if (n + tl + 1 >= cap) break;
            bmove(out + at + tl + 1, out + at, n - at);
            mem_copy(out + at, t, tl);
            out[at + tl] = ' ';
            n += tl + 1;
            break;
        }
        case 3: {
            u32 sl = 1 + rng_u32(r) % 64;
            if (at + sl > n || n + sl >= cap) break;
            u32 to = rng_u32(r) % n;
            bmove(out + to + sl, out + to, n - to);
            mem_copy(out + to, out + (at < to ? at : at + sl), sl);
            n += sl;
            break;
        }
        default: n = at; break;   /* kes */
        }
    }
    return n;
}

TEST(matoki_fuzz_mutated_real_shaders) {
    fuzz_begin();
    static const char* const FILES[] = { "lit.mtk", "lit_hq.mtk", "water.mtk", "sky.mtk", "std_vert.mtk", "wave_vert.mtk" };
    static const u32 STAGE[] = { MATOKI_STAGE_FRAG, MATOKI_STAGE_FRAG, MATOKI_STAGE_FRAG, MATOKI_STAGE_FRAG,
                                 MATOKI_STAGE_VERT, MATOKI_STAGE_VERT };
    const u32 CAP = 64u * 1024u;
    char* buf = ARENA_ARRAY(test_arena(), char, CAP);
    Rng r = rng_make(12345);
    FuzzStats st = { 0, 0, 0 };
    for (u32 f = 0; f < ARRAY_COUNT(FILES); f++) {
        const EmbedFile* ef = embed_find(FILES[f]);
        CHECK(ef != NULL);
        if (!ef) continue;
        FuzzStats base = { 0, 0, 0 };
        fuzz_compile((const char*)ef->data, ef->len, STAGE[f], &base);
        CHECK(base.ok == 1);   /* değiştirilmemiş kaynak derlenir */
        for (u32 i = 0; i < 800; i++) {
            u32 n = mutate(&r, (const char*)ef->data, ef->len, buf, CAP);
            fuzz_compile(buf, n, STAGE[f], &st);
        }
    }
    test_note("mutasyon: derlenen", st.ok, 0);
    test_note("mutasyon: hata veren", st.fail, 0);
    test_note("mutasyon: konumu/mesaji bozuk hata", st.bad_err, 0);
    CHECK(st.fail > 1000);
    CHECK(st.bad_err == 0);
}

TEST(matoki_fuzz_token_soup) {
    fuzz_begin();
    char* buf = ARENA_ARRAY(test_arena(), char, 8192);
    Rng r = rng_make(999);
    FuzzStats st = { 0, 0, 0 };
    for (u32 i = 0; i < 3000; i++) {
        StrBuf sb;
        sb_init(&sb, buf, 8191);
        if (i % 2) sb_cstr(&sb, "fn frag() -> vec4 { ");
        u32 toks = rng_u32(&r) % 200;
        for (u32 k = 0; k < toks; k++) {
            sb_cstr(&sb, VOCAB[rng_u32(&r) % ARRAY_COUNT(VOCAB)]);
            sb_char(&sb, ' ');
        }
        fuzz_compile(buf, sb.len, i % 3 ? MATOKI_STAGE_FRAG : MATOKI_STAGE_VERT, &st);
    }
    test_note("corba: hata veren", st.fail, 0);
    test_note("corba: bozuk hata", st.bad_err, 0);
    CHECK(st.bad_err == 0);
}

/* pre + tekrar(open)×n + mid + tekrar(close)×n + post */
static u32 nest(char* out, u32 cap, const char* pre, const char* open, u32 n, const char* mid, const char* close, const char* post) {
    StrBuf sb;
    sb_init(&sb, out, cap - 1);
    sb_cstr(&sb, pre);
    for (u32 i = 0; i < n; i++) sb_cstr(&sb, open);
    sb_cstr(&sb, mid);
    for (u32 i = 0; i < n; i++) sb_cstr(&sb, close);
    sb_cstr(&sb, post);
    return sb.len;
}

TEST(matoki_fuzz_deep_nesting_no_crash) {
    fuzz_begin();
    const u32 CAP = 512u * 1024u;
    char* buf = ARENA_ARRAY(test_arena(), char, CAP);
    static const u32 DEPTH[] = { 50, 20000 };
    for (u32 d = 0; d < 2; d++) {
        FuzzStats st = { 0, 0, 0 };
        u32 n = nest(buf, CAP, "fn frag() -> vec4 { let x = ", "(", DEPTH[d], "1.0", ")", "; return vec4(x, x, x, 1.0); }");
        fuzz_compile(buf, n, MATOKI_STAGE_FRAG, &st);
        n = nest(buf, CAP, "fn frag() -> vec4 { let x = ", "-", DEPTH[d], "1.0", "", "; return vec4(x, x, x, 1.0); }");
        fuzz_compile(buf, n, MATOKI_STAGE_FRAG, &st);
        n = nest(buf, CAP, "fn frag() -> vec4 { var x = 1.0; ", "if x > 0.0 { ", DEPTH[d], "x = 2.0; ", "} ", "return vec4(x, x, x, 1.0); }");
        fuzz_compile(buf, n, MATOKI_STAGE_FRAG, &st);
        test_note(d ? "derinlik 20000: derlenen" : "derinlik 50: derlenen", st.ok, 0);
        CHECK(st.bad_err == 0);
        if (d == 0) CHECK(st.ok == 3);   /* makul derinlik derlenir */
    }
}

TEST(matoki_fuzz_large_program) {
    fuzz_begin();
    const u32 CAP = 256u * 1024u;
    char* buf = ARENA_ARRAY(test_arena(), char, CAP);
    StrBuf sb;
    sb_init(&sb, buf, CAP - 1);
    sb_cstr(&sb, "varying uv: vec2;\nfn frag() -> vec4 {\n    var a = uv.x;\n");
    for (u32 i = 0; i < 3000; i++) sb_cstr(&sb, "    a = a * 0.999 + uv.y * 0.001;\n");
    sb_cstr(&sb, "    return vec4(a, a, a, 1.0);\n}\n");
    FuzzStats st = { 0, 0, 0 };
    fuzz_compile(buf, sb.len, MATOKI_STAGE_FRAG, &st);
    test_note("buyuk program (3000 satir) derlendi", st.ok, 0);
    CHECK(st.ok == 1);
}
