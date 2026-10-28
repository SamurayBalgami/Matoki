/* matoki/sema.c — isim çözümleme, tip denetimi ve yerleşim.
 * Tamsayı literalleri f32 beklenen yerde f32'ye dönüşür (ör. `x * 2`); başka örtük dönüşüm yok.
 * Vektör işlemleri bileşen bazlı; vektör ⊕ skaler yayılır. Fonksiyonlar satır içine alınacağı
 * için özyineleme yasak (DFS ile tespit). `return` yalnızca fonksiyonun son deyimi olabilir. */
#include "matoki/sema.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "raster/shade_abi.h"

typedef struct Sema {
    Arena*       a;
    Module*      m;
    MatokiError* err;
    Sym*         globals;
    Sym*         scope;
} Sema;

static const struct { const char* name; Builtin bi; } BUILTINS[] = {
    { "vec2", BI_VEC2 }, { "vec3", BI_VEC3 }, { "vec4", BI_VEC4 }, { "f32", BI_F32 }, { "i32", BI_I32 },
    { "abs", BI_ABS }, { "min", BI_MIN }, { "max", BI_MAX }, { "clamp", BI_CLAMP },
    { "floor", BI_FLOOR }, { "ceil", BI_CEIL }, { "fract", BI_FRACT }, { "trunc", BI_TRUNC }, { "round", BI_ROUND },
    { "sqrt", BI_SQRT }, { "sin", BI_SIN }, { "cos", BI_COS },
    { "mix", BI_MIX }, { "step", BI_STEP }, { "smoothstep", BI_SMOOTHSTEP }, { "mod", BI_MOD },
    { "dot", BI_DOT }, { "cross", BI_CROSS }, { "length", BI_LENGTH }, { "distance", BI_DISTANCE },
    { "normalize", BI_NORMALIZE }, { "reflect", BI_REFLECT },
    { "select", BI_SELECT },
    { "sample", BI_SAMPLE }, { "texel_fetch", BI_TEXEL_FETCH }, { "tex_size", BI_TEX_SIZE },
    { "bitcast_f32", BI_BITCAST_F32 }, { "bitcast_i32", BI_BITCAST_I32 },
    { "dFdx", BI_DFDX }, { "dFdy", BI_DFDY }, { "fwidth", BI_FWIDTH },
    { "frag_coord", BI_FRAG_COORD },
};

static b32 name_eq(const char* a, u32 al, const char* b, u32 bl) { return al == bl && mem_equal(a, b, al); }

static b32 name_is(const char* p, u32 len, const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return name_eq(p, len, s, n);
}

static const char* type_name(Type t) {
    switch (t.base) {
    case TB_VOID: return "void";
    case TB_BOOL: return "bool";
    case TB_INT: return "i32";
    case TB_TEX: return "texture2d";
    case TB_FLOAT:
        switch (t.n) {
        case 1: return "f32";
        case 2: return "vec2";
        case 3: return "vec3";
        default: return "vec4";
        }
    default: return "<hata>";
    }
}

static b32 failed(Sema* s) { return s->err->failed; }

static Type err_at(Sema* s, const Node* n, const char* msg) {
    matoki_error(s->err, n->line, n->col, msg, NULL, 0);
    return type_make(TB_ERROR, 0);
}

static Type err_name(Sema* s, const Node* n, const char* msg) {
    matoki_error(s->err, n->line, n->col, msg, n->name, n->name_len);
    return type_make(TB_ERROR, 0);
}

static Type err_types(Sema* s, const Node* n, const char* msg, Type a, Type b) {
    char buf[160];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf) - 1);
    sb_cstr(&sb, msg);
    sb_cstr(&sb, " (");
    sb_cstr(&sb, type_name(a));
    if (b.base != TB_ERROR) {
        sb_cstr(&sb, ", ");
        sb_cstr(&sb, type_name(b));
    }
    sb_char(&sb, ')');
    buf[sb.len] = 0;
    return err_at(s, n, buf);
}

static Sym* lookup(Sema* s, const char* name, u32 len) {
    for (Sym* y = s->scope; y; y = y->next) {
        if (name_eq(y->name, y->name_len, name, len)) return y;
    }
    for (Sym* y = s->globals; y; y = y->next) {
        if (name_eq(y->name, y->name_len, name, len)) return y;
    }
    return NULL;
}

static FnDecl* find_fn(Sema* s, const char* name, u32 len) {
    for (FnDecl* f = s->m->fns; f; f = f->next) {
        if (name_eq(f->name, f->name_len, name, len)) return f;
    }
    return NULL;
}

static Builtin find_builtin(const char* name, u32 len) {
    for (u32 i = 0; i < ARRAY_COUNT(BUILTINS); i++) {
        if (name_is(name, len, BUILTINS[i].name)) return BUILTINS[i].bi;
    }
    return BI_NONE;
}

static Sym* sym_new(Sema* s, const char* name, u32 len, u32 kind, Type t, b32 mut) {
    Sym* y = ARENA_NEW(s->a, Sym);
    y->name = name;
    y->name_len = len;
    y->kind = kind;
    y->type = t;
    y->mutable_ = mut;
    return y;
}

static inline b32 is_f(Type t) { return t.base == TB_FLOAT; }
static inline b32 is_fs(Type t) { return t.base == TB_FLOAT && t.n == 1; }
static inline b32 is_is(Type t) { return t.base == TB_INT && t.n == 1; }
static inline b32 is_bs(Type t) { return t.base == TB_BOOL && t.n == 1; }

/* Tamsayı literalini f32'ye çevir (yalnızca literal; ifadeler örtük dönüşmez). */
static void coerce_float(Node* e) {
    if (e->kind == NK_INT) {
        e->kind = NK_FLOAT;
        e->fval = (f32)e->ival;
        e->type = type_make(TB_FLOAT, 1);
    }
}

static Type sema_expr(Sema* s, Node* e);
static void sema_fn(Sema* s, FnDecl* f, const Node* site);

/* + - * / için: aynı n veya biri skaler (yayılım) */
static Type arith_type(Sema* s, Node* n, Node* a, Node* b) {
    Type ta = a->type, tb = b->type;
    if (is_f(ta) || is_f(tb)) {
        coerce_float(a);
        coerce_float(b);
        ta = a->type;
        tb = b->type;
        if (!is_f(ta) || !is_f(tb)) return err_types(s, n, "aritmetikte tip uyuşmazlığı", ta, tb);
        if (ta.n == tb.n || tb.n == 1) return ta;
        if (ta.n == 1) return tb;
        return err_types(s, n, "vektör boyutları uyuşmuyor", ta, tb);
    }
    if (is_is(ta) && is_is(tb)) {
        if (n->op == T_SLASH) return err_at(s, n, "tamsayı bölme desteklenmiyor (f32 kullanın)");
        return ta;
    }
    return err_types(s, n, "aritmetik işlenenler sayısal olmalı", ta, tb);
}

static Type sema_binary(Sema* s, Node* e) {
    Type ta = sema_expr(s, e->a);
    Type tb = sema_expr(s, e->b);
    if (failed(s)) return ta;
    switch (e->op) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH:
        return arith_type(s, e, e->a, e->b);
    case T_PERCENT:
        return err_at(s, e, "'%' yok; f32 için mod(x, y) kullanın");
    case T_LT: case T_LE: case T_GT: case T_GE: case T_EQEQ: case T_NE:
        if (is_fs(ta) || is_fs(tb)) {
            coerce_float(e->a);
            coerce_float(e->b);
            ta = e->a->type;
            tb = e->b->type;
        }
        if (!type_eq(ta, tb) || ta.n != 1) return err_types(s, e, "karşılaştırma iki aynı tip skaler ister", ta, tb);
        if (ta.base == TB_BOOL && e->op != T_EQEQ && e->op != T_NE) return err_at(s, e, "bool yalnızca == / != ile karşılaştırılır");
        if (ta.base != TB_FLOAT && ta.base != TB_INT && ta.base != TB_BOOL) return err_types(s, e, "karşılaştırılamaz", ta, tb);
        return type_make(TB_BOOL, 1);
    case T_ANDAND: case T_OROR:
        if (!is_bs(ta) || !is_bs(tb)) return err_types(s, e, "'&&' / '||' bool ister", ta, tb);
        return ta;
    case T_AMP: case T_PIPE: case T_CARET: case T_SHL: case T_SHR:
        if (!is_is(ta) || !is_is(tb)) return err_types(s, e, "bit işlemleri i32 ister", ta, tb);
        return ta;
    default:
        return err_at(s, e, "bilinmeyen ikili işleç");
    }
}

static b32 expect_args(Sema* s, Node* call, u32 got, u32 want) {
    if (got != want) {
        char buf[96];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf) - 1);
        sb_u64(&sb, want);
        sb_cstr(&sb, " argüman bekleniyor, verilen ");
        sb_u64(&sb, got);
        sb_cstr(&sb, ":");
        buf[sb.len] = 0;
        err_name(s, call, buf);
        return FALSE;
    }
    return TRUE;
}

/* argümanları f32 tipine zorla; hepsi float genType olmalı */
static b32 all_float(Sema* s, Node* call, Node** args, u32 n) {
    for (u32 i = 0; i < n; i++) {
        coerce_float(args[i]);
        if (!is_f(args[i]->type)) {
            err_types(s, args[i], "f32/vec argüman bekleniyor", args[i]->type, type_make(TB_ERROR, 0));
            UNUSED(call);
            return FALSE;
        }
    }
    return TRUE;
}

static b32 is_tex_arg(Sema* s, Node* arg) {
    if (arg->kind != NK_NAME || arg->type.base != TB_TEX) {
        err_at(s, arg, "ilk argüman bir texture2d uniform'u olmalı");
        return FALSE;
    }
    return TRUE;
}

static Type sema_builtin(Sema* s, Node* call, Builtin bi, Node** args, u32 n) {
    const Type F1 = type_make(TB_FLOAT, 1);
    switch (bi) {
    case BI_VEC2: case BI_VEC3: case BI_VEC4: {
        u32 want = (u32)(bi - BI_VEC2) + 2;
        if (!all_float(s, call, args, n)) return F1;
        u32 total = 0;
        for (u32 i = 0; i < n; i++) total += args[i]->type.n;
        if (!(total == want || (n == 1 && total == 1))) {
            return err_at(s, call, "vektör kurucusunun bileşen sayısı uyuşmuyor");
        }
        return type_make(TB_FLOAT, (u8)want);
    }
    case BI_F32:
        if (!expect_args(s, call, n, 1)) return F1;
        if (!is_fs(args[0]->type) && !is_is(args[0]->type)) return err_types(s, call, "f32() skaler sayı ister", args[0]->type, type_make(TB_ERROR, 0));
        return F1;
    case BI_I32:
        if (!expect_args(s, call, n, 1)) return F1;
        if (!is_fs(args[0]->type) && !is_is(args[0]->type)) return err_types(s, call, "i32() skaler sayı ister", args[0]->type, type_make(TB_ERROR, 0));
        return type_make(TB_INT, 1);
    case BI_ABS:
        if (!expect_args(s, call, n, 1)) return F1;
        if (is_is(args[0]->type)) return args[0]->type;
        if (!all_float(s, call, args, 1)) return F1;
        return args[0]->type;
    case BI_MIN: case BI_MAX:
        if (!expect_args(s, call, n, 2)) return F1;
        if (is_is(args[0]->type) && is_is(args[1]->type)) return args[0]->type;
        if (!all_float(s, call, args, 2)) return F1;
        if (args[0]->type.n != args[1]->type.n && args[1]->type.n != 1) return err_types(s, call, "min/max boyut uyuşmazlığı", args[0]->type, args[1]->type);
        return args[0]->type;
    case BI_CLAMP:
        if (!expect_args(s, call, n, 3)) return F1;
        if (is_is(args[0]->type) && is_is(args[1]->type) && is_is(args[2]->type)) return args[0]->type;
        if (!all_float(s, call, args, 3)) return F1;
        for (u32 i = 1; i < 3; i++) {
            if (args[i]->type.n != args[0]->type.n && args[i]->type.n != 1) return err_types(s, call, "clamp boyut uyuşmazlığı", args[0]->type, args[i]->type);
        }
        return args[0]->type;
    case BI_FLOOR: case BI_CEIL: case BI_FRACT: case BI_TRUNC: case BI_ROUND: case BI_SQRT: case BI_SIN: case BI_COS:
    case BI_NORMALIZE: case BI_DFDX: case BI_DFDY: case BI_FWIDTH:
        if (!expect_args(s, call, n, 1) || !all_float(s, call, args, 1)) return F1;
        return args[0]->type;
    case BI_LENGTH:
        if (!expect_args(s, call, n, 1) || !all_float(s, call, args, 1)) return F1;
        return F1;
    case BI_MIX:
        if (!expect_args(s, call, n, 3) || !all_float(s, call, args, 3)) return F1;
        if (!type_eq(args[0]->type, args[1]->type)) return err_types(s, call, "mix(a, b, t): a ve b aynı tip olmalı", args[0]->type, args[1]->type);
        if (args[2]->type.n != 1 && args[2]->type.n != args[0]->type.n) return err_types(s, call, "mix: t boyutu", args[0]->type, args[2]->type);
        return args[0]->type;
    case BI_STEP:
        if (!expect_args(s, call, n, 2) || !all_float(s, call, args, 2)) return F1;
        if (args[0]->type.n != 1 && args[0]->type.n != args[1]->type.n) return err_types(s, call, "step boyut uyuşmazlığı", args[0]->type, args[1]->type);
        return args[1]->type;
    case BI_SMOOTHSTEP:
        if (!expect_args(s, call, n, 3) || !all_float(s, call, args, 3)) return F1;
        for (u32 i = 0; i < 2; i++) {
            if (args[i]->type.n != 1 && args[i]->type.n != args[2]->type.n) return err_types(s, call, "smoothstep boyut uyuşmazlığı", args[i]->type, args[2]->type);
        }
        return args[2]->type;
    case BI_MOD:
        if (!expect_args(s, call, n, 2) || !all_float(s, call, args, 2)) return F1;
        if (args[1]->type.n != 1 && args[1]->type.n != args[0]->type.n) return err_types(s, call, "mod boyut uyuşmazlığı", args[0]->type, args[1]->type);
        return args[0]->type;
    case BI_DOT: case BI_DISTANCE:
        if (!expect_args(s, call, n, 2) || !all_float(s, call, args, 2)) return F1;
        if (!type_eq(args[0]->type, args[1]->type)) return err_types(s, call, "aynı tip iki vektör bekleniyor", args[0]->type, args[1]->type);
        return F1;
    case BI_CROSS:
        if (!expect_args(s, call, n, 2) || !all_float(s, call, args, 2)) return F1;
        if (args[0]->type.n != 3 || args[1]->type.n != 3) return err_types(s, call, "cross iki vec3 ister", args[0]->type, args[1]->type);
        return args[0]->type;
    case BI_REFLECT:
        if (!expect_args(s, call, n, 2) || !all_float(s, call, args, 2)) return F1;
        if (!type_eq(args[0]->type, args[1]->type)) return err_types(s, call, "reflect aynı tip ister", args[0]->type, args[1]->type);
        return args[0]->type;
    case BI_SELECT: {
        if (!expect_args(s, call, n, 3)) return F1;
        if (!is_bs(args[0]->type)) return err_types(s, call, "select ilk argümanı bool olmalı", args[0]->type, type_make(TB_ERROR, 0));
        if (is_f(args[1]->type) || is_f(args[2]->type)) {
            coerce_float(args[1]);
            coerce_float(args[2]);
        }
        Type t = args[1]->type;
        if (!type_eq(t, args[2]->type) || t.base == TB_TEX) return err_types(s, call, "select dalları aynı tip olmalı", t, args[2]->type);
        return t;
    }
    case BI_SAMPLE:
        if (!expect_args(s, call, n, 2) || !is_tex_arg(s, args[0])) return F1;
        coerce_float(args[1]);
        if (!type_eq(args[1]->type, type_make(TB_FLOAT, 2))) return err_types(s, call, "sample(tex, uv): uv vec2 olmalı", args[1]->type, type_make(TB_ERROR, 0));
        if (args[0]->sym->g->attrs & (ATTR_MIPMAP | ATTR_BILINEAR)) {
            return err_at(s, call, "@mipmap / @bilinear örnekleme henüz yok (M8 Faz 2)");
        }
        return type_make(TB_FLOAT, 4);
    case BI_TEXEL_FETCH:
        if (!expect_args(s, call, n, 3) || !is_tex_arg(s, args[0])) return F1;
        if (!is_is(args[1]->type) || !is_is(args[2]->type)) return err_at(s, call, "texel_fetch(tex, x: i32, y: i32)");
        return type_make(TB_INT, 1);
    case BI_TEX_SIZE:
        if (!expect_args(s, call, n, 1) || !is_tex_arg(s, args[0])) return F1;
        return type_make(TB_FLOAT, 2);
    case BI_BITCAST_F32:
        if (!expect_args(s, call, n, 1)) return F1;
        if (!is_is(args[0]->type)) return err_at(s, call, "bitcast_f32 i32 ister");
        return F1;
    case BI_BITCAST_I32:
        if (!expect_args(s, call, n, 1)) return F1;
        coerce_float(args[0]);
        if (!is_fs(args[0]->type)) return err_at(s, call, "bitcast_i32 f32 ister");
        return type_make(TB_INT, 1);
    case BI_FRAG_COORD:
        if (!expect_args(s, call, n, 0)) return F1;
        return type_make(TB_FLOAT, 3);
    default:
        return err_at(s, call, "bilinmeyen yerleşik");
    }
}

static Type sema_call(Sema* s, Node* e) {
    Node* args[16];
    u32 n = 0;
    for (Node* a = e->list; a; a = a->next) {
        if (n == ARRAY_COUNT(args)) return err_at(s, e, "en fazla 16 argüman");
        args[n++] = a;
        sema_expr(s, a);
        if (failed(s)) return a->type;
    }
    Builtin bi = find_builtin(e->name, e->name_len);
    if (bi != BI_NONE) {
        e->builtin = (i32)bi;
        return sema_builtin(s, e, bi, args, n);
    }
    FnDecl* f = find_fn(s, e->name, e->name_len);
    if (!f) return err_name(s, e, "tanımsız fonksiyon");
    if (!expect_args(s, e, n, f->param_count)) return f->ret;
    sema_fn(s, f, e);
    if (failed(s)) return f->ret;
    u32 i = 0;
    for (Param* p = f->params; p; p = p->next, i++) {
        if (is_f(p->type)) coerce_float(args[i]);
        if (!type_eq(args[i]->type, p->type)) return err_types(s, args[i], "argüman tipi uyuşmuyor", args[i]->type, p->type);
    }
    e->fn = f;
    return f->ret;
}

static Type sema_expr(Sema* s, Node* e) {
    Type t = type_make(TB_ERROR, 0);
    if (failed(s)) return t;
    switch (e->kind) {
    case NK_INT:
        if (e->ival > 2147483647 || e->ival < -2147483647 - 1) t = err_at(s, e, "tamsayı i32 aralığının dışında");
        else t = type_make(TB_INT, 1);
        break;
    case NK_FLOAT: t = type_make(TB_FLOAT, 1); break;
    case NK_BOOL: t = type_make(TB_BOOL, 1); break;
    case NK_NAME: {
        Sym* y = lookup(s, e->name, e->name_len);
        if (!y) {
            t = err_name(s, e, "tanımsız isim");
            break;
        }
        e->sym = y;
        t = y->type;
        break;
    }
    case NK_UNARY: {
        Type ta = sema_expr(s, e->a);
        if (failed(s)) break;
        if (e->op == T_MINUS) {
            if (!is_f(ta) && !is_is(ta)) t = err_types(s, e, "tekli '-' sayı ister", ta, type_make(TB_ERROR, 0));
            else t = ta;
        } else if (e->op == T_BANG) {
            t = is_bs(ta) ? ta : err_types(s, e, "'!' bool ister", ta, type_make(TB_ERROR, 0));
        } else {
            t = is_is(ta) ? ta : err_types(s, e, "'~' i32 ister", ta, type_make(TB_ERROR, 0));
        }
        break;
    }
    case NK_BINARY: t = sema_binary(s, e); break;
    case NK_SWIZZLE: {
        Type ta = sema_expr(s, e->a);
        if (failed(s)) break;
        if (!is_f(ta) || ta.n < 2) {
            t = err_types(s, e, "swizzle yalnızca vektörlerde", ta, type_make(TB_ERROR, 0));
            break;
        }
        for (u32 i = 0; i < e->swz_n; i++) {
            if (e->swz[i] >= ta.n) {
                t = err_types(s, e, "swizzle bileşeni vektörün dışında", ta, type_make(TB_ERROR, 0));
                return t;
            }
        }
        t = type_make(TB_FLOAT, e->swz_n);
        break;
    }
    case NK_CALL: t = sema_call(s, e); break;
    default: t = err_at(s, e, "ifade bekleniyor"); break;
    }
    e->type = t;
    return t;
}

/* ---- deyimler ---- */

static void sema_block(Sema* s, Node* blk);

static void sema_stmt(Sema* s, Node* st) {
    if (failed(s)) return;
    switch (st->kind) {
    case NK_LET:
    case NK_VAR: {
        Type t;
        if (st->a) {
            sema_expr(s, st->a);
            if (failed(s)) return;
            if (st->has_decl_type) {
                if (is_f(st->decl_type)) coerce_float(st->a);
                if (!type_eq(st->a->type, st->decl_type)) {
                    err_types(s, st->a, "ilk değer tipi bildirilen tiple uyuşmuyor", st->a->type, st->decl_type);
                    return;
                }
            }
            t = st->a->type;
        } else {
            t = st->decl_type;
        }
        if (t.base != TB_FLOAT && t.base != TB_INT && t.base != TB_BOOL) {
            err_types(s, st, "yerel değişken bu tipte olamaz", t, type_make(TB_ERROR, 0));
            return;
        }
        Sym* y = sym_new(s, st->name, st->name_len, SYM_LOCAL, t, st->kind == NK_VAR);
        y->next = s->scope;
        s->scope = y;
        st->sym = y;
        return;
    }
    case NK_ASSIGN: {
        Node* target = st->a;
        Node* base = target->kind == NK_SWIZZLE ? target->a : target;
        if (base->kind != NK_NAME) {
            err_at(s, target, "atama hedefi bir değişken (veya bileşeni) olmalı");
            return;
        }
        sema_expr(s, target);
        if (failed(s)) return;
        if (base->sym->kind != SYM_LOCAL || !base->sym->mutable_) {
            err_name(s, base, "değiştirilemez (let, parametre, uniform veya varying):");
            return;
        }
        if (target->kind == NK_SWIZZLE) {
            for (u32 i = 0; i < target->swz_n; i++) {
                for (u32 j = i + 1; j < target->swz_n; j++) {
                    if (target->swz[i] == target->swz[j]) {
                        err_at(s, target, "atamada swizzle bileşeni tekrar edemez");
                        return;
                    }
                }
            }
        }
        sema_expr(s, st->b);
        if (failed(s)) return;
        if (st->op != T_ASSIGN) {
            Node* bin = ARENA_NEW(s->a, Node);
            *bin = *st;
            bin->kind = NK_BINARY;
            bin->op = st->op == T_PLUSEQ ? T_PLUS : st->op == T_MINUSEQ ? T_MINUS : st->op == T_STAREQ ? T_STAR : T_SLASH;
            bin->a = target;
            bin->b = st->b;
            bin->next = NULL;
            Type rt = arith_type(s, bin, target, st->b);
            bin->type = rt;
            if (failed(s)) return;
            if (!type_eq(rt, target->type)) {
                err_types(s, st, "bileşik atama sonucu hedef tipiyle uyuşmuyor", rt, target->type);
                return;
            }
            st->c = bin;
        } else {
            if (is_f(target->type)) coerce_float(st->b);
            if (!type_eq(st->b->type, target->type)) err_types(s, st, "atama tipi uyuşmuyor", st->b->type, target->type);
        }
        return;
    }
    case NK_IF:
        sema_expr(s, st->a);
        if (failed(s)) return;
        if (!is_bs(st->a->type)) {
            err_types(s, st->a, "if koşulu bool olmalı", st->a->type, type_make(TB_ERROR, 0));
            return;
        }
        sema_block(s, st->b);
        if (st->c) sema_block(s, st->c);
        return;
    case NK_FOR: {
        if (st->hi - st->lo > 1024) {
            err_at(s, st, "döngü en fazla 1024 tur (tamamen açılır)");
            return;
        }
        Sym* saved = s->scope;
        Sym* y = sym_new(s, st->name, st->name_len, SYM_LOCAL, type_make(TB_INT, 1), FALSE);
        y->next = s->scope;
        s->scope = y;
        st->sym = y;
        sema_block(s, st->b);
        s->scope = saved;
        return;
    }
    case NK_RETURN:
        err_at(s, st, "return yalnızca fonksiyonun son deyimi olabilir");
        return;
    case NK_EXPR:
        sema_expr(s, st->a);
        return;
    case NK_BLOCK:
        sema_block(s, st);
        return;
    default:
        err_at(s, st, "deyim bekleniyor");
        return;
    }
}

static void sema_block(Sema* s, Node* blk) {
    Sym* saved = s->scope;
    for (Node* st = blk->list; st && !failed(s); st = st->next) sema_stmt(s, st);
    s->scope = saved;
}

static void sema_fn(Sema* s, FnDecl* f, const Node* site) {
    if (f->visit == 2) return;
    if (f->visit == 1) {
        if (site) matoki_error(s->err, site->line, site->col, "özyineleme desteklenmiyor (fonksiyonlar satır içine alınır):", f->name, f->name_len);
        return;
    }
    f->visit = 1;
    Sym* saved = s->scope;
    s->scope = NULL;
    for (Param* p = f->params; p; p = p->next) {
        if (p->type.base != TB_FLOAT && p->type.base != TB_INT && p->type.base != TB_BOOL) {
            matoki_error(s->err, p->line, p->col, "parametre bu tipte olamaz:", p->name, p->name_len);
            break;
        }
        Sym* y = sym_new(s, p->name, p->name_len, SYM_LOCAL, p->type, FALSE);
        y->next = s->scope;
        s->scope = y;
        p->sym = y;
    }
    if (f->ret.base != TB_FLOAT && f->ret.base != TB_INT && f->ret.base != TB_BOOL && !failed(s)) {
        matoki_error(s->err, f->line, f->col, "fonksiyon bir değer döndürmeli (f32, i32, bool, vecN):", f->name, f->name_len);
    }
    Node* last = NULL;
    for (Node* st = f->body->list; st && !failed(s); st = st->next) {
        if (!st->next && st->kind == NK_RETURN) {
            last = st;
            break;
        }
        sema_stmt(s, st);
    }
    if (!failed(s)) {
        if (!last || !last->a) {
            matoki_error(s->err, f->line, f->col, "fonksiyon 'return <ifade>;' ile bitmeli:", f->name, f->name_len);
        } else {
            sema_expr(s, last->a);
            if (!failed(s)) {
                if (is_f(f->ret)) coerce_float(last->a);
                if (!type_eq(last->a->type, f->ret)) err_types(s, last->a, "dönüş tipi uyuşmuyor", last->a->type, f->ret);
            }
        }
    }
    s->scope = saved;
    f->visit = 2;
}

b32 sema_module(Arena* a, Module* m, SemaResult* out, MatokiError* err) {
    Sema s;
    mem_zero(&s, sizeof(s));
    s.a = a;
    s.m = m;
    s.err = err;
    mem_zero(out, sizeof(*out));

    u32 uofs = 0, vslot = 0;
    Sym** gtail = &s.globals;
    for (GlobalDecl* g = m->globals; g && !err->failed; g = g->next) {
        for (GlobalDecl* h = m->globals; h != g; h = h->next) {
            if (name_eq(h->name, h->name_len, g->name, g->name_len)) {
                matoki_error(err, g->line, g->col, "yinelenen global:", g->name, g->name_len);
            }
        }
        if (find_builtin(g->name, g->name_len) != BI_NONE) matoki_error(err, g->line, g->col, "yerleşik adı kullanılamaz:", g->name, g->name_len);
        if (g->kind == GD_UNIFORM) {
            if (g->type.base == TB_BOOL) {
                matoki_error(err, g->line, g->col, "bool uniform yok (i32 kullanın):", g->name, g->name_len);
                break;
            }
            if (g->type.base != TB_TEX && g->attrs) matoki_error(err, g->line, g->col, "öznitelik yalnızca texture2d için:", g->name, g->name_len);
            g->offset = uofs;
            uofs += g->type.base == TB_TEX ? 4u : 4u * g->type.n;
        } else {
            if (g->type.base != TB_FLOAT) {
                matoki_error(err, g->line, g->col, "varying f32/vecN olmalı:", g->name, g->name_len);
                break;
            }
            if (g->slot >= 0) vslot = (u32)g->slot;
            g->offset = vslot;
            vslot += g->type.n;
            if (vslot > SHADE_MAX_VARS) matoki_error(err, g->line, g->col, "varying yuvaları 16'yı aşıyor:", g->name, g->name_len);
            if (vslot > out->varying_slots) out->varying_slots = vslot;
        }
        Sym* y = sym_new(&s, g->name, g->name_len, g->kind == GD_UNIFORM ? SYM_UNIFORM : SYM_VARYING, g->type, FALSE);
        y->g = g;
        *gtail = y;
        gtail = &y->next;
    }
    out->uniform_size = uofs;
    for (FnDecl* f = m->fns; f && !err->failed; f = f->next) {
        for (FnDecl* h = m->fns; h != f; h = h->next) {
            if (name_eq(h->name, h->name_len, f->name, f->name_len)) matoki_error(err, f->line, f->col, "yinelenen fonksiyon:", f->name, f->name_len);
        }
        if (find_builtin(f->name, f->name_len) != BI_NONE) matoki_error(err, f->line, f->col, "yerleşik adı kullanılamaz:", f->name, f->name_len);
        sema_fn(&s, f, NULL);
    }
    if (!err->failed) {
        FnDecl* frag = find_fn(&s, "frag", 4);
        if (!frag) {
            matoki_error(err, 1, 1, "giriş noktası yok: fn frag() -> vec4", NULL, 0);
        } else if (frag->param_count != 0 || !type_eq(frag->ret, type_make(TB_FLOAT, 4))) {
            matoki_error(err, frag->line, frag->col, "giriş noktası 'fn frag() -> vec4' olmalı", NULL, 0);
        }
        out->frag = frag;
    }
    return !err->failed;
}
