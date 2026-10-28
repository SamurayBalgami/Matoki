/* matoki/codegen.c — IR → wasm ikili modülü (SIMD128).
 * Yerel düzeni: 0 = q (ShadeQuad*), 1 = uniforms, 2 = out, 3.. = canlı IR değerleri (v128),
 * en sonda 1 adet i32 geçici (doku işaretçisi). Semantik ir_eval ile birebir:
 *   - kaydırma miktarı uniform ise lane 0'dan alınır, değilse lane lane skaler kaydırma
 *   - DERIV işleneni uniform ise sabit 0 (ir_eval aynı kuralı uygular)
 *   - texel / doku sözcüğü okuma: 4 skaler i32.load (wasm'da gather yok); indeks quad-sabitse 1 okuma */
#include "matoki/codegen.h"
#include "core/math.h"
#include "core/mem.h"

typedef struct WB {
    u8* p;
    u32 len, cap;
    b32 overflow;
} WB;

static void wb_init(WB* b, Arena* a, u32 cap) {
    b->p = (u8*)arena_alloc_nozero(a, cap, 1);
    b->len = 0;
    b->cap = cap;
    b->overflow = FALSE;
}

static void byte(WB* b, u32 v) {
    if (b->len < b->cap) b->p[b->len] = (u8)v;
    else b->overflow = TRUE;
    b->len++;
}

static void uleb(WB* b, u32 v) {
    do {
        u8 c = v & 0x7F;
        v >>= 7;
        if (v) c |= 0x80;
        byte(b, c);
    } while (v);
}

static void sleb(WB* b, i32 v) {
    b32 more = TRUE;
    while (more) {
        u8 c = (u8)(v & 0x7F);
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) more = FALSE;
        else c |= 0x80;
        byte(b, c);
    }
}

static void name(WB* b, const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    uleb(b, n);
    for (u32 i = 0; i < n; i++) byte(b, (u8)s[i]);
}

static void append(WB* dst, const WB* src) {
    for (u32 i = 0; i < src->len && i < src->cap; i++) byte(dst, src->p[i]);
    if (src->overflow) dst->overflow = TRUE;
}

/* ---- komutlar ---- */
enum {
    OP_END = 0x0b, OP_LOCAL_GET = 0x20, OP_LOCAL_SET = 0x21,
    OP_I32_LOAD = 0x28, OP_F32_LOAD = 0x2a, OP_I32_STORE = 0x36, OP_I32_CONST = 0x41,
    OP_I32_ADD = 0x6a, OP_I32_SHL = 0x74, OP_I32_SHR_S = 0x75, OP_I32_SHR_U = 0x76,
    OP_SIMD = 0xfd
};

enum {
    S_V128_LOAD = 0, S_V128_STORE = 11, S_V128_CONST = 12, S_I8X16_SHUFFLE = 13,
    S_I32X4_SPLAT = 17, S_F32X4_SPLAT = 19, S_I32X4_EXTRACT = 27, S_I32X4_REPLACE = 28,
    S_I32X4_EQ = 55, S_I32X4_NE = 56, S_I32X4_LT_S = 57, S_I32X4_GT_S = 59, S_I32X4_LE_S = 61, S_I32X4_GE_S = 63,
    S_F32X4_EQ = 65, S_F32X4_NE = 66, S_F32X4_LT = 67, S_F32X4_GT = 68, S_F32X4_LE = 69, S_F32X4_GE = 70,
    S_V128_AND = 78, S_V128_OR = 80, S_V128_XOR = 81, S_V128_BITSELECT = 82,
    S_F32X4_CEIL = 103, S_F32X4_FLOOR = 104, S_F32X4_TRUNC = 105, S_F32X4_NEAREST = 106,
    S_I32X4_NEG = 161, S_I32X4_BITMASK = 164, S_I32X4_SHL = 171, S_I32X4_SHR_S = 172, S_I32X4_SHR_U = 173,
    S_I32X4_ADD = 174, S_I32X4_SUB = 177, S_I32X4_MUL = 181, S_I32X4_MIN_S = 182, S_I32X4_MAX_S = 184,
    S_F32X4_ABS = 224, S_F32X4_NEG = 225, S_F32X4_SQRT = 227, S_F32X4_ADD = 228, S_F32X4_SUB = 229,
    S_F32X4_MUL = 230, S_F32X4_DIV = 231, S_F32X4_MIN = 232, S_F32X4_MAX = 233,
    S_I32X4_TRUNC_SAT_F32X4_S = 248, S_F32X4_CONVERT_I32X4_S = 250
};

static void simd(WB* b, u32 op) {
    byte(b, OP_SIMD);
    uleb(b, op);
}

static void memarg(WB* b, u32 align_log2, u32 offset) {
    uleb(b, align_log2);
    uleb(b, offset);
}

static void lget(WB* b, u32 i) { byte(b, OP_LOCAL_GET); uleb(b, i); }
static void lset(WB* b, u32 i) { byte(b, OP_LOCAL_SET); uleb(b, i); }

static void v128_const_u32x4(WB* b, u32 x0, u32 x1, u32 x2, u32 x3) {
    simd(b, S_V128_CONST);
    const u32 xs[4] = { x0, x1, x2, x3 };
    for (u32 l = 0; l < 4; l++) {
        for (u32 k = 0; k < 4; k++) byte(b, (xs[l] >> (k * 8)) & 0xFF);
    }
}

/* tüm lane'lere lane k'yı yay */
static void shuffle_broadcast(WB* b, u32 k) {
    simd(b, S_I8X16_SHUFFLE);
    for (u32 l = 0; l < 4; l++) {
        for (u32 j = 0; j < 4; j++) byte(b, k * 4 + j);
    }
}

static i32 simple_simd_op(u32 op) {
    switch (op) {
    case IR_FADD: return S_F32X4_ADD;
    case IR_FSUB: return S_F32X4_SUB;
    case IR_FMUL: return S_F32X4_MUL;
    case IR_FDIV: return S_F32X4_DIV;
    case IR_FMIN: return S_F32X4_MIN;
    case IR_FMAX: return S_F32X4_MAX;
    case IR_FNEG: return S_F32X4_NEG;
    case IR_FABS: return S_F32X4_ABS;
    case IR_FSQRT: return S_F32X4_SQRT;
    case IR_FFLOOR: return S_F32X4_FLOOR;
    case IR_FCEIL: return S_F32X4_CEIL;
    case IR_FTRUNC: return S_F32X4_TRUNC;
    case IR_FNEAREST: return S_F32X4_NEAREST;
    case IR_FEQ: return S_F32X4_EQ;
    case IR_FNE: return S_F32X4_NE;
    case IR_FLT: return S_F32X4_LT;
    case IR_FLE: return S_F32X4_LE;
    case IR_FGT: return S_F32X4_GT;
    case IR_FGE: return S_F32X4_GE;
    case IR_IADD: return S_I32X4_ADD;
    case IR_ISUB: return S_I32X4_SUB;
    case IR_IMUL: return S_I32X4_MUL;
    case IR_INEG: return S_I32X4_NEG;
    case IR_IAND: return S_V128_AND;
    case IR_IOR: return S_V128_OR;
    case IR_IXOR: return S_V128_XOR;
    case IR_IMIN: return S_I32X4_MIN_S;
    case IR_IMAX: return S_I32X4_MAX_S;
    case IR_IEQ: return S_I32X4_EQ;
    case IR_INE: return S_I32X4_NE;
    case IR_ILT: return S_I32X4_LT_S;
    case IR_ILE: return S_I32X4_LE_S;
    case IR_IGT: return S_I32X4_GT_S;
    case IR_IGE: return S_I32X4_GE_S;
    case IR_F2I: return S_I32X4_TRUNC_SAT_F32X4_S;
    case IR_I2F: return S_F32X4_CONVERT_I32X4_S;
    default: return -1;
    }
}

#define Q_X_OFS   0
#define Q_Y_OFS   4
#define Q_KILL_OFS 12
#define Q_Z_OFS   16
#define Q_VAR_OFS 32

b32 codegen_wasm(IrFunc* f, Arena* a, WasmModule* out) {
    mem_zero(out, sizeof(*out));
    u32 live = ir_mark_live(f);
    out->live_insts = live;
    u32* local_of = ARENA_ARRAY(a, u32, f->count);
    u32 next = 3;
    for (u32 i = 1; i < f->count; i++) {
        if (f->ins[i].live) local_of[i] = next++;
    }
    const u32 nv128 = next - 3;
    const u32 tmp_i32 = next;

    WB body;
    wb_init(&body, a, 256 + live * 96);
    /* yerel bildirimleri */
    uleb(&body, 2);
    uleb(&body, nv128);
    byte(&body, 0x7b);
    uleb(&body, 1);
    byte(&body, 0x7f);

    for (u32 i = 1; i < f->count; i++) {
        const IrInst* in = &f->ins[i];
        if (!in->live) continue;
        u32 la = local_of[in->a], lb = local_of[in->b], lc = local_of[in->c];
        switch (in->op) {
        case IR_CONST:
            v128_const_u32x4(&body, in->imm, in->imm, in->imm, in->imm);
            break;
        case IR_UNIFORM:
            lget(&body, 1);
            if (in->type == IR_T_F32) {
                byte(&body, OP_F32_LOAD);
                memarg(&body, 2, in->imm);
                simd(&body, S_F32X4_SPLAT);
            } else {
                byte(&body, OP_I32_LOAD);
                memarg(&body, 2, in->imm);
                simd(&body, S_I32X4_SPLAT);
            }
            break;
        case IR_VARYING:
            lget(&body, 0);
            simd(&body, S_V128_LOAD);
            memarg(&body, 4, Q_VAR_OFS + in->imm * 16);
            break;
        case IR_FRAG_X:
        case IR_FRAG_Y: {
            b32 is_x = in->op == IR_FRAG_X;
            lget(&body, 0);
            byte(&body, OP_I32_LOAD);
            memarg(&body, 2, is_x ? Q_X_OFS : Q_Y_OFS);
            simd(&body, S_I32X4_SPLAT);
            if (is_x) v128_const_u32x4(&body, 0, 1, 0, 1);
            else v128_const_u32x4(&body, 0, 0, 1, 1);
            simd(&body, S_I32X4_ADD);
            simd(&body, S_F32X4_CONVERT_I32X4_S);
            u32 half = m_f32_bits(0.5f);
            v128_const_u32x4(&body, half, half, half, half);
            simd(&body, S_F32X4_ADD);
            break;
        }
        case IR_FRAG_Z:
            lget(&body, 0);
            simd(&body, S_V128_LOAD);
            memarg(&body, 4, Q_Z_OFS);
            break;
        case IR_TEX_FIELD:
            lget(&body, 1);
            byte(&body, OP_I32_LOAD);
            memarg(&body, 2, in->imm & 0xFFFF);
            byte(&body, OP_I32_LOAD);
            memarg(&body, 2, in->imm >> 16);
            simd(&body, S_I32X4_SPLAT);
            break;
        case IR_TEXEL_LOAD:
        case IR_TEX_WORD:
            /* tmp = tex->px (TEXEL_LOAD) ya da tex (TEX_WORD) */
            lget(&body, 1);
            byte(&body, OP_I32_LOAD);
            memarg(&body, 2, in->imm);
            if (in->op == IR_TEXEL_LOAD) {
                byte(&body, OP_I32_LOAD);
                memarg(&body, 2, 0);
            }
            lset(&body, tmp_i32);
            if (f->ins[in->a].uniform) {
                /* quad-sabit indeks: tek okuma + yayılım */
                lget(&body, tmp_i32);
                lget(&body, la);
                simd(&body, S_I32X4_EXTRACT);
                byte(&body, 0);
                byte(&body, OP_I32_CONST);
                sleb(&body, 2);
                byte(&body, OP_I32_SHL);
                byte(&body, OP_I32_ADD);
                byte(&body, OP_I32_LOAD);
                memarg(&body, 2, 0);
                simd(&body, S_I32X4_SPLAT);
                break;
            }
            lget(&body, la);   /* taban vektör (lane'ler değiştirilecek) */
            for (u32 l = 0; l < 4; l++) {
                lget(&body, tmp_i32);
                lget(&body, la);
                simd(&body, S_I32X4_EXTRACT);
                byte(&body, l);
                byte(&body, OP_I32_CONST);
                sleb(&body, 2);
                byte(&body, OP_I32_SHL);
                byte(&body, OP_I32_ADD);
                byte(&body, OP_I32_LOAD);
                memarg(&body, 2, 0);
                simd(&body, S_I32X4_REPLACE);
                byte(&body, l);
            }
            break;
        case IR_ISHL:
        case IR_ISHR:
        case IR_ISHRU:
            if (f->ins[in->b].uniform) {
                lget(&body, la);
                lget(&body, lb);
                simd(&body, S_I32X4_EXTRACT);
                byte(&body, 0);
                simd(&body, in->op == IR_ISHL ? S_I32X4_SHL : in->op == IR_ISHR ? S_I32X4_SHR_S : S_I32X4_SHR_U);
            } else {
                lget(&body, la);
                for (u32 l = 0; l < 4; l++) {
                    lget(&body, la);
                    simd(&body, S_I32X4_EXTRACT);
                    byte(&body, l);
                    lget(&body, lb);
                    simd(&body, S_I32X4_EXTRACT);
                    byte(&body, l);
                    byte(&body, in->op == IR_ISHL ? OP_I32_SHL : in->op == IR_ISHR ? OP_I32_SHR_S : OP_I32_SHR_U);
                    simd(&body, S_I32X4_REPLACE);
                    byte(&body, l);
                }
            }
            break;
        case IR_SELECT:
            lget(&body, lb);
            lget(&body, lc);
            lget(&body, la);
            simd(&body, S_V128_BITSELECT);
            break;
        case IR_BITCAST_F2I:
        case IR_BITCAST_I2F:
            lget(&body, la);
            break;
        case IR_DERIV_X:
        case IR_DERIV_Y:
            if (f->ins[in->a].uniform) {
                v128_const_u32x4(&body, 0, 0, 0, 0);
            } else {
                lget(&body, la);
                lget(&body, la);
                shuffle_broadcast(&body, in->op == IR_DERIV_X ? 1 : 2);
                lget(&body, la);
                lget(&body, la);
                shuffle_broadcast(&body, 0);
                simd(&body, S_F32X4_SUB);
            }
            break;
        default: {
            i32 sop = simple_simd_op(in->op);
            if (sop < 0) return FALSE;
            u32 ar = ir_op_arity((IrOp)in->op);
            lget(&body, la);
            if (ar >= 2) lget(&body, lb);
            simd(&body, (u32)sop);
            break;
        }
        }
        lset(&body, local_of[i]);
    }
    /* *out = sonuç */
    lget(&body, 2);
    lget(&body, local_of[f->out]);
    simd(&body, S_V128_STORE);
    memarg(&body, 4, 0);
    if (f->kill) {
        /* q->kill = i32x4.bitmask(kill) */
        lget(&body, 0);
        lget(&body, local_of[f->kill]);
        simd(&body, S_I32X4_BITMASK);
        byte(&body, OP_I32_STORE);
        memarg(&body, 2, Q_KILL_OFS);
    }
    byte(&body, OP_END);
    if (body.overflow) return FALSE;
    out->body_bytes = body.len;

    WB m;
    wb_init(&m, a, body.len + 128);
    /* başlık */
    byte(&m, 0x00); byte(&m, 0x61); byte(&m, 0x73); byte(&m, 0x6d);
    byte(&m, 0x01); byte(&m, 0x00); byte(&m, 0x00); byte(&m, 0x00);
    /* tip: (i32 i32 i32) -> () */
    byte(&m, 1);
    uleb(&m, 7);
    uleb(&m, 1);
    byte(&m, 0x60);
    uleb(&m, 3);
    byte(&m, 0x7f); byte(&m, 0x7f); byte(&m, 0x7f);
    uleb(&m, 0);
    /* import: env.memory, en az 1 sayfa, üst sınır yok */
    {
        WB s;
        wb_init(&s, a, 32);
        uleb(&s, 1);
        name(&s, "env");
        name(&s, "memory");
        byte(&s, 0x02);
        byte(&s, 0x00);
        uleb(&s, 1);
        byte(&m, 2);
        uleb(&m, s.len);
        append(&m, &s);
    }
    /* fonksiyon */
    byte(&m, 3);
    uleb(&m, 2);
    uleb(&m, 1);
    uleb(&m, 0);
    /* export "shade" */
    byte(&m, 7);
    uleb(&m, 9);
    uleb(&m, 1);
    name(&m, "shade");
    byte(&m, 0x00);
    uleb(&m, 0);
    /* kod */
    {
        WB len_buf;
        wb_init(&len_buf, a, 8);
        uleb(&len_buf, body.len);
        byte(&m, 10);
        uleb(&m, 1 + len_buf.len + body.len);
        uleb(&m, 1);
        append(&m, &len_buf);
        append(&m, &body);
    }
    if (m.overflow) return FALSE;
    out->bytes = m.p;
    out->len = m.len;
    return TRUE;
}
