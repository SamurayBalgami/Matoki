/* matoki/matoki.c — derleme hattını birleştirir.
 * Her aşama ilk hatada durur; MatokiError satır:sütun ve Türkçe mesaj taşır.
 * import (D57): her dosya bir kez, bağımlılık önce katılır; satırları dosya başına bir bantta (k · 2^20) tutulur, hata
 * olunca bant dosya adına geri çevrilir. IR doğrulanır (ir_verify) — doğrulama hatası derleyici hatasıdır, kullanıcı hatası değil. */
#include "matoki/matoki.h"
#include "core/embed.h"
#include "core/mem.h"
#include "jit/jit.h"

#define LINE_BAND (1 << 20)

typedef struct ImportCtx {
    Arena*                a;
    const MatokiOptions*  opt;
    MatokiError*          err;
    const char*           names[MATOKI_MAX_IMPORTS];
    u32                   name_lens[MATOKI_MAX_IMPORTS];
    i32                   via[MATOKI_MAX_IMPORTS];     /* ana kaynaktaki kök import satırı */
    u8                    state[MATOKI_MAX_IMPORTS];   /* 1 işleniyor, 2 bitti */
    u32                   count;
    GlobalDecl*           globals;                     /* katılan dosyaların global'leri, bağımlılık önce */
    GlobalDecl**          gtail;
    FnDecl*               fns;
    FnDecl**              ftail;
} ImportCtx;

static b32 resolve_embed(void* user, const char* name, u32 name_len, const char** src, u32* len) {
    (void)user;
    char buf[64];
    if (name_len >= sizeof(buf)) return FALSE;
    mem_copy(buf, name, name_len);
    buf[name_len] = 0;
    const EmbedFile* ef = embed_find(buf);
    if (!ef) return FALSE;
    *src = (const char*)ef->data;
    *len = ef->len;
    return TRUE;
}

static b32 same_decl(const GlobalDecl* x, const GlobalDecl* y) {
    return x->kind == y->kind && x->kind != GD_CONST && type_eq(x->type, y->type) && x->attrs == y->attrs && x->slot == y->slot;
}

/* g'yi listeye ekler; başka dosyada aynı ad + aynı bildirim varsa (ör. iki kütüphanenin de istediği uniform) bir kez */
static void add_global(GlobalDecl** head, GlobalDecl*** tail, GlobalDecl* g) {
    for (GlobalDecl* h = *head; h; h = h->next) {
        if (h->name_len == g->name_len && mem_equal(h->name, g->name, g->name_len) && h->line / LINE_BAND != g->line / LINE_BAND &&
            same_decl(h, g)) {
            return;
        }
    }
    g->next = NULL;
    **tail = g;
    *tail = &g->next;
}

static void import_list(ImportCtx* c, const Import* im, i32 via);

static void import_one(ImportCtx* c, const Import* im, i32 via) {
    for (u32 i = 0; i < c->count; i++) {
        if (c->name_lens[i] == im->name_len && mem_equal(c->names[i], im->name, im->name_len)) {
            if (c->state[i] == 1) matoki_error(c->err, im->line, im->col, "döngüsel import:", im->name, im->name_len);
            return;
        }
    }
    if (c->count >= MATOKI_MAX_IMPORTS) {
        matoki_error(c->err, im->line, im->col, "en fazla 16 dosya import edilebilir:", im->name, im->name_len);
        return;
    }
    const char* src = NULL;
    u32 len = 0;
    b32 found = c->opt && c->opt->resolve ? c->opt->resolve(c->opt->resolve_user, im->name, im->name_len, &src, &len)
                                         : resolve_embed(NULL, im->name, im->name_len, &src, &len);
    if (!found) {
        matoki_error(c->err, im->line, im->col, "import edilen dosya bulunamadı:", im->name, im->name_len);
        return;
    }
    u32 k = c->count++;
    c->names[k] = im->name;
    c->name_lens[k] = im->name_len;
    c->via[k] = via;
    c->state[k] = 1;
    i32 band = (i32)(k + 1) * LINE_BAND;
    TokenList toks;
    if (!lex_source(c->a, src, len, &toks)) {
        matoki_error(c->err, band + toks.err_line, toks.err_col, toks.err_msg, NULL, 0);
        return;
    }
    for (u32 i = 0; i < toks.count; i++) toks.toks[i].line += band;
    Module m;
    if (!parse_module(c->a, &toks, &m, c->err)) return;
    import_list(c, m.imports, via);   /* derinlik dosya sayısıyla sınırlı (en fazla 16) */
    if (c->err->failed) return;
    for (GlobalDecl* g = m.globals; g;) {
        GlobalDecl* nx = g->next;
        add_global(&c->globals, &c->gtail, g);
        g = nx;
    }
    for (FnDecl* f = m.fns; f; f = f->next) {
        *c->ftail = f;
        c->ftail = &f->next;
    }
    *c->ftail = NULL;
    c->state[k] = 2;
}

static void import_list(ImportCtx* c, const Import* im, i32 via) {
    for (; im && !c->err->failed; im = im->next) import_one(c, im, via ? via : im->line);
}

/* hata bir bantta ise dosya adı + yerel satır */
static void error_locate(const ImportCtx* c, MatokiError* err) {
    if (err->line < LINE_BAND) return;
    u32 k = (u32)(err->line / LINE_BAND) - 1;
    err->line %= LINE_BAND;
    if (k >= c->count) return;
    u32 n = MIN(c->name_lens[k], (u32)sizeof(err->file) - 1);
    mem_copy(err->file, c->names[k], n);
    err->file[n] = 0;
    err->via_line = c->via[k];
}

/* ana modülün import'larını çözüp kütüphane global'lerini + fonksiyonlarını öne katar */
static b32 resolve_imports(Arena* a, const MatokiOptions* opt, Module* m, ImportCtx* c, MatokiError* err) {
    mem_zero(c, sizeof(*c));
    c->a = a;
    c->opt = opt;
    c->err = err;
    c->gtail = &c->globals;
    c->ftail = &c->fns;
    if (!m->imports) return TRUE;
    import_list(c, m->imports, 0);
    if (err->failed) return FALSE;
    for (GlobalDecl* g = m->globals; g;) {
        GlobalDecl* nx = g->next;
        add_global(&c->globals, &c->gtail, g);
        g = nx;
    }
    *c->ftail = m->fns;
    m->globals = c->globals;
    m->fns = c->fns;
    return TRUE;
}

b32 matoki_compile(Arena* a, const char* src, u32 len, const MatokiOptions* opt, MatokiProgram* p, MatokiError* err) {
    mem_zero(p, sizeof(*p));
    mem_zero(err, sizeof(*err));
    TokenList toks;
    if (!lex_source(a, src, len, &toks)) {
        matoki_error(err, toks.err_line, toks.err_col, toks.err_msg, NULL, 0);
        return FALSE;
    }
    if (!parse_module(a, &toks, &p->module, err)) return FALSE;
    ImportCtx ic;
    b32 ok = resolve_imports(a, opt, &p->module, &ic, err) &&
             sema_module(a, &p->module, opt ? opt->layout : NULL, opt ? &opt->overrides : NULL, &p->sema, err);
    if (!ok) {
        error_locate(&ic, err);
        return FALSE;
    }
    u32 cap = opt && opt->ir_cap ? opt->ir_cap : 65536;
    ir_init(&p->ir, a, cap, opt ? opt->no_fold : FALSE);
    p->stage = opt ? opt->stage : MATOKI_STAGE_FRAG;
    if (!lower_module(a, &p->module, &p->sema, p->stage, &p->ir, err)) {
        error_locate(&ic, err);
        return FALSE;
    }
    char buf[160];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    if (!ir_verify(&p->ir, &sb)) {
        buf[sb.len] = 0;
        matoki_error(err, 1, 1, "iç hata: IR doğrulanamadı:", buf, sb.len);
        return FALSE;
    }
    p->ir_total = p->ir.count - 1;
    p->ir_live = ir_mark_live(&p->ir);
    return TRUE;
}

b32 matoki_codegen(Arena* a, MatokiProgram* p, MatokiError* err) {
    if (!codegen_wasm(&p->ir, a, &p->wasm)) {
        matoki_error(err, 1, 1, "iç hata: wasm üretilemedi", NULL, 0);
        return FALSE;
    }
    return TRUE;
}

b32 matoki_jit(Arena* a, MatokiProgram* p, MatokiError* err) {
    if (!p->wasm.bytes && !matoki_codegen(a, p, err)) return FALSE;
    JitShader s;
    if (!jit_load(p->wasm.bytes, p->wasm.len, p->wasm.cache, p->wasm.cache_stride, TRUE, &s)) {
        matoki_error(err, 1, 1, "JIT yüklemesi başarısız (platform modülü reddetti)", NULL, 0);
        return FALSE;
    }
    p->fn = s.shade;
    p->prelude = s.prelude;
    p->fn_fast = s.fast;
    return TRUE;
}

static const GlobalDecl* find_global(const MatokiProgram* p, const char* name, u32 kind) {
    u32 n = 0;
    while (name[n]) n++;
    for (const GlobalDecl* g = p->module.globals; g; g = g->next) {
        if (g->kind == kind && g->name_len == n && mem_equal(g->name, name, n)) return g;
    }
    return NULL;
}

const GlobalDecl* matoki_find_uniform(const MatokiProgram* p, const char* name) { return find_global(p, name, GD_UNIFORM); }
const GlobalDecl* matoki_find_varying(const MatokiProgram* p, const char* name) { return find_global(p, name, GD_VARYING); }
const GlobalDecl* matoki_find_attribute(const MatokiProgram* p, const char* name) { return find_global(p, name, GD_ATTRIBUTE); }

void matoki_format_error(const MatokiError* e, StrBuf* out) {
    if (e->file[0]) {
        sb_cstr(out, e->file);
        sb_char(out, ':');
    }
    sb_i64(out, e->line);
    sb_char(out, ':');
    sb_i64(out, e->col);
    sb_cstr(out, ": ");
    sb_cstr(out, e->msg);
}
