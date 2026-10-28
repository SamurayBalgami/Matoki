/* matoki/codegen.c — IR → wasm ikili modülü (SIMD128).
 * Yerel düzeni: 0 = q (ShadeQuad*), 1 = uniforms, 2 = out, 3.. = canlı IR değerleri (v128),
 * en sonda 1 v128 geçici (texel adresleri) ve 1 i32 geçici (doku işaretçisi). Semantik ir_eval ile birebir:
 *   - kaydırma miktarı uniform ise lane 0'dan alınır, değilse lane lane skaler kaydırma
 *   - DERIV işleneni uniform ise sabit 0 (ir_eval aynı kuralı uygular)
 *   - texel / doku sözcüğü okuma: adresler vektörde, lane başına v128.load32_lane (wasm'da gather yok);
 *     indeks quad-sabitse tek v128.load32_splat. Uniform'lar da v128.load32_splat ile okunur. */
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
    S_V128_LOAD = 0, S_V128_LOAD32_SPLAT = 9, S_V128_STORE = 11, S_V128_LOAD32_LANE = 86, S_V128_CONST = 12, S_I8X16_SHUFFLE = 13,
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

/* skaler komutlar (quad-sabit değerler skaler yerellerde hesaplanır) */
enum {
    OP_DROP = 0x1a, OP_SELECT = 0x1b, OP_LOCAL_TEE = 0x22, OP_F32_CONST = 0x43,
    OP_I32_EQ = 0x46, OP_I32_NE = 0x47, OP_I32_LT_S = 0x48, OP_I32_GT_S = 0x4a, OP_I32_LE_S = 0x4c, OP_I32_GE_S = 0x4e,
    OP_F32_EQ = 0x5b, OP_F32_NE = 0x5c, OP_F32_LT = 0x5d, OP_F32_GT = 0x5e, OP_F32_LE = 0x5f, OP_F32_GE = 0x60,
    OP_I32_SUB = 0x6b, OP_I32_MUL = 0x6c, OP_I32_AND = 0x71, OP_I32_OR = 0x72, OP_I32_XOR = 0x73,
    OP_F32_ABS = 0x8b, OP_F32_NEG = 0x8c, OP_F32_CEIL = 0x8d, OP_F32_FLOOR = 0x8e, OP_F32_TRUNC = 0x8f,
    OP_F32_NEAREST = 0x90, OP_F32_SQRT = 0x91, OP_F32_ADD = 0x92, OP_F32_SUB = 0x93, OP_F32_MUL = 0x94,
    OP_F32_DIV = 0x95, OP_F32_MIN = 0x96, OP_F32_MAX = 0x97,
    OP_F32_CONVERT_I32_S = 0xb2, OP_I32_REINTERPRET_F32 = 0xbc, OP_F32_REINTERPRET_I32 = 0xbe,
    OP_MISC = 0xfc   /* 0xfc 0x00 = i32.trunc_sat_f32_s */
};
enum { S_F32X4_EXTRACT = 31 };

/* tek komutluk skaler eşler (wasm skaler ve f32x4/i32x4 lane semantiği aynı) */
static i32 scalar_op(u32 op) {
    switch (op) {
    case IR_FADD: return OP_F32_ADD;
    case IR_FSUB: return OP_F32_SUB;
    case IR_FMUL: return OP_F32_MUL;
    case IR_FDIV: return OP_F32_DIV;
    case IR_FMIN: return OP_F32_MIN;
    case IR_FMAX: return OP_F32_MAX;
    case IR_FNEG: return OP_F32_NEG;
    case IR_FABS: return OP_F32_ABS;
    case IR_FSQRT: return OP_F32_SQRT;
    case IR_FFLOOR: return OP_F32_FLOOR;
    case IR_FCEIL: return OP_F32_CEIL;
    case IR_FTRUNC: return OP_F32_TRUNC;
    case IR_FNEAREST: return OP_F32_NEAREST;
    case IR_IADD: return OP_I32_ADD;
    case IR_ISUB: return OP_I32_SUB;
    case IR_IMUL: return OP_I32_MUL;
    case IR_IAND: return OP_I32_AND;
    case IR_IOR: return OP_I32_OR;
    case IR_IXOR: return OP_I32_XOR;
    case IR_ISHL: return OP_I32_SHL;
    case IR_ISHR: return OP_I32_SHR_S;
    case IR_ISHRU: return OP_I32_SHR_U;
    case IR_I2F: return OP_F32_CONVERT_I32_S;
    case IR_BITCAST_F2I: return OP_I32_REINTERPRET_F32;
    case IR_BITCAST_I2F: return OP_F32_REINTERPRET_I32;
    default: return -1;
    }
}

/* karşılaştırma → 0/1; maske için 0 - sonuç */
static i32 scalar_cmp(u32 op) {
    switch (op) {
    case IR_FEQ: return OP_F32_EQ;
    case IR_FNE: return OP_F32_NE;
    case IR_FLT: return OP_F32_LT;
    case IR_FLE: return OP_F32_LE;
    case IR_FGT: return OP_F32_GT;
    case IR_FGE: return OP_F32_GE;
    case IR_IEQ: return OP_I32_EQ;
    case IR_INE: return OP_I32_NE;
    case IR_ILT: return OP_I32_LT_S;
    case IR_ILE: return OP_I32_LE_S;
    case IR_IGT: return OP_I32_GT_S;
    case IR_IGE: return OP_I32_GE_S;
    default: return -1;
    }
}

#define Q_X_OFS   0
#define Q_Y_OFS   4
#define Q_KILL_OFS 12
#define Q_Z_OFS   16
#define Q_VAR_OFS 32

/* Gövde türleri: FULL her şeyi hesaplar ("shade"); PRELUDE yalnızca uniform'dan türeyen ve quad'a
 * kalan değerleri hesaplayıp önbelleğe yazar ("prelude"); FAST onları önbellekten okur ("shade_fast"). */
enum { BODY_FULL, BODY_PRELUDE, BODY_FAST };

/* quad-sabit değerler skaler yerellerde mi (varsayılan) yoksa v128'de mi hesaplansın (ölçüm anahtarı) */
static b32 g_scalar = TRUE;
void codegen_set_scalar(b32 on) { g_scalar = on; }
static inline b32 scal(const IrFunc* f, u32 i) { return g_scalar && f->ins[i].uniform; }

/* tüm lane'lere lane k'yı yay */
static void shuffle_broadcast(WB* b, u32 k) {
    simd(b, S_I8X16_SHUFFLE);
    for (u32 l = 0; l < 4; l++) {
        for (u32 j = 0; j < 4; j++) byte(b, k * 4 + j);
    }
}

typedef struct Plan {
    u8*  du;          /* çizim-sabit: yalnızca uniform/doku/sabitten türer (lane ve quad'dan bağımsız) */
    u8*  hoist;       /* önbelleğe alınan: çizim-sabit, ucuz kaynak değil, quad'a bağlı bir kullanıcısı var */
    u8*  need_pre;
    u8*  need_fast;
    u32* slot;        /* önbellek yuvası (16 bayt) */
    u32  nhoist;
    u32* rel;         /* canlı komutun v128 yerel indeksi (göreli) */
    u32* srel;        /* quad-sabit canlı komutun skaler yerel indeksi (tipine göre göreli) */
    u32  nv128, ni32, nf32;
    u32  cache;       /* önbelleğin bellek adresi */
    /* gövde başına */
    u8*  emit;        /* bu gövdede hesaplanır */
    u8*  load;        /* bu gövdede önbellekten okunur (FAST) */
    u8*  need_v;      /* v128 biçimi gerekiyor */
    u8*  need_s;      /* skaler biçimi gerekiyor */
} Plan;

typedef struct Emit {
    WB*           b;
    const IrFunc* f;
    const Plan*   pl;
    u32           base;     /* ilk v128 yerelinin indeksi (= parametre sayısı) */
    u32           uni;      /* uniform işaretçisi parametresi */
    u32           q;        /* ShadeQuad işaretçisi parametresi (PRELUDE'da yok) */
} Emit;

static inline u32 V_(const Emit* e, u32 ref) { return e->base + e->pl->rel[ref]; }
static inline u32 tmp_v(const Emit* e) { return e->base + e->pl->nv128; }
static inline u32 tmp_i(const Emit* e) { return e->base + e->pl->nv128 + 1; }
static inline u32 S_(const Emit* e, u32 ref) {
    u32 first_i = e->base + e->pl->nv128 + 2;
    return e->f->ins[ref].type == IR_T_F32 ? first_i + e->pl->ni32 + e->pl->srel[ref] : first_i + e->pl->srel[ref];
}
static inline void vget(const Emit* e, u32 ref) { lget(e->b, V_(e, ref)); }
static inline void sget(const Emit* e, u32 ref) { lget(e->b, S_(e, ref)); }

static b32 is_leaf(u32 op) { return op == IR_CONST || op == IR_UNIFORM || op == IR_TEX_FIELD; }
static b32 is_shift(u32 op) { return op == IR_ISHL || op == IR_ISHR || op == IR_ISHRU; }

/* quad-sabit komut i'nin skaler değerini yığına koyar */
static b32 push_scalar(const Emit* e, u32 i) {
    WB* b = e->b;
    const IrFunc* f = e->f;
    const IrInst* in = &f->ins[i];
    b32 fl = in->type == IR_T_F32;
    switch (in->op) {
    case IR_CONST:
        if (fl) {
            byte(b, OP_F32_CONST);
            for (u32 k = 0; k < 4; k++) byte(b, (in->imm >> (k * 8)) & 0xFF);
        } else {
            byte(b, OP_I32_CONST);
            sleb(b, (i32)in->imm);
        }
        return TRUE;
    case IR_UNIFORM:
        lget(b, e->uni);
        byte(b, fl ? OP_F32_LOAD : OP_I32_LOAD);
        memarg(b, 2, in->imm);
        return TRUE;
    case IR_TEX_FIELD:
        lget(b, e->uni);
        byte(b, OP_I32_LOAD);
        memarg(b, 2, in->imm & 0xFFFF);
        byte(b, OP_I32_LOAD);
        memarg(b, 2, in->imm >> 16);
        return TRUE;
    case IR_TEXEL_LOAD:
    case IR_TEX_WORD:
        lget(b, e->uni);
        byte(b, OP_I32_LOAD);
        memarg(b, 2, in->imm);
        if (in->op == IR_TEXEL_LOAD) {
            byte(b, OP_I32_LOAD);
            memarg(b, 2, 0);
        }
        sget(e, in->a);
        byte(b, OP_I32_CONST);
        sleb(b, 2);
        byte(b, OP_I32_SHL);
        byte(b, OP_I32_ADD);
        byte(b, OP_I32_LOAD);
        memarg(b, 2, 0);
        return TRUE;
    case IR_DERIV_X:
    case IR_DERIV_Y:
        if (f->ins[in->a].uniform) {
            byte(b, OP_F32_CONST);
            for (u32 k = 0; k < 4; k++) byte(b, 0);
        } else {
            /* v[1] - v[0] (x) veya v[2] - v[0] (y): iki lane çıkar, skaler fark */
            vget(e, in->a);
            simd(b, S_F32X4_EXTRACT);
            byte(b, in->op == IR_DERIV_X ? 1 : 2);
            vget(e, in->a);
            simd(b, S_F32X4_EXTRACT);
            byte(b, 0);
            byte(b, OP_F32_SUB);
        }
        return TRUE;
    case IR_SELECT: {
        /* koşul bir karşılaştırmaysa maske tamdır (0 / ~0): tek wasm select, karşılaştırma doğrudan koşul */
        const IrInst* m = &f->ins[in->a];
        i32 cmp = scalar_cmp(m->op);
        if (cmp >= 0 && e->pl->emit[in->a]) {   /* önbellekten okunan karşılaştırmanın işlenenleri yok */
            sget(e, in->b);
            sget(e, in->c);
            sget(e, m->a);
            sget(e, m->b);
            byte(b, (u32)cmp);
            byte(b, OP_SELECT);
            return TRUE;
        }
        /* genel: (b & m) | (c & ~m), F32 bitleri i32 olarak */
        sget(e, in->b);
        if (fl) byte(b, OP_I32_REINTERPRET_F32);
        sget(e, in->a);
        byte(b, OP_I32_AND);
        sget(e, in->c);
        if (fl) byte(b, OP_I32_REINTERPRET_F32);
        sget(e, in->a);
        byte(b, OP_I32_CONST);
        sleb(b, -1);
        byte(b, OP_I32_XOR);
        byte(b, OP_I32_AND);
        byte(b, OP_I32_OR);
        if (fl) byte(b, OP_F32_REINTERPRET_I32);
        return TRUE;
    }
    case IR_INEG:
        byte(b, OP_I32_CONST);
        sleb(b, 0);
        sget(e, in->a);
        byte(b, OP_I32_SUB);
        return TRUE;
    case IR_IMIN:
    case IR_IMAX:
        sget(e, in->a);
        sget(e, in->b);
        sget(e, in->a);
        sget(e, in->b);
        byte(b, in->op == IR_IMIN ? OP_I32_LT_S : OP_I32_GT_S);
        byte(b, OP_SELECT);
        return TRUE;
    case IR_F2I:
        sget(e, in->a);
        byte(b, OP_MISC);
        uleb(b, 0);
        return TRUE;
    default: {
        i32 cmp = scalar_cmp(in->op);
        if (cmp >= 0) {
            byte(b, OP_I32_CONST);
            sleb(b, 0);
            sget(e, in->a);
            sget(e, in->b);
            byte(b, (u32)cmp);
            byte(b, OP_I32_SUB);
            return TRUE;
        }
        i32 sop = scalar_op(in->op);
        if (sop < 0) return FALSE;
        u32 ar = ir_op_arity((IrOp)in->op);
        sget(e, in->a);
        if (ar >= 2) sget(e, in->b);
        byte(b, (u32)sop);
        return TRUE;
    }
    }
}

/* yaprak quad-sabitin doğrudan v128 biçimi */
static void push_leaf_vector(const Emit* e, u32 i) {
    WB* b = e->b;
    const IrInst* in = &e->f->ins[i];
    if (in->op == IR_CONST) {
        v128_const_u32x4(b, in->imm, in->imm, in->imm, in->imm);
    } else if (in->op == IR_UNIFORM) {
        lget(b, e->uni);
        simd(b, S_V128_LOAD32_SPLAT);
        memarg(b, 2, in->imm);
    } else {
        lget(b, e->uni);
        byte(b, OP_I32_LOAD);
        memarg(b, 2, in->imm & 0xFFFF);
        simd(b, S_V128_LOAD32_SPLAT);
        memarg(b, 2, in->imm >> 16);
    }
}

static b32 emit_uniform(const Emit* e, u32 i) {
    WB* b = e->b;
    const Plan* pl = e->pl;
    const IrInst* in = &e->f->ins[i];
    if (is_leaf(in->op) && !pl->need_s[i]) {
        if (pl->need_v[i]) {
            push_leaf_vector(e, i);
            lset(b, V_(e, i));
        }
        return TRUE;
    }
    if (!push_scalar(e, i)) return FALSE;
    if (pl->need_s[i]) {
        byte(b, pl->need_v[i] ? OP_LOCAL_TEE : OP_LOCAL_SET);
        uleb(b, S_(e, i));
    } else if (!pl->need_v[i]) {
        byte(b, OP_DROP);
        return TRUE;
    }
    if (pl->need_v[i]) {
        simd(b, in->type == IR_T_F32 ? S_F32X4_SPLAT : S_I32X4_SPLAT);
        lset(b, V_(e, i));
    }
    return TRUE;
}

/* lane'e bağlı (vektör) komut */
static b32 emit_vector(const Emit* e, u32 i) {
    WB* body = e->b;
    const IrFunc* f = e->f;
    const IrInst* in = &f->ins[i];
    const u32 dst = V_(e, i);
    switch (in->op) {
    case IR_CONST:
    case IR_UNIFORM:
    case IR_TEX_FIELD:
        push_leaf_vector(e, i);
        break;
    case IR_DERIV_X:
    case IR_DERIV_Y:
        if (f->ins[in->a].uniform) {
            v128_const_u32x4(body, 0, 0, 0, 0);
        } else {
            vget(e, in->a);
            vget(e, in->a);
            shuffle_broadcast(body, in->op == IR_DERIV_X ? 1 : 2);
            vget(e, in->a);
            vget(e, in->a);
            shuffle_broadcast(body, 0);
            simd(body, S_F32X4_SUB);
        }
        break;
    case IR_VARYING:
        lget(body, e->q);
        simd(body, S_V128_LOAD);
        memarg(body, 4, Q_VAR_OFS + in->imm * 16);
        break;
    case IR_FRAG_X:
    case IR_FRAG_Y: {
        b32 is_x = in->op == IR_FRAG_X;
        lget(body, e->q);
        byte(body, OP_I32_LOAD);
        memarg(body, 2, is_x ? Q_X_OFS : Q_Y_OFS);
        simd(body, S_I32X4_SPLAT);
        if (is_x) v128_const_u32x4(body, 0, 1, 0, 1);
        else v128_const_u32x4(body, 0, 0, 1, 1);
        simd(body, S_I32X4_ADD);
        simd(body, S_F32X4_CONVERT_I32X4_S);
        u32 half = m_f32_bits(0.5f);
        v128_const_u32x4(body, half, half, half, half);
        simd(body, S_F32X4_ADD);
        break;
    }
    case IR_FRAG_Z:
        lget(body, e->q);
        simd(body, S_V128_LOAD);
        memarg(body, 4, Q_Z_OFS);
        break;
    case IR_TEXEL_LOAD:
    case IR_TEX_WORD:
        /* tmp = tex->px (TEXEL_LOAD) ya da tex (TEX_WORD); adresler vektörde, lane başına load32_lane */
        lget(body, e->uni);
        byte(body, OP_I32_LOAD);
        memarg(body, 2, in->imm);
        if (in->op == IR_TEXEL_LOAD) {
            byte(body, OP_I32_LOAD);
            memarg(body, 2, 0);
        }
        lset(body, tmp_i(e));
        if (f->ins[in->a].uniform) {
            /* quad-sabit indeks (skaler kapalıyken): lane 0'dan tek okuma + yayılım */
            lget(body, tmp_i(e));
            vget(e, in->a);
            simd(body, S_I32X4_EXTRACT);
            byte(body, 0);
            byte(body, OP_I32_CONST);
            sleb(body, 2);
            byte(body, OP_I32_SHL);
            byte(body, OP_I32_ADD);
            simd(body, S_V128_LOAD32_SPLAT);
            memarg(body, 2, 0);
            break;
        }
        vget(e, in->a);
        byte(body, OP_I32_CONST);
        sleb(body, 2);
        simd(body, S_I32X4_SHL);
        lget(body, tmp_i(e));
        simd(body, S_I32X4_SPLAT);
        simd(body, S_I32X4_ADD);
        lset(body, tmp_v(e));
        for (u32 l = 0; l < 4; l++) {
            lget(body, tmp_v(e));
            simd(body, S_I32X4_EXTRACT);
            byte(body, l);
            lget(body, l == 0 ? tmp_v(e) : dst);   /* lane 0: içerik önemsiz, hepsi yazılır */
            simd(body, S_V128_LOAD32_LANE);
            memarg(body, 2, 0);
            byte(body, l);
            if (l < 3) lset(body, dst);
        }
        break;
    case IR_ISHL:
    case IR_ISHR:
    case IR_ISHRU:
        if (f->ins[in->b].uniform) {
            vget(e, in->a);
            if (scal(f, in->b)) {
                sget(e, in->b);
            } else {
                vget(e, in->b);
                simd(body, S_I32X4_EXTRACT);
                byte(body, 0);
            }
            simd(body, in->op == IR_ISHL ? S_I32X4_SHL : in->op == IR_ISHR ? S_I32X4_SHR_S : S_I32X4_SHR_U);
        } else {
            vget(e, in->a);
            for (u32 l = 0; l < 4; l++) {
                vget(e, in->a);
                simd(body, S_I32X4_EXTRACT);
                byte(body, l);
                vget(e, in->b);
                simd(body, S_I32X4_EXTRACT);
                byte(body, l);
                byte(body, in->op == IR_ISHL ? OP_I32_SHL : in->op == IR_ISHR ? OP_I32_SHR_S : OP_I32_SHR_U);
                simd(body, S_I32X4_REPLACE);
                byte(body, l);
            }
        }
        break;
    case IR_SELECT:
        vget(e, in->b);
        vget(e, in->c);
        vget(e, in->a);
        simd(body, S_V128_BITSELECT);
        break;
    case IR_BITCAST_F2I:
    case IR_BITCAST_I2F:
        vget(e, in->a);
        break;
    default: {
        i32 sop = simple_simd_op(in->op);
        if (sop < 0) return FALSE;
        u32 ar = ir_op_arity((IrOp)in->op);
        vget(e, in->a);
        if (ar >= 2) vget(e, in->b);
        simd(body, (u32)sop);
        break;
    }
    }
    lset(body, dst);
    return TRUE;
}

/* gövdenin hesap kümesi ve her değerin hangi biçim(ler)de gerektiği */
static void body_needs(const IrFunc* f, Plan* pl, u32 kind) {
    u32 n = f->count;
    mem_zero(pl->emit, n);
    mem_zero(pl->load, n);
    mem_zero(pl->need_v, n);
    mem_zero(pl->need_s, n);
    for (u32 i = 1; i < n; i++) {
        if (!f->ins[i].live) continue;
        if (kind == BODY_FULL) pl->emit[i] = 1;
        else if (kind == BODY_PRELUDE) pl->emit[i] = pl->need_pre[i];
        else {
            pl->load[i] = pl->hoist[i];
            pl->emit[i] = pl->need_fast[i] && !pl->hoist[i];
        }
    }
    for (u32 i = 1; i < n; i++) {
        if (!pl->emit[i]) continue;
        const IrInst* in = &f->ins[i];
        u32 ar = ir_op_arity((IrOp)in->op);
        const u32 ops[3] = { in->a, in->b, in->c };
        if (scal(f, i)) {
            if (in->op == IR_DERIV_X || in->op == IR_DERIV_Y) {
                if (!f->ins[in->a].uniform) pl->need_v[in->a] = 1;
            } else {
                for (u32 k = 0; k < ar; k++) pl->need_s[ops[k]] = 1;
            }
        } else {
            for (u32 k = 0; k < ar; k++) {
                if (k == 1 && is_shift(in->op) && scal(f, in->b)) pl->need_s[in->b] = 1;
                else pl->need_v[ops[k]] = 1;
            }
        }
    }
    if (kind == BODY_PRELUDE) {
        for (u32 i = 1; i < n; i++) {
            if (pl->hoist[i]) pl->need_v[i] = 1;
        }
    } else {
        pl->need_v[f->out] = 1;
        if (f->kill) pl->need_v[f->kill] = 1;
    }
}

static b32 emit_body(WB* body, const IrFunc* f, Plan* pl, u32 kind) {
    body_needs(f, pl, kind);
    Emit e;
    e.b = body;
    e.f = f;
    e.pl = pl;
    e.base = kind == BODY_PRELUDE ? 1 : 3;
    e.uni = kind == BODY_PRELUDE ? 0 : 1;
    e.q = 0;
    /* yerel bildirimleri: v128 × (nv128 + 1 geçici), i32 × (1 geçici + skalerler), f32 × skalerler */
    uleb(body, 3);
    uleb(body, pl->nv128 + 1);
    byte(body, 0x7b);
    uleb(body, 1 + pl->ni32);
    byte(body, 0x7f);
    uleb(body, pl->nf32);
    byte(body, 0x7d);
    if (kind == BODY_FAST) {
        for (u32 i = 1; i < f->count; i++) {
            if (!pl->load[i]) continue;
            if (pl->need_v[i]) {
                byte(body, OP_I32_CONST);
                sleb(body, (i32)pl->cache);
                simd(body, S_V128_LOAD);
                memarg(body, 4, pl->slot[i] * 16);
                lset(body, V_(&e, i));
            }
            if (pl->need_s[i]) {
                byte(body, OP_I32_CONST);
                sleb(body, (i32)pl->cache);
                byte(body, f->ins[i].type == IR_T_F32 ? OP_F32_LOAD : OP_I32_LOAD);
                memarg(body, 2, pl->slot[i] * 16);
                lset(body, S_(&e, i));
            }
        }
    }
    for (u32 i = 1; i < f->count; i++) {
        if (!pl->emit[i]) continue;
        b32 ok = scal(f, i) ? emit_uniform(&e, i) : emit_vector(&e, i);
        if (!ok) return FALSE;
    }
    if (kind == BODY_PRELUDE) {
        for (u32 i = 1; i < f->count; i++) {
            if (!pl->hoist[i]) continue;
            byte(body, OP_I32_CONST);
            sleb(body, (i32)pl->cache);
            vget(&e, i);
            simd(body, S_V128_STORE);
            memarg(body, 4, pl->slot[i] * 16);
        }
    } else {
        /* *out = sonuç */
        lget(body, 2);
        vget(&e, f->out);
        simd(body, S_V128_STORE);
        memarg(body, 4, 0);
        if (f->kill) {
            /* q->kill = i32x4.bitmask(kill) */
            lget(body, 0);
            vget(&e, f->kill);
            simd(body, S_I32X4_BITMASK);
            byte(body, OP_I32_STORE);
            memarg(body, 2, Q_KILL_OFS);
        }
    }
    byte(body, OP_END);
    return !body->overflow;
}

static b32 cheap_source(u32 op) { return is_leaf(op); }

/* çizim-sabit analizi + önbellek planı */
static void plan_make(const IrFunc* f, Arena* a, Plan* pl) {
    u32 n = f->count;
    pl->du = ARENA_ARRAY(a, u8, n);
    pl->hoist = ARENA_ARRAY(a, u8, n);
    pl->need_pre = ARENA_ARRAY(a, u8, n);
    pl->need_fast = ARENA_ARRAY(a, u8, n);
    pl->slot = ARENA_ARRAY(a, u32, n);
    pl->rel = ARENA_ARRAY(a, u32, n);
    pl->srel = ARENA_ARRAY(a, u32, n);
    pl->emit = ARENA_ARRAY(a, u8, n);
    pl->load = ARENA_ARRAY(a, u8, n);
    pl->need_v = ARENA_ARRAY(a, u8, n);
    pl->need_s = ARENA_ARRAY(a, u8, n);
    pl->nv128 = pl->ni32 = pl->nf32 = 0;
    for (u32 i = 1; i < n; i++) {
        const IrInst* in = &f->ins[i];
        if (in->live) {
            pl->rel[i] = pl->nv128++;
            if (in->uniform) pl->srel[i] = in->type == IR_T_F32 ? pl->nf32++ : pl->ni32++;
        }
        u32 op = in->op;
        if (cheap_source(op)) {
            pl->du[i] = 1;
        } else if (op == IR_VARYING || op == IR_FRAG_X || op == IR_FRAG_Y || op == IR_FRAG_Z ||
                   op == IR_DERIV_X || op == IR_DERIV_Y) {
            pl->du[i] = 0;
        } else {
            u32 ar = ir_op_arity((IrOp)op);
            pl->du[i] = (ar < 1 || pl->du[in->a]) && (ar < 2 || pl->du[in->b]) && (ar < 3 || pl->du[in->c]);
        }
    }
    /* önbelleğe alınacaklar: quad'a bağlı canlı bir kullanıcısı olan (ya da çıktı olan) pahalı çizim-sabitleri */
    for (u32 i = 1; i < n; i++) {
        const IrInst* in = &f->ins[i];
        if (!in->live || pl->du[i]) continue;
        u32 ar = ir_op_arity((IrOp)in->op);
        const u32 ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < ar; k++) {
            if (pl->du[ops[k]] && !cheap_source(f->ins[ops[k]].op)) pl->hoist[ops[k]] = 1;
        }
    }
    if (pl->du[f->out] && !cheap_source(f->ins[f->out].op)) pl->hoist[f->out] = 1;
    if (f->kill && pl->du[f->kill] && !cheap_source(f->ins[f->kill].op)) pl->hoist[f->kill] = 1;
    pl->nhoist = 0;
    for (u32 i = 1; i < n; i++) {
        if (pl->hoist[i]) pl->slot[i] = pl->nhoist++;
    }
    /* gereksinimler: geriye doğru; FAST önbellekteki değerlerde durur, PRELUDE önbellek köklerinden başlar */
    pl->need_fast[f->out] = 1;
    if (f->kill) pl->need_fast[f->kill] = 1;
    for (u32 i = n - 1; i >= 1; i--) {
        const IrInst* in = &f->ins[i];
        if (pl->hoist[i]) pl->need_pre[i] = 1;
        u32 ar = ir_op_arity((IrOp)in->op);
        const u32 ops[3] = { in->a, in->b, in->c };
        for (u32 k = 0; k < ar; k++) {
            if (pl->need_fast[i] && !pl->hoist[i]) pl->need_fast[ops[k]] = 1;
            if (pl->need_pre[i]) pl->need_pre[ops[k]] = 1;
        }
    }
    u8* cache = (u8*)arena_alloc(a, 16u * (pl->nhoist ? pl->nhoist : 1u), 16);
    pl->cache = (u32)(uptr)cache;
}

static void section(WB* m, u32 id, const WB* s) {
    byte(m, id);
    uleb(m, s->len);
    append(m, s);
}

b32 codegen_wasm(IrFunc* f, Arena* a, WasmModule* out) {
    mem_zero(out, sizeof(*out));
    u32 live = ir_mark_live(f);
    out->live_insts = live;
    Plan pl;
    plan_make(f, a, &pl);
    out->hoisted = pl.nhoist;
    out->cache = (void*)(uptr)pl.cache;

    WB bodies[3];
    for (u32 k = 0; k < 3; k++) {
        wb_init(&bodies[k], a, 256 + live * 112 + pl.nhoist * 32);
        if (!emit_body(&bodies[k], f, &pl, k)) return FALSE;
    }
    out->body_bytes = bodies[BODY_FULL].len;

    WB m;
    wb_init(&m, a, bodies[0].len + bodies[1].len + bodies[2].len + 256);
    /* başlık */
    byte(&m, 0x00); byte(&m, 0x61); byte(&m, 0x73); byte(&m, 0x6d);
    byte(&m, 0x01); byte(&m, 0x00); byte(&m, 0x00); byte(&m, 0x00);
    WB s;
    /* tipler: 0 = (i32 i32 i32) -> () [ShadeFn], 1 = (i32) -> () [PreludeFn] */
    wb_init(&s, a, 32);
    uleb(&s, 2);
    byte(&s, 0x60); uleb(&s, 3); byte(&s, 0x7f); byte(&s, 0x7f); byte(&s, 0x7f); uleb(&s, 0);
    byte(&s, 0x60); uleb(&s, 1); byte(&s, 0x7f); uleb(&s, 0);
    section(&m, 1, &s);
    /* import: env.memory, en az 1 sayfa, üst sınır yok */
    wb_init(&s, a, 32);
    uleb(&s, 1);
    name(&s, "env");
    name(&s, "memory");
    byte(&s, 0x02);
    byte(&s, 0x00);
    uleb(&s, 1);
    section(&m, 2, &s);
    /* fonksiyonlar: shade (tip 0), prelude (tip 1), shade_fast (tip 0) */
    wb_init(&s, a, 8);
    uleb(&s, 3);
    uleb(&s, 0); uleb(&s, 1); uleb(&s, 0);
    section(&m, 3, &s);
    /* export'lar (sıra = tabloya eklenme sırası, jit.h) */
    wb_init(&s, a, 64);
    uleb(&s, 3);
    name(&s, "shade"); byte(&s, 0x00); uleb(&s, 0);
    name(&s, "prelude"); byte(&s, 0x00); uleb(&s, 1);
    name(&s, "shade_fast"); byte(&s, 0x00); uleb(&s, 2);
    section(&m, 7, &s);
    /* kod */
    wb_init(&s, a, bodies[0].len + bodies[1].len + bodies[2].len + 32);
    uleb(&s, 3);
    for (u32 k = 0; k < 3; k++) {
        uleb(&s, bodies[k].len);
        append(&s, &bodies[k]);
    }
    section(&m, 10, &s);
    if (m.overflow || s.overflow) return FALSE;
    out->bytes = m.p;
    out->len = m.len;
    return TRUE;
}
