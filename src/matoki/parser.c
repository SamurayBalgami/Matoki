/* matoki/parser.c — özyinelemeli iniş parser'ı (Token → AST).
 * İşleç önceliği (düşükten yükseğe): || && | ^ & (== !=) (< <= > >=) (<< >>) (+ -) (* / %)
 * tekli (- ! ~) sonek (çağrı, .swizzle). İlk hatada durur; hata konumu satır:sütun.
 * İç içe ifade / blok derinliği MATOKI_MAX_DEPTH ile sınırlı: aşırı derin girdi yığını taşırıp motoru düşürmez, hata olur. */
#include "matoki/ast.h"
#include "core/fmt.h"
#include "core/mem.h"

typedef struct Parser {
    Arena*       a;
    const Token* t;
    u32          pos;
    u32          count;
    MatokiError* err;
    u32          depth;     /* parse_unary / parse_block özyineleme derinliği */
} Parser;

#define MATOKI_MAX_DEPTH 200

void matoki_error(MatokiError* e, i32 line, i32 col, const char* msg, const char* detail, u32 detail_len) {
    if (e->failed) return;
    e->failed = TRUE;
    e->line = line;
    e->col = col;
    StrBuf sb;
    sb_init(&sb, e->msg, sizeof(e->msg) - 1);
    sb_cstr(&sb, msg);
    if (detail) {
        sb_cstr(&sb, " '");
        for (u32 i = 0; i < detail_len; i++) sb_char(&sb, detail[i]);
        sb_char(&sb, '\'');
    }
    e->msg[sb.len] = 0;
}

static const Token* peek(Parser* p) { return &p->t[p->pos]; }
static b32 at(Parser* p, TokKind k) { return p->t[p->pos].kind == k; }
static b32 failed(Parser* p) { return p->err->failed; }

static const Token* advance(Parser* p) {
    const Token* t = &p->t[p->pos];
    if (t->kind != T_EOF) p->pos++;
    return t;
}

static void error_at(Parser* p, const Token* t, const char* msg) {
    matoki_error(p->err, t->line, t->col, msg, t->kind == T_EOF ? NULL : t->p, t->kind == T_EOF ? 0 : t->len);
}

static b32 accept(Parser* p, TokKind k) {
    if (at(p, k)) {
        advance(p);
        return TRUE;
    }
    return FALSE;
}

static const Token* expect(Parser* p, TokKind k, const char* what) {
    if (failed(p)) return peek(p);
    if (!at(p, k)) {
        char buf[96];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf) - 1);
        sb_cstr(&sb, what);
        sb_cstr(&sb, " bekleniyor, bulunan:");
        buf[sb.len] = 0;
        error_at(p, peek(p), buf);
        return peek(p);
    }
    return advance(p);
}

static Node* node_new(Parser* p, NodeKind k, const Token* t) {
    Node* n = ARENA_NEW(p->a, Node);
    n->kind = k;
    n->line = t->line;
    n->col = t->col;
    return n;
}

static b32 tok_is(const Token* t, const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return t->kind == T_IDENT && t->len == n && mem_equal(t->p, s, n);
}

static b32 parse_type(Parser* p, Type* out) {
    const Token* t = peek(p);
    static const struct { const char* s; u8 base, n; } TYPES[] = {
        { "f32", TB_FLOAT, 1 }, { "i32", TB_INT, 1 }, { "bool", TB_BOOL, 1 },
        { "vec2", TB_FLOAT, 2 }, { "vec3", TB_FLOAT, 3 }, { "vec4", TB_FLOAT, 4 },
        { "texture2d", TB_TEX, 1 }, { "mat3", TB_MAT, 3 }, { "mat4", TB_MAT, 4 },
    };
    for (u32 i = 0; i < ARRAY_COUNT(TYPES); i++) {
        if (tok_is(t, TYPES[i].s)) {
            advance(p);
            *out = type_make(TYPES[i].base, TYPES[i].n);
            return TRUE;
        }
    }
    error_at(p, t, "tip bekleniyor (f32, i32, bool, vec2, vec3, vec4, mat3, mat4, texture2d), bulunan:");
    return FALSE;
}

/* ---- ifadeler ---- */

static Node* parse_expr(Parser* p);

static Node* parse_primary(Parser* p) {
    const Token* t = peek(p);
    switch (t->kind) {
    case T_INT: {
        advance(p);
        Node* n = node_new(p, NK_INT, t);
        n->ival = t->ival;   /* 2^31 yalnızca tekli eksiyle geçerli; parse_unary denetler */
        return n;
    }
    case T_FLOAT: {
        advance(p);
        Node* n = node_new(p, NK_FLOAT, t);
        n->fval = t->fval;
        return n;
    }
    case T_TRUE:
    case T_FALSE: {
        advance(p);
        Node* n = node_new(p, NK_BOOL, t);
        n->ival = t->kind == T_TRUE;
        return n;
    }
    case T_LPAREN: {
        advance(p);
        Node* e = parse_expr(p);
        expect(p, T_RPAREN, "')'");
        return e;
    }
    case T_IDENT: {
        advance(p);
        if (at(p, T_LPAREN)) {
            advance(p);
            Node* n = node_new(p, NK_CALL, t);
            n->name = t->p;
            n->name_len = t->len;
            Node** tail = &n->list;
            if (!at(p, T_RPAREN)) {
                do {
                    Node* arg = parse_expr(p);
                    if (failed(p)) return n;
                    *tail = arg;
                    tail = &arg->next;
                } while (accept(p, T_COMMA));
            }
            expect(p, T_RPAREN, "')'");
            return n;
        }
        Node* n = node_new(p, NK_NAME, t);
        n->name = t->p;
        n->name_len = t->len;
        return n;
    }
    default:
        error_at(p, t, "ifade bekleniyor, bulunan:");
        return node_new(p, NK_INT, t);
    }
}

static Node* parse_postfix(Parser* p) {
    Node* e = parse_primary(p);
    while (!failed(p) && at(p, T_DOT)) {
        const Token* dot = advance(p);
        const Token* f = expect(p, T_IDENT, "bileşen adı (xyzw / rgba)");
        if (failed(p)) break;
        Node* s = node_new(p, NK_SWIZZLE, dot);
        s->a = e;
        if (f->len > 4) {
            error_at(p, f, "swizzle en fazla 4 bileşen olabilir:");
            break;
        }
        for (u32 i = 0; i < f->len; i++) {
            char c = f->p[i];
            i32 idx = -1;
            switch (c) {
            case 'x': case 'r': idx = 0; break;
            case 'y': case 'g': idx = 1; break;
            case 'z': case 'b': idx = 2; break;
            case 'w': case 'a': idx = 3; break;
            default: break;
            }
            if (idx < 0) {
                error_at(p, f, "geçersiz swizzle:");
                return s;
            }
            s->swz[i] = (u8)idx;
        }
        s->swz_n = (u8)f->len;
        e = s;
    }
    return e;
}

static Node* parse_unary_inner(Parser* p);

/* derinlik sınırı: aşılırsa hata + yer tutucu sabit (çağıranlar NULL beklemez) */
static b32 depth_enter(Parser* p) {
    if (++p->depth <= MATOKI_MAX_DEPTH) return TRUE;
    error_at(p, peek(p), "iç içe ifade/blok çok derin (en çok 200):");
    return FALSE;
}

static Node* placeholder(Parser* p) {
    Node* n = node_new(p, NK_FLOAT, peek(p));
    n->fval = 0.0f;
    return n;
}

static Node* parse_unary(Parser* p) {
    Node* n = depth_enter(p) ? parse_unary_inner(p) : placeholder(p);
    p->depth--;
    return n;
}

static Node* parse_unary_inner(Parser* p) {
    const Token* t = peek(p);
    if (t->kind == T_MINUS || t->kind == T_BANG || t->kind == T_TILDE) {
        advance(p);
        Node* operand = parse_unary(p);
        /* -LİTERAL doğrudan sabit olsun (i32 en küçük değeri ve int→f32 zorlaması için) */
        if (t->kind == T_MINUS && operand->kind == NK_INT) {
            operand->ival = -operand->ival;
            operand->line = t->line;
            operand->col = t->col;
            return operand;
        }
        if (t->kind == T_MINUS && operand->kind == NK_FLOAT) {
            operand->fval = -operand->fval;
            operand->line = t->line;
            operand->col = t->col;
            return operand;
        }
        Node* n = node_new(p, NK_UNARY, t);
        n->op = t->kind;
        n->a = operand;
        return n;
    }
    Node* e = parse_postfix(p);
    if (e->kind == NK_INT && e->ival > 2147483647) error_at(p, t, "tamsayı i32 aralığının dışında:");
    return e;
}

/* öncelik tablosu: yüksek sayı = daha sıkı bağlanır */
static i32 binary_prec(TokKind k) {
    switch (k) {
    case T_OROR: return 1;
    case T_ANDAND: return 2;
    case T_PIPE: return 3;
    case T_CARET: return 4;
    case T_AMP: return 5;
    case T_EQEQ: case T_NE: return 6;
    case T_LT: case T_LE: case T_GT: case T_GE: return 7;
    case T_SHL: case T_SHR: return 8;
    case T_PLUS: case T_MINUS: return 9;
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    default: return 0;
    }
}

static Node* parse_binary(Parser* p, i32 min_prec) {
    Node* lhs = parse_unary(p);
    while (!failed(p)) {
        const Token* t = peek(p);
        i32 prec = binary_prec(t->kind);
        if (prec == 0 || prec < min_prec) break;
        advance(p);
        Node* rhs = parse_binary(p, prec + 1);
        Node* n = node_new(p, NK_BINARY, t);
        n->op = t->kind;
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static Node* parse_expr(Parser* p) { return parse_binary(p, 1); }

/* ---- deyimler ---- */

static Node* parse_block(Parser* p);

static Node* parse_stmt(Parser* p) {
    const Token* t = peek(p);
    switch (t->kind) {
    case T_LET:
    case T_VAR: {
        advance(p);
        const Token* name = expect(p, T_IDENT, "değişken adı");
        Node* n = node_new(p, t->kind == T_LET ? NK_LET : NK_VAR, name);
        n->name = name->p;
        n->name_len = name->len;
        if (accept(p, T_COLON)) {
            n->has_decl_type = parse_type(p, &n->decl_type);
        }
        if (accept(p, T_ASSIGN)) {
            n->a = parse_expr(p);
        } else if (t->kind == T_LET) {
            error_at(p, peek(p), "'let' bir ilk değer ister; bulunan:");
        } else if (!n->has_decl_type) {
            error_at(p, peek(p), "ilk değeri olmayan 'var' tip ister; bulunan:");
        }
        expect(p, T_SEMI, "';'");
        return n;
    }
    case T_IF: {
        advance(p);
        Node* n = node_new(p, NK_IF, t);
        n->a = parse_expr(p);
        n->b = parse_block(p);
        if (accept(p, T_ELSE)) {
            if (at(p, T_IF)) {
                Node* inner = parse_stmt(p);
                Node* blk = node_new(p, NK_BLOCK, t);
                blk->list = inner;
                n->c = blk;
            } else {
                n->c = parse_block(p);
            }
        }
        return n;
    }
    case T_FOR: {
        advance(p);
        const Token* name = expect(p, T_IDENT, "döngü değişkeni");
        Node* n = node_new(p, NK_FOR, name);
        n->name = name->p;
        n->name_len = name->len;
        expect(p, T_IN, "'in'");
        Node* lo = parse_unary(p);
        expect(p, T_DOTDOT, "'..'");
        Node* hi = parse_unary(p);
        if (failed(p)) return n;
        if (lo->kind != NK_INT || hi->kind != NK_INT) {
            matoki_error(p->err, lo->line, lo->col, "for sınırları tamsayı sabiti olmalı", NULL, 0);
            return n;
        }
        n->lo = lo->ival;
        n->hi = hi->ival;
        n->b = parse_block(p);
        return n;
    }
    case T_RETURN: {
        advance(p);
        Node* n = node_new(p, NK_RETURN, t);
        if (!at(p, T_SEMI)) n->a = parse_expr(p);
        expect(p, T_SEMI, "';'");
        return n;
    }
    case T_DISCARD: {
        advance(p);
        Node* n = node_new(p, NK_DISCARD, t);
        expect(p, T_SEMI, "';'");
        return n;
    }
    case T_LBRACE:
        return parse_block(p);
    default: {
        Node* e = parse_expr(p);
        TokKind k = peek(p)->kind;
        if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ || k == T_SLASHEQ) {
            const Token* op = advance(p);
            Node* n = node_new(p, NK_ASSIGN, op);
            n->op = op->kind;
            n->a = e;
            n->b = parse_expr(p);
            expect(p, T_SEMI, "';'");
            return n;
        }
        Node* n = node_new(p, NK_EXPR, t);
        n->a = e;
        expect(p, T_SEMI, "';'");
        return n;
    }
    }
}

static Node* parse_block(Parser* p) {
    if (!depth_enter(p)) {
        p->depth--;
        return node_new(p, NK_BLOCK, peek(p));
    }
    const Token* t = expect(p, T_LBRACE, "'{'");
    Node* blk = node_new(p, NK_BLOCK, t);
    Node** tail = &blk->list;
    while (!failed(p) && !at(p, T_RBRACE) && !at(p, T_EOF)) {
        Node* s = parse_stmt(p);
        *tail = s;
        tail = &s->next;
    }
    expect(p, T_RBRACE, "'}'");
    p->depth--;
    return blk;
}

/* ---- üst düzey ---- */

static void parse_attrs(Parser* p, GlobalDecl* g) {
    while (!failed(p) && accept(p, T_AT)) {
        const Token* a = expect(p, T_IDENT, "öznitelik adı");
        if (failed(p)) return;
        if (tok_is(a, "clamp")) g->attrs |= ATTR_CLAMP;
        else if (tok_is(a, "mipmap")) g->attrs |= ATTR_MIPMAP;
        else if (tok_is(a, "bilinear")) g->attrs |= ATTR_BILINEAR;
        else if (tok_is(a, "slot")) {
            expect(p, T_LPAREN, "'('");
            const Token* n = expect(p, T_INT, "yuva numarası");
            expect(p, T_RPAREN, "')'");
            g->slot = (i32)n->ival;
        } else {
            error_at(p, a, "bilinmeyen öznitelik:");
        }
    }
}

b32 parse_module(Arena* a, const TokenList* toks, Module* out, MatokiError* err) {
    Parser p;
    p.a = a;
    p.t = toks->toks;
    p.pos = 0;
    p.count = toks->count;
    p.err = err;
    p.depth = 0;
    mem_zero(out, sizeof(*out));
    GlobalDecl** gtail = &out->globals;
    FnDecl** ftail = &out->fns;
    while (!failed(&p) && !at(&p, T_EOF)) {
        const Token* t = peek(&p);
        if (t->kind == T_UNIFORM || t->kind == T_VARYING || t->kind == T_ATTRIBUTE) {
            advance(&p);
            const Token* name = expect(&p, T_IDENT, "isim");
            GlobalDecl* g = ARENA_NEW(a, GlobalDecl);
            g->kind = t->kind == T_UNIFORM ? GD_UNIFORM : t->kind == T_VARYING ? GD_VARYING : GD_ATTRIBUTE;
            g->name = name->p;
            g->name_len = name->len;
            g->line = name->line;
            g->col = name->col;
            g->slot = -1;
            expect(&p, T_COLON, "':'");
            if (!failed(&p)) parse_type(&p, &g->type);
            parse_attrs(&p, g);
            expect(&p, T_SEMI, "';'");
            *gtail = g;
            gtail = &g->next;
        } else if (t->kind == T_FN) {
            advance(&p);
            const Token* name = expect(&p, T_IDENT, "fonksiyon adı");
            FnDecl* f = ARENA_NEW(a, FnDecl);
            f->name = name->p;
            f->name_len = name->len;
            f->line = name->line;
            f->col = name->col;
            f->ret = type_make(TB_VOID, 0);
            expect(&p, T_LPAREN, "'('");
            Param** ptail = &f->params;
            if (!at(&p, T_RPAREN)) {
                do {
                    const Token* pn = expect(&p, T_IDENT, "parametre adı");
                    if (failed(&p)) break;
                    Param* prm = ARENA_NEW(a, Param);
                    prm->name = pn->p;
                    prm->name_len = pn->len;
                    prm->line = pn->line;
                    prm->col = pn->col;
                    expect(&p, T_COLON, "':'");
                    if (!failed(&p)) parse_type(&p, &prm->type);
                    *ptail = prm;
                    ptail = &prm->next;
                    f->param_count++;
                } while (!failed(&p) && accept(&p, T_COMMA));
            }
            expect(&p, T_RPAREN, "')'");
            if (accept(&p, T_ARROW) && !failed(&p)) parse_type(&p, &f->ret);
            if (!failed(&p)) f->body = parse_block(&p);
            *ftail = f;
            ftail = &f->next;
        } else {
            error_at(&p, t, "'uniform', 'varying' veya 'fn' bekleniyor, bulunan:");
        }
    }
    return !failed(&p);
}
