/* matoki/ir.c — IR kurucu (sabit katlama + CSE), doğrulayıcı, yazdırıcı, DCE.
 * Sabit katlama ir_eval_lane'i çağırır → katlanmış sonuç codegen/ir_eval ile bit-bit aynı.
 * CSE: (op, type, a, b, c, imm) açık adresli karma tablosunda; tüm komutlar saf olduğu için güvenli. */
#include "matoki/ir.h"
#include "core/math.h"
#include "core/mem.h"

static const char* const OP_NAMES[IR_OP_COUNT] = {
    "nop", "const", "uniform", "varying", "attr", "frag_x", "frag_y", "frag_z", "tex_field", "texel_load", "tex_word",
    "fadd", "fsub", "fmul", "fdiv", "fmin", "fmax",
    "fneg", "fabs", "fsqrt", "ffloor", "fceil", "ftrunc", "fnearest",
    "feq", "fne", "flt", "fle", "fgt", "fge",
    "iadd", "isub", "imul", "ineg", "iand", "ior", "ixor", "ishl", "ishr", "ishru", "imin", "imax",
    "ieq", "ine", "ilt", "ile", "igt", "ige",
    "select", "f2i", "i2f", "bitcast_f2i", "bitcast_i2f", "deriv_x", "deriv_y",
};

const char* ir_op_name(IrOp op) { return op < IR_OP_COUNT ? OP_NAMES[op] : "?"; }

u32 ir_op_arity(IrOp op) {
    switch (op) {
    case IR_NOP: case IR_CONST: case IR_UNIFORM: case IR_VARYING: case IR_ATTR:
    case IR_FRAG_X: case IR_FRAG_Y: case IR_FRAG_Z: case IR_TEX_FIELD:
        return 0;
    case IR_TEXEL_LOAD: case IR_TEX_WORD:
    case IR_FNEG: case IR_FABS: case IR_FSQRT: case IR_FFLOOR: case IR_FCEIL: case IR_FTRUNC: case IR_FNEAREST:
    case IR_INEG: case IR_F2I: case IR_I2F: case IR_BITCAST_F2I: case IR_BITCAST_I2F:
    case IR_DERIV_X: case IR_DERIV_Y:
        return 1;
    case IR_SELECT:
        return 3;
    default:
        return 2;
    }
}

b32 ir_op_is_pure_lane(u32 op) { return op >= IR_FADD && op <= IR_BITCAST_I2F; }

/* İşlenen tipi → sonuç tipi; 0 = geçersiz kombinasyon */
u32 ir_result_type(IrOp op, u32 t) {
    if (op >= IR_FADD && op <= IR_FNEAREST) return t == IR_T_F32 ? IR_T_F32 : 0;
    if (op >= IR_FEQ && op <= IR_FGE) return t == IR_T_F32 ? IR_T_I32 : 0;
    if (op >= IR_IADD && op <= IR_IGE) return t == IR_T_I32 ? IR_T_I32 : 0;
    switch (op) {
    case IR_F2I: case IR_BITCAST_F2I: return t == IR_T_F32 ? IR_T_I32 : 0;
    case IR_I2F: case IR_BITCAST_I2F: return t == IR_T_I32 ? IR_T_F32 : 0;
    case IR_DERIV_X: case IR_DERIV_Y: return t == IR_T_F32 ? IR_T_F32 : 0;
    default: return t;
    }
}

/* ---- lane semantiği (wasm f32x4 / i32x4 ile birebir) ---- */

static inline f32 bf(u32 x) { return m_bits_f32(x); }
static inline u32 fb(f32 x) { return m_f32_bits(x); }
static inline u32 mask(b32 c) { return c ? 0xFFFFFFFFu : 0u; }

static u32 wasm_fmin(u32 a, u32 b) {
    f32 x = bf(a), y = bf(b);
    if (x != x || y != y) return 0x7FC00000u;
    if (x == y) return a | b;      /* ±0: min(-0, +0) = -0 */
    return x < y ? a : b;
}

static u32 wasm_fmax(u32 a, u32 b) {
    f32 x = bf(a), y = bf(b);
    if (x != x || y != y) return 0x7FC00000u;
    if (x == y) return a & b;      /* max(-0, +0) = +0 */
    return x > y ? a : b;
}

u32 ir_eval_lane(u32 op, u32 a, u32 b, u32 c) {
    i32 ia = (i32)a, ib = (i32)b;
    switch (op) {
    case IR_FADD: return fb(bf(a) + bf(b));
    case IR_FSUB: return fb(bf(a) - bf(b));
    case IR_FMUL: return fb(bf(a) * bf(b));
    case IR_FDIV: return fb(bf(a) / bf(b));
    case IR_FMIN: return wasm_fmin(a, b);
    case IR_FMAX: return wasm_fmax(a, b);
    case IR_FNEG: return a ^ 0x80000000u;
    case IR_FABS: return a & 0x7FFFFFFFu;
    case IR_FSQRT: return fb(__builtin_sqrtf(bf(a)));
    case IR_FFLOOR: return fb(__builtin_floorf(bf(a)));
    case IR_FCEIL: return fb(__builtin_ceilf(bf(a)));
    case IR_FTRUNC: return fb(__builtin_truncf(bf(a)));
    case IR_FNEAREST: return fb(__builtin_rintf(bf(a)));
    case IR_FEQ: return mask(bf(a) == bf(b));
    case IR_FNE: return mask(bf(a) != bf(b));
    case IR_FLT: return mask(bf(a) < bf(b));
    case IR_FLE: return mask(bf(a) <= bf(b));
    case IR_FGT: return mask(bf(a) > bf(b));
    case IR_FGE: return mask(bf(a) >= bf(b));
    case IR_IADD: return a + b;
    case IR_ISUB: return a - b;
    case IR_IMUL: return a * b;
    case IR_INEG: return 0u - a;
    case IR_IAND: return a & b;
    case IR_IOR: return a | b;
    case IR_IXOR: return a ^ b;
    case IR_ISHL: return a << (b & 31);
    case IR_ISHR: return (u32)(ia >> (b & 31));
    case IR_ISHRU: return a >> (b & 31);
    case IR_IMIN: return ia < ib ? a : b;
    case IR_IMAX: return ia > ib ? a : b;
    case IR_IEQ: return mask(a == b);
    case IR_INE: return mask(a != b);
    case IR_ILT: return mask(ia < ib);
    case IR_ILE: return mask(ia <= ib);
    case IR_IGT: return mask(ia > ib);
    case IR_IGE: return mask(ia >= ib);
    case IR_SELECT: return (b & a) | (c & ~a);
    case IR_F2I: return (u32)m_f2i_sat(bf(a));
    case IR_I2F: return fb((f32)ia);
    case IR_BITCAST_F2I: return a;
    case IR_BITCAST_I2F: return a;
    default: return 0;
    }
}

/* ---- kurucu ---- */

void ir_init(IrFunc* f, Arena* a, u32 cap, b32 no_fold) {
    mem_zero(f, sizeof(*f));
    f->ins = ARENA_ARRAY(a, IrInst, cap);
    f->cap = cap;
    f->hcap = 1;
    while (f->hcap < cap * 2) f->hcap <<= 1;
    f->htab = ARENA_ARRAY(a, u32, f->hcap);
    f->count = 1;   /* ref 0 = NOP */
    f->no_fold = no_fold;
}

static u32 inst_hash(u32 op, u32 type, u32 a, u32 b, u32 c, u32 imm) {
    u32 h = 2166136261u;
    h = (h ^ op) * 16777619u;
    h = (h ^ type) * 16777619u;
    h = (h ^ a) * 16777619u;
    h = (h ^ b) * 16777619u;
    h = (h ^ c) * 16777619u;
    h = (h ^ imm) * 16777619u;
    return h ^ (h >> 15);
}

static IrRef append(IrFunc* f, u32 op, u32 type, IrRef a, IrRef b, IrRef c, u32 imm, u8 uniform) {
    if (f->count >= f->cap) {
        f->overflow = TRUE;
        return 0;
    }
    IrRef r = f->count++;
    IrInst* in = &f->ins[r];
    in->op = (u8)op;
    in->type = (u8)type;
    in->uniform = uniform;
    in->live = 0;
    in->a = a;
    in->b = b;
    in->c = c;
    in->imm = imm;
    return r;
}

static IrRef emit_raw(IrFunc* f, u32 op, u32 type, IrRef a, IrRef b, IrRef c, u32 imm, u8 uniform) {
    if (f->no_fold) return append(f, op, type, a, b, c, imm, uniform);
    u32 h = inst_hash(op, type, a, b, c, imm) & (f->hcap - 1);
    while (f->htab[h]) {
        const IrInst* e = &f->ins[f->htab[h]];
        if (e->op == op && e->type == type && e->a == a && e->b == b && e->c == c && e->imm == imm) {
            f->cse_hits++;
            return f->htab[h];
        }
        h = (h + 1) & (f->hcap - 1);
    }
    IrRef r = append(f, op, type, a, b, c, imm, uniform);
    if (r) f->htab[h] = r;
    return r;
}

IrRef ir_const_i(IrFunc* f, i32 v) { return emit_raw(f, IR_CONST, IR_T_I32, 0, 0, 0, (u32)v, 1); }
IrRef ir_const_f(IrFunc* f, f32 v) { return emit_raw(f, IR_CONST, IR_T_F32, 0, 0, 0, m_f32_bits(v), 1); }

static b32 is_const(const IrFunc* f, IrRef r) { return r && f->ins[r].op == IR_CONST; }

IrRef ir_emit(IrFunc* f, IrOp op, u32 type, IrRef a, IrRef b, IrRef c, u32 imm) {
    if (f->overflow) return 0;
    u32 ar = ir_op_arity(op);
    u8 uniform = 1;
    if (op == IR_VARYING || op == IR_ATTR || op == IR_FRAG_X || op == IR_FRAG_Y || op == IR_FRAG_Z) uniform = 0;
    /* türev her lane'e aynı farkı yayar: işleneni ne olursa olsun quad-sabit */
    if (op == IR_DERIV_X || op == IR_DERIV_Y) ar = 0;
    if (ar >= 1 && !f->ins[a].uniform) uniform = 0;
    if (ar >= 2 && !f->ins[b].uniform) uniform = 0;
    if (ar >= 3 && !f->ins[c].uniform) uniform = 0;

    if (!f->no_fold) {
        /* uniform işlenenin türevi tanım gereği 0 (ir_eval de aynısını yapar) */
        if ((op == IR_DERIV_X || op == IR_DERIV_Y) && f->ins[a].uniform) {
            f->folded++;
            return ir_const_f(f, 0.0f);
        }
        if (op == IR_SELECT) {
            if (b == c) { f->folded++; return b; }
            if (is_const(f, a)) {
                u32 m = f->ins[a].imm;
                if (m == 0xFFFFFFFFu) { f->folded++; return b; }
                if (m == 0) { f->folded++; return c; }
            }
        }
        if (op == IR_BITCAST_F2I || op == IR_BITCAST_I2F) {
            /* bitcast(bitcast(x)) = x */
            if (f->ins[a].op == (op == IR_BITCAST_F2I ? IR_BITCAST_I2F : IR_BITCAST_F2I)) return f->ins[a].a;
        }
        if (ir_op_is_pure_lane(op) && (ar < 1 || is_const(f, a)) && (ar < 2 || is_const(f, b)) && (ar < 3 || is_const(f, c))) {
            u32 v = ir_eval_lane(op, ar >= 1 ? f->ins[a].imm : 0, ar >= 2 ? f->ins[b].imm : 0, ar >= 3 ? f->ins[c].imm : 0);
            f->folded++;
            return emit_raw(f, IR_CONST, type, 0, 0, 0, v, 1);
        }
    }
    return emit_raw(f, op, type, a, b, c, imm, uniform);
}

b32 ir_verify(const IrFunc* f, StrBuf* err) {
    for (u32 i = 1; i < f->count; i++) {
        const IrInst* in = &f->ins[i];
        u32 ar = ir_op_arity((IrOp)in->op);
        const IrRef ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < ar; k++) {
            if (ops[k] == 0 || ops[k] >= i) {
                sb_cstr(err, "geçersiz işlenen: %");
                sb_u64(err, i);
                return FALSE;
            }
        }
        u32 want = 0;
        switch (in->op) {
        case IR_SELECT:
            if (f->ins[in->a].type != IR_T_I32 || f->ins[in->b].type != in->type || f->ins[in->c].type != in->type) want = 1;
            break;
        case IR_TEXEL_LOAD:
        case IR_TEX_WORD:
            if (f->ins[in->a].type != IR_T_I32 || in->type != IR_T_I32) want = 1;
            break;
        default:
            if (ar >= 1) {
                u32 t0 = f->ins[in->a].type;
                if (ar >= 2 && f->ins[in->b].type != t0) want = 1;
                if (ir_result_type((IrOp)in->op, t0) != in->type) want = 1;
            }
            break;
        }
        if (want) {
            sb_cstr(err, "tip uyuşmazlığı: %");
            sb_u64(err, i);
            sb_char(err, ' ');
            sb_cstr(err, ir_op_name((IrOp)in->op));
            return FALSE;
        }
    }
    if (!f->out || f->out >= f->count) {
        sb_cstr(err, "çıktı geçersiz");
        return FALSE;
    }
    for (u32 k = 0; k < f->nxout; k++) {
        if (!f->xout[k] || f->xout[k] >= f->count) {
            sb_cstr(err, "ek çıktı geçersiz");
            return FALSE;
        }
    }
    if (f->kill && (f->kill >= f->count || f->ins[f->kill].type != IR_T_I32)) {
        sb_cstr(err, "discard maskesi geçersiz");
        return FALSE;
    }
    return TRUE;
}

void ir_print(const IrFunc* f, StrBuf* out) {
    for (u32 i = 1; i < f->count; i++) {
        const IrInst* in = &f->ins[i];
        sb_char(out, in->live ? '%' : '.');
        sb_u64(out, i);
        sb_cstr(out, in->type == IR_T_F32 ? ":f " : ":i ");
        sb_cstr(out, in->uniform ? "u " : "v ");
        sb_cstr(out, ir_op_name((IrOp)in->op));
        u32 ar = ir_op_arity((IrOp)in->op);
        const IrRef ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < ar; k++) {
            sb_cstr(out, " %");
            sb_u64(out, ops[k]);
        }
        if (in->op == IR_CONST) {
            sb_char(out, ' ');
            if (in->type == IR_T_F32) sb_f64(out, (f64)m_bits_f32(in->imm), 6);
            else sb_i64(out, (i32)in->imm);
        } else if (ar == 0 || in->op == IR_TEXEL_LOAD || in->op == IR_TEX_WORD) {
            sb_cstr(out, " #");
            sb_u64(out, in->imm);
        }
        sb_char(out, '\n');
    }
    sb_cstr(out, "out %");
    sb_u64(out, f->out);
    if (f->kill) {
        sb_cstr(out, " kill %");
        sb_u64(out, f->kill);
    }
    sb_char(out, '\n');
}

u32 ir_mark_live(IrFunc* f) {
    for (u32 i = 1; i < f->count; i++) f->ins[i].live = 0;
    if (!f->out) return 0;
    f->ins[f->out].live = 1;
    if (f->kill) f->ins[f->kill].live = 1;
    u32 top = MAX(f->out, f->kill);
    for (u32 k = 0; k < f->nxout; k++) {
        f->ins[f->xout[k]].live = 1;
        top = MAX(top, f->xout[k]);
    }
    u32 n = 0;
    for (u32 i = top; i >= 1; i--) {
        IrInst* in = &f->ins[i];
        if (!in->live) continue;
        n++;
        u32 ar = ir_op_arity((IrOp)in->op);
        if (ar >= 1) f->ins[in->a].live = 1;
        if (ar >= 2) f->ins[in->b].live = 1;
        if (ar >= 3) f->ins[in->c].live = 1;
    }
    return n;
}
