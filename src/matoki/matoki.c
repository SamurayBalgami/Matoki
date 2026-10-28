/* matoki/matoki.c — derleme hattını birleştirir.
 * Her aşama ilk hatada durur; MatokiError satır:sütun ve Türkçe mesaj taşır.
 * IR doğrulanır (ir_verify) — doğrulama hatası derleyici hatasıdır, kullanıcı hatası değil. */
#include "matoki/matoki.h"
#include "core/mem.h"
#include "jit/jit.h"

b32 matoki_compile(Arena* a, const char* src, u32 len, const MatokiOptions* opt, MatokiProgram* p, MatokiError* err) {
    mem_zero(p, sizeof(*p));
    mem_zero(err, sizeof(*err));
    TokenList toks;
    if (!lex_source(a, src, len, &toks)) {
        matoki_error(err, toks.err_line, toks.err_col, toks.err_msg, NULL, 0);
        return FALSE;
    }
    if (!parse_module(a, &toks, &p->module, err)) return FALSE;
    if (!sema_module(a, &p->module, &p->sema, err)) return FALSE;
    u32 cap = opt && opt->ir_cap ? opt->ir_cap : 65536;
    ir_init(&p->ir, a, cap, opt ? opt->no_fold : FALSE);
    if (!lower_module(a, &p->module, &p->sema, &p->ir, err)) return FALSE;
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
    if (!jit_load(p->wasm.bytes, p->wasm.len, TRUE, &s)) {
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

void matoki_format_error(const MatokiError* e, StrBuf* out) {
    sb_i64(out, e->line);
    sb_char(out, ':');
    sb_i64(out, e->col);
    sb_cstr(out, ": ");
    sb_cstr(out, e->msg);
}
