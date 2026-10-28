/* matoki/lower.c — tip denetimi geçmiş AST → düz SSA IR.
 * Vektörler skalerleştirilir (bileşen başına bir IR değeri). if/else: iki dal da üretilir,
 * değişen yereller select ile birleşir (SPMD). for: tamamen açılır. Çağrılar satır içi.
 * Yerleşiklerin IR açılımları ve çıktı paketlemesi C referanslarıyla (math.c, ref_shaders.c)
 * aynı işlem sırasını kullanır → aynı formül bit-bit aynı sonucu verir. */
#include "matoki/sema.h"
#include "core/math.h"
#include "core/mem.h"
#include "raster/shade_abi.h"

typedef struct Lower {
    IrFunc*      f;
    Arena*       a;
    MatokiError* err;
    Sym**        live;
    u32          live_count, live_cap;
    u32          budget;   /* açılan döngü turu + satır içi çağrı sınırı */
} Lower;

static b32 failed(Lower* L) { return L->err->failed; }

static void fail(Lower* L, const Node* n, const char* msg) { matoki_error(L->err, n->line, n->col, msg, NULL, 0); }

static u32 irt(Type t) { return t.base == TB_FLOAT ? IR_T_F32 : IR_T_I32; }

static void live_push(Lower* L, Sym* y) {
    if (L->live_count == L->live_cap) {
        u32 ncap = L->live_cap ? L->live_cap * 2 : 64;
        Sym** n = ARENA_ARRAY(L->a, Sym*, ncap);
        for (u32 i = 0; i < L->live_count; i++) n[i] = L->live[i];
        L->live = n;
        L->live_cap = ncap;
    }
    L->live[L->live_count++] = y;
}

/* kısaltmalar */
#define F IR_T_F32
#define I IR_T_I32
static IrRef cf(Lower* L, f32 v) { return ir_const_f(L->f, v); }
static IrRef ci(Lower* L, i32 v) { return ir_const_i(L->f, v); }
static IrRef op1(Lower* L, IrOp op, u32 t, IrRef a) { return ir_emit(L->f, op, t, a, 0, 0, 0); }
static IrRef op2(Lower* L, IrOp op, u32 t, IrRef a, IrRef b) { return ir_emit(L->f, op, t, a, b, 0, 0); }
static IrRef sel(Lower* L, u32 t, IrRef m, IrRef a, IrRef b) { return ir_emit(L->f, IR_SELECT, t, m, a, b, 0); }
static IrRef fadd(Lower* L, IrRef a, IrRef b) { return op2(L, IR_FADD, F, a, b); }
static IrRef fsub(Lower* L, IrRef a, IrRef b) { return op2(L, IR_FSUB, F, a, b); }
static IrRef fmul(Lower* L, IrRef a, IrRef b) { return op2(L, IR_FMUL, F, a, b); }
static IrRef fdiv(Lower* L, IrRef a, IrRef b) { return op2(L, IR_FDIV, F, a, b); }

static u32 lower_expr(Lower* L, Node* e, IrRef out[4]);
static void lower_block(Lower* L, Node* blk);
static void lower_fn(Lower* L, FnDecl* fn, IrRef args[][4], IrRef out[4]);

/* x[i] (n==1 ise yayılım) */
static inline IrRef comp(const IrRef* v, u32 n, u32 i) { return n == 1 ? v[0] : v[i]; }

static IrRef dot_n(Lower* L, const IrRef* a, const IrRef* b, u32 n) {
    IrRef acc = fmul(L, a[0], b[0]);
    for (u32 i = 1; i < n; i++) acc = fadd(L, acc, fmul(L, a[i], b[i]));
    return acc;
}

/* m_sinf ile aynı: [-π, π]'ye indirgeme, yansıtma, 11. derece Taylor */
static IrRef lower_sin(Lower* L, IrRef x) {
    IrRef k = op1(L, IR_FFLOOR, F, fadd(L, fmul(L, x, cf(L, 1.0f / M_TAU_F)), cf(L, 0.5f)));
    IrRef r = fsub(L, x, fmul(L, k, cf(L, M_TAU_F)));
    IrRef hp = cf(L, M_PI_F * 0.5f);
    IrRef hn = cf(L, -M_PI_F * 0.5f);
    IrRef refl_hi = fsub(L, cf(L, M_PI_F), r);
    IrRef refl_lo = fsub(L, cf(L, -M_PI_F), r);
    r = sel(L, F, op2(L, IR_FGT, I, r, hp), refl_hi, sel(L, F, op2(L, IR_FLT, I, r, hn), refl_lo, r));
    IrRef x2 = fmul(L, r, r);
    IrRef p = cf(L, -1.0f / 39916800.0f);
    p = fadd(L, cf(L, 1.0f / 362880.0f), fmul(L, x2, p));
    p = fadd(L, cf(L, -1.0f / 5040.0f), fmul(L, x2, p));
    p = fadd(L, cf(L, 1.0f / 120.0f), fmul(L, x2, p));
    p = fadd(L, cf(L, -1.0f / 6.0f), fmul(L, x2, p));
    p = fadd(L, cf(L, 1.0f), fmul(L, x2, p));
    return fmul(L, r, p);
}

static IrRef tex_field(Lower* L, const GlobalDecl* g, u32 field_ofs) {
    return ir_emit(L->f, IR_TEX_FIELD, I, 0, 0, 0, g->offset | (field_ofs << 16));
}

static IrRef texel_index_clamped(Lower* L, const GlobalDecl* g, IrRef ix, IrRef iy, b32 clamp) {
    IrRef w = tex_field(L, g, TEX_OFF_W);
    if (clamp) {
        IrRef h = tex_field(L, g, TEX_OFF_H);
        ix = op2(L, IR_IMIN, I, op2(L, IR_IMAX, I, ix, ci(L, 0)), op2(L, IR_ISUB, I, w, ci(L, 1)));
        iy = op2(L, IR_IMIN, I, op2(L, IR_IMAX, I, iy, ci(L, 0)), op2(L, IR_ISUB, I, h, ci(L, 1)));
    } else {
        ix = op2(L, IR_IAND, I, ix, tex_field(L, g, TEX_OFF_WMASK));
        iy = op2(L, IR_IAND, I, iy, tex_field(L, g, TEX_OFF_HMASK));
    }
    return op2(L, IR_IADD, I, op2(L, IR_IMUL, I, iy, w), ix);
}

/* ref_sample_level (seviye 0) ve unpack_rgb ile aynı aritmetik */
static void lower_sample(Lower* L, const GlobalDecl* g, IrRef u, IrRef v, IrRef out[4]) {
    IrRef wf = op1(L, IR_I2F, F, tex_field(L, g, TEX_OFF_W));
    IrRef hf = op1(L, IR_I2F, F, tex_field(L, g, TEX_OFF_H));
    IrRef ix = op1(L, IR_F2I, I, op1(L, IR_FFLOOR, F, fmul(L, u, wf)));
    IrRef iy = op1(L, IR_F2I, I, op1(L, IR_FFLOOR, F, fmul(L, v, hf)));
    IrRef idx = texel_index_clamped(L, g, ix, iy, (g->attrs & ATTR_CLAMP) != 0);
    IrRef t = ir_emit(L->f, IR_TEXEL_LOAD, I, idx, 0, 0, g->offset);
    IrRef k = cf(L, 1.0f / 255.0f);
    for (u32 c = 0; c < 4; c++) {
        IrRef sh = c ? op2(L, IR_ISHRU, I, t, ci(L, (i32)(c * 8))) : t;
        out[c] = fmul(L, op1(L, IR_I2F, F, op2(L, IR_IAND, I, sh, ci(L, 0xFF))), k);
    }
}

static u32 lower_builtin(Lower* L, Node* e, IrRef out[4]) {
    Node* args[16];
    IrRef av[16][4];
    u32 an[16];
    u32 n = 0;
    for (Node* a = e->list; a; a = a->next) {
        args[n] = a;
        an[n] = (a->type.base == TB_TEX) ? 0 : lower_expr(L, a, av[n]);
        n++;
    }
    u32 rn = e->type.n;
    switch ((Builtin)e->builtin) {
    case BI_VEC2: case BI_VEC3: case BI_VEC4: {
        if (n == 1 && an[0] == 1) {
            for (u32 i = 0; i < rn; i++) out[i] = av[0][0];
            return rn;
        }
        u32 k = 0;
        for (u32 i = 0; i < n; i++) {
            for (u32 j = 0; j < an[i]; j++) out[k++] = av[i][j];
        }
        return rn;
    }
    case BI_F32:
        out[0] = args[0]->type.base == TB_INT ? op1(L, IR_I2F, F, av[0][0]) : av[0][0];
        return 1;
    case BI_I32:
        out[0] = args[0]->type.base == TB_FLOAT ? op1(L, IR_F2I, I, av[0][0]) : av[0][0];
        return 1;
    case BI_ABS:
        for (u32 i = 0; i < rn; i++) {
            out[i] = e->type.base == TB_INT ? op2(L, IR_IMAX, I, av[0][i], op1(L, IR_INEG, I, av[0][i]))
                                            : op1(L, IR_FABS, F, av[0][i]);
        }
        return rn;
    case BI_MIN: case BI_MAX: {
        b32 is_int = e->type.base == TB_INT;
        IrOp op = e->builtin == BI_MIN ? (is_int ? IR_IMIN : IR_FMIN) : (is_int ? IR_IMAX : IR_FMAX);
        for (u32 i = 0; i < rn; i++) out[i] = op2(L, op, irt(e->type), av[0][i], comp(av[1], an[1], i));
        return rn;
    }
    case BI_CLAMP: {
        b32 is_int = e->type.base == TB_INT;
        for (u32 i = 0; i < rn; i++) {
            IrRef m = op2(L, is_int ? IR_IMAX : IR_FMAX, irt(e->type), av[0][i], comp(av[1], an[1], i));
            out[i] = op2(L, is_int ? IR_IMIN : IR_FMIN, irt(e->type), m, comp(av[2], an[2], i));
        }
        return rn;
    }
    case BI_FLOOR: case BI_CEIL: case BI_TRUNC: case BI_ROUND: case BI_SQRT: {
        IrOp op = e->builtin == BI_FLOOR ? IR_FFLOOR : e->builtin == BI_CEIL ? IR_FCEIL
                : e->builtin == BI_TRUNC ? IR_FTRUNC : e->builtin == BI_ROUND ? IR_FNEAREST : IR_FSQRT;
        for (u32 i = 0; i < rn; i++) out[i] = op1(L, op, F, av[0][i]);
        return rn;
    }
    case BI_FRACT:
        for (u32 i = 0; i < rn; i++) out[i] = fsub(L, av[0][i], op1(L, IR_FFLOOR, F, av[0][i]));
        return rn;
    case BI_SIN:
        for (u32 i = 0; i < rn; i++) out[i] = lower_sin(L, av[0][i]);
        return rn;
    case BI_COS:
        for (u32 i = 0; i < rn; i++) out[i] = lower_sin(L, fadd(L, av[0][i], cf(L, M_PI_F * 0.5f)));
        return rn;
    case BI_MIX:
        for (u32 i = 0; i < rn; i++) {
            out[i] = fadd(L, av[0][i], fmul(L, fsub(L, av[1][i], av[0][i]), comp(av[2], an[2], i)));
        }
        return rn;
    case BI_STEP:
        for (u32 i = 0; i < rn; i++) {
            out[i] = sel(L, F, op2(L, IR_FLT, I, av[1][i], comp(av[0], an[0], i)), cf(L, 0.0f), cf(L, 1.0f));
        }
        return rn;
    case BI_SMOOTHSTEP:
        for (u32 i = 0; i < rn; i++) {
            IrRef e0 = comp(av[0], an[0], i), e1 = comp(av[1], an[1], i);
            IrRef t = fdiv(L, fsub(L, av[2][i], e0), fsub(L, e1, e0));
            t = op2(L, IR_FMIN, F, op2(L, IR_FMAX, F, t, cf(L, 0.0f)), cf(L, 1.0f));
            out[i] = fmul(L, fmul(L, t, t), fsub(L, cf(L, 3.0f), fmul(L, cf(L, 2.0f), t)));
        }
        return rn;
    case BI_MOD:
        for (u32 i = 0; i < rn; i++) {
            IrRef y = comp(av[1], an[1], i);
            out[i] = fsub(L, av[0][i], fmul(L, y, op1(L, IR_FFLOOR, F, fdiv(L, av[0][i], y))));
        }
        return rn;
    case BI_DOT:
        out[0] = dot_n(L, av[0], av[1], an[0]);
        return 1;
    case BI_LENGTH:
        out[0] = op1(L, IR_FSQRT, F, dot_n(L, av[0], av[0], an[0]));
        return 1;
    case BI_DISTANCE: {
        IrRef d[4];
        for (u32 i = 0; i < an[0]; i++) d[i] = fsub(L, av[0][i], av[1][i]);
        out[0] = op1(L, IR_FSQRT, F, dot_n(L, d, d, an[0]));
        return 1;
    }
    case BI_NORMALIZE: {
        IrRef inv = fdiv(L, cf(L, 1.0f), op1(L, IR_FSQRT, F, dot_n(L, av[0], av[0], an[0])));
        for (u32 i = 0; i < rn; i++) out[i] = fmul(L, av[0][i], inv);
        return rn;
    }
    case BI_CROSS: {
        const IrRef* a = av[0];
        const IrRef* b = av[1];
        out[0] = fsub(L, fmul(L, a[1], b[2]), fmul(L, a[2], b[1]));
        out[1] = fsub(L, fmul(L, a[2], b[0]), fmul(L, a[0], b[2]));
        out[2] = fsub(L, fmul(L, a[0], b[1]), fmul(L, a[1], b[0]));
        return 3;
    }
    case BI_REFLECT: {
        IrRef s = fmul(L, cf(L, 2.0f), dot_n(L, av[1], av[0], an[0]));
        for (u32 i = 0; i < rn; i++) out[i] = fsub(L, av[0][i], fmul(L, s, av[1][i]));
        return rn;
    }
    case BI_SELECT: {
        u32 t = irt(e->type);
        for (u32 i = 0; i < rn; i++) out[i] = sel(L, t, av[0][0], av[1][i], av[2][i]);
        return rn;
    }
    case BI_SAMPLE:
        lower_sample(L, args[0]->sym->g, av[1][0], av[1][1], out);
        return 4;
    case BI_TEXEL_FETCH: {
        const GlobalDecl* g = args[0]->sym->g;
        IrRef idx = texel_index_clamped(L, g, av[1][0], av[2][0], TRUE);
        out[0] = ir_emit(L->f, IR_TEXEL_LOAD, I, idx, 0, 0, g->offset);
        return 1;
    }
    case BI_TEX_SIZE: {
        const GlobalDecl* g = args[0]->sym->g;
        out[0] = op1(L, IR_I2F, F, tex_field(L, g, TEX_OFF_W));
        out[1] = op1(L, IR_I2F, F, tex_field(L, g, TEX_OFF_H));
        return 2;
    }
    case BI_BITCAST_F32:
        out[0] = op1(L, IR_BITCAST_I2F, F, av[0][0]);
        return 1;
    case BI_BITCAST_I32:
        out[0] = op1(L, IR_BITCAST_F2I, I, av[0][0]);
        return 1;
    case BI_DFDX: case BI_DFDY:
        for (u32 i = 0; i < rn; i++) out[i] = op1(L, e->builtin == BI_DFDX ? IR_DERIV_X : IR_DERIV_Y, F, av[0][i]);
        return rn;
    case BI_FWIDTH:
        for (u32 i = 0; i < rn; i++) {
            out[i] = fadd(L, op1(L, IR_FABS, F, op1(L, IR_DERIV_X, F, av[0][i])),
                          op1(L, IR_FABS, F, op1(L, IR_DERIV_Y, F, av[0][i])));
        }
        return rn;
    case BI_FRAG_COORD:
        out[0] = ir_emit(L->f, IR_FRAG_X, F, 0, 0, 0, 0);
        out[1] = ir_emit(L->f, IR_FRAG_Y, F, 0, 0, 0, 0);
        out[2] = ir_emit(L->f, IR_FRAG_Z, F, 0, 0, 0, 0);
        return 3;
    default:
        fail(L, e, "yerleşik lower edilemedi");
        return rn;
    }
}

static u32 lower_binary_vals(Lower* L, TokKind op, Type rt, Type ta, const IrRef* a, u32 na, const IrRef* b, u32 nb, IrRef out[4]) {
    u32 rn = rt.n;
    switch (op) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: {
        b32 fl = rt.base == TB_FLOAT;
        IrOp o = op == T_PLUS ? (fl ? IR_FADD : IR_IADD) : op == T_MINUS ? (fl ? IR_FSUB : IR_ISUB)
               : op == T_STAR ? (fl ? IR_FMUL : IR_IMUL) : IR_FDIV;
        for (u32 i = 0; i < rn; i++) out[i] = op2(L, o, irt(rt), comp(a, na, i), comp(b, nb, i));
        return rn;
    }
    case T_LT: case T_LE: case T_GT: case T_GE: case T_EQEQ: case T_NE: {
        b32 fl = ta.base == TB_FLOAT;
        IrOp o;
        switch (op) {
        case T_LT: o = fl ? IR_FLT : IR_ILT; break;
        case T_LE: o = fl ? IR_FLE : IR_ILE; break;
        case T_GT: o = fl ? IR_FGT : IR_IGT; break;
        case T_GE: o = fl ? IR_FGE : IR_IGE; break;
        case T_EQEQ: o = fl ? IR_FEQ : IR_IEQ; break;
        default: o = fl ? IR_FNE : IR_INE; break;
        }
        out[0] = op2(L, o, I, a[0], b[0]);
        return 1;
    }
    case T_ANDAND: out[0] = op2(L, IR_IAND, I, a[0], b[0]); return 1;
    case T_OROR: out[0] = op2(L, IR_IOR, I, a[0], b[0]); return 1;
    case T_AMP: out[0] = op2(L, IR_IAND, I, a[0], b[0]); return 1;
    case T_PIPE: out[0] = op2(L, IR_IOR, I, a[0], b[0]); return 1;
    case T_CARET: out[0] = op2(L, IR_IXOR, I, a[0], b[0]); return 1;
    case T_SHL: out[0] = op2(L, IR_ISHL, I, a[0], b[0]); return 1;
    case T_SHR: out[0] = op2(L, IR_ISHR, I, a[0], b[0]); return 1;
    default: return 0;
    }
}

static u32 lower_expr(Lower* L, Node* e, IrRef out[4]) {
    if (failed(L)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return e->type.n ? e->type.n : 1;
    }
    switch (e->kind) {
    case NK_INT: out[0] = ci(L, (i32)e->ival); return 1;
    case NK_FLOAT: out[0] = cf(L, e->fval); return 1;
    case NK_BOOL: out[0] = ci(L, e->ival ? -1 : 0); return 1;
    case NK_NAME: {
        Sym* y = e->sym;
        u32 n = y->type.n;
        for (u32 i = 0; i < n; i++) {
            if (y->kind == SYM_LOCAL) out[i] = y->vals[i];
            else if (y->kind == SYM_UNIFORM) out[i] = ir_emit(L->f, IR_UNIFORM, irt(y->type), 0, 0, 0, y->g->offset + 4 * i);
            else out[i] = ir_emit(L->f, IR_VARYING, F, 0, 0, 0, y->g->offset + i);
        }
        return n;
    }
    case NK_UNARY: {
        IrRef a[4];
        u32 n = lower_expr(L, e->a, a);
        for (u32 i = 0; i < n; i++) {
            if (e->op == T_MINUS) out[i] = e->type.base == TB_FLOAT ? op1(L, IR_FNEG, F, a[i]) : op1(L, IR_INEG, I, a[i]);
            else out[i] = op2(L, IR_IXOR, I, a[i], ci(L, -1));
        }
        return n;
    }
    case NK_BINARY: {
        IrRef a[4], b[4];
        u32 na = lower_expr(L, e->a, a);
        u32 nb = lower_expr(L, e->b, b);
        return lower_binary_vals(L, e->op, e->type, e->a->type, a, na, b, nb, out);
    }
    case NK_SWIZZLE: {
        IrRef a[4];
        lower_expr(L, e->a, a);
        for (u32 i = 0; i < e->swz_n; i++) out[i] = a[e->swz[i]];
        return e->swz_n;
    }
    case NK_CALL: {
        if (e->builtin) return lower_builtin(L, e, out);
        IrRef args[16][4];
        u32 k = 0;
        for (Node* a = e->list; a; a = a->next) lower_expr(L, a, args[k++]);
        lower_fn(L, e->fn, args, out);
        return e->type.n;
    }
    default:
        fail(L, e, "ifade lower edilemedi");
        return 1;
    }
}

static void lower_stmt(Lower* L, Node* st) {
    if (failed(L)) return;
    switch (st->kind) {
    case NK_LET:
    case NK_VAR: {
        Sym* y = st->sym;
        if (st->a) {
            lower_expr(L, st->a, y->vals);
        } else {
            for (u32 i = 0; i < y->type.n; i++) y->vals[i] = y->type.base == TB_FLOAT ? cf(L, 0.0f) : ci(L, 0);
        }
        live_push(L, y);
        return;
    }
    case NK_ASSIGN: {
        IrRef v[4];
        lower_expr(L, st->c ? st->c : st->b, v);
        Node* target = st->a;
        if (target->kind == NK_SWIZZLE) {
            Sym* y = target->a->sym;
            for (u32 i = 0; i < target->swz_n; i++) y->vals[target->swz[i]] = v[i];
        } else {
            Sym* y = target->sym;
            for (u32 i = 0; i < y->type.n; i++) y->vals[i] = v[i];
        }
        return;
    }
    case NK_IF: {
        IrRef c[4];
        lower_expr(L, st->a, c);
        u32 n = L->live_count;
        IrRef (*before)[4] = (IrRef(*)[4])arena_alloc(L->a, sizeof(IrRef) * 4 * (n ? n : 1), 4);
        IrRef (*then_v)[4] = (IrRef(*)[4])arena_alloc(L->a, sizeof(IrRef) * 4 * (n ? n : 1), 4);
        for (u32 i = 0; i < n; i++) mem_copy(before[i], L->live[i]->vals, sizeof(IrRef) * 4);
        lower_block(L, st->b);
        for (u32 i = 0; i < n; i++) {
            mem_copy(then_v[i], L->live[i]->vals, sizeof(IrRef) * 4);
            mem_copy(L->live[i]->vals, before[i], sizeof(IrRef) * 4);
        }
        if (st->c) lower_block(L, st->c);
        for (u32 i = 0; i < n; i++) {
            Sym* y = L->live[i];
            for (u32 k = 0; k < y->type.n; k++) {
                if (then_v[i][k] != y->vals[k]) y->vals[k] = sel(L, irt(y->type), c[0], then_v[i][k], y->vals[k]);
            }
        }
        return;
    }
    case NK_FOR: {
        for (i64 it = st->lo; it < st->hi && !failed(L); it++) {
            if (L->budget == 0) {
                fail(L, st, "açılan döngü/çağrı sınırı aşıldı (shader çok büyük)");
                return;
            }
            L->budget--;
            st->sym->vals[0] = ci(L, (i32)it);
            u32 saved = L->live_count;
            live_push(L, st->sym);
            lower_block(L, st->b);
            L->live_count = saved;
        }
        return;
    }
    case NK_EXPR: {
        IrRef tmp[4];
        lower_expr(L, st->a, tmp);
        return;
    }
    case NK_BLOCK:
        lower_block(L, st);
        return;
    default:
        fail(L, st, "deyim lower edilemedi");
        return;
    }
}

static void lower_block(Lower* L, Node* blk) {
    u32 saved = L->live_count;
    for (Node* st = blk->list; st && !failed(L); st = st->next) lower_stmt(L, st);
    L->live_count = saved;
}

static void lower_fn(Lower* L, FnDecl* fn, IrRef args[][4], IrRef out[4]) {
    if (L->budget == 0) {
        matoki_error(L->err, fn->line, fn->col, "açılan döngü/çağrı sınırı aşıldı (shader çok büyük)", NULL, 0);
        return;
    }
    L->budget--;
    u32 saved = L->live_count;
    u32 i = 0;
    for (Param* p = fn->params; p; p = p->next, i++) {
        mem_copy(p->sym->vals, args[i], sizeof(IrRef) * 4);
        live_push(L, p->sym);
    }
    Node* st = fn->body->list;
    for (; st && st->next && !failed(L); st = st->next) lower_stmt(L, st);
    if (st && !failed(L)) lower_expr(L, st->a, out);
    L->live_count = saved;
}

/* pack_rgb ile aynı: m_f2i_sat(m_clampf(c, 0, 1) * 255 + 0.5), r | g<<8 | b<<16 | a<<24 */
static IrRef pack_color(Lower* L, const IrRef c[4]) {
    IrRef bytes[4];
    for (u32 i = 0; i < 4; i++) {
        IrRef x = c[i];
        IrRef cl = sel(L, F, op2(L, IR_FLT, I, x, cf(L, 0.0f)), cf(L, 0.0f),
                       sel(L, F, op2(L, IR_FGT, I, x, cf(L, 1.0f)), cf(L, 1.0f), x));
        bytes[i] = op1(L, IR_F2I, I, fadd(L, fmul(L, cl, cf(L, 255.0f)), cf(L, 0.5f)));
    }
    IrRef r = bytes[0];
    r = op2(L, IR_IOR, I, r, op2(L, IR_ISHL, I, bytes[1], ci(L, 8)));
    r = op2(L, IR_IOR, I, r, op2(L, IR_ISHL, I, bytes[2], ci(L, 16)));
    r = op2(L, IR_IOR, I, r, op2(L, IR_ISHL, I, bytes[3], ci(L, 24)));
    return r;
}

b32 lower_module(Arena* a, Module* m, const SemaResult* s, IrFunc* f, MatokiError* err) {
    UNUSED(m);
    Lower L;
    mem_zero(&L, sizeof(L));
    L.f = f;
    L.a = a;
    L.err = err;
    L.budget = 4096;
    IrRef color[4];
    lower_fn(&L, s->frag, NULL, color);
    if (err->failed) return FALSE;
    f->out = pack_color(&L, color);
    if (f->overflow) {
        matoki_error(err, s->frag->line, s->frag->col, "IR kapasitesi aşıldı (shader çok büyük)", NULL, 0);
        return FALSE;
    }
    return TRUE;
}

#undef F
#undef I
