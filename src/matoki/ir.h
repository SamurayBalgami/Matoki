/* matoki/ir.h — Matoki ara gösterimi (SÖZLEŞME: ir_eval ve codegen birebir bu semantiğe uyar).
 * Düz (dallanmasız) SSA: her komut 4 lane'lik bir değer üretir (lane = quad pikseli; lane sırası
 * shade_abi.h). Tipler yalnızca F32 ve I32; bool = I32 maske (tüm bitler 1 = doğru, 0 = yanlış).
 * Kontrol akışı lower'da select'e, döngüler açılmaya, fonksiyonlar satır içine çevrilir.
 * ir_emit sabit katlar (ir_eval_lane ile, yani bit-bit aynı) ve CSE yapar. Ref 0 geçersizdir. */
#ifndef MATOKI_IR_H
#define MATOKI_IR_H
#include "core/arena.h"
#include "core/fmt.h"

typedef enum IrOp {
    IR_NOP,
    /* kaynaklar */
    IR_CONST,       /* imm = bitler */
    IR_UNIFORM,     /* imm = uniform bloğunda bayt ofseti (4 bayt yükle, tüm lane'lere) */
    IR_VARYING,     /* imm = varying yuvası → ShadeQuad.var[imm][lane] (F32) */
    IR_FRAG_X,      /* (f32)(q.x + lane_x) + 0.5 */
    IR_FRAG_Y,      /* (f32)(q.y + lane_y) + 0.5 */
    IR_FRAG_Z,      /* q.z[lane] */
    IR_TEX_FIELD,   /* imm = tex_ofs | (alan_ofs << 16): *(u32*)(*(Texture**)(uni + tex_ofs) + alan_ofs) (I32) */
    IR_TEXEL_LOAD,  /* a = indeks (I32), imm = tex_ofs: px[a[lane]] (I32) */
    IR_TEX_WORD,    /* a = u32 indeksi (I32), imm = tex_ofs: ((u32*)tex)[a[lane]] (I32; mip ofset tablosu) */
    /* f32 aritmetik (IEEE, wasm f32x4 semantiği) */
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
    IR_FMIN, IR_FMAX,                  /* wasm min/max: NaN yayılır, -0 < +0 */
    IR_FNEG, IR_FABS, IR_FSQRT, IR_FFLOOR, IR_FCEIL, IR_FTRUNC, IR_FNEAREST,
    IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE,   /* → I32 maske */
    /* i32 aritmetik (sarmalayan) */
    IR_IADD, IR_ISUB, IR_IMUL, IR_INEG,
    IR_IAND, IR_IOR, IR_IXOR,
    IR_ISHL, IR_ISHR, IR_ISHRU,        /* kaydırma miktarı & 31 */
    IR_IMIN, IR_IMAX,                  /* işaretli */
    IR_IEQ, IR_INE, IR_ILT, IR_ILE, IR_IGT, IR_IGE,   /* işaretli → maske */
    /* diğer */
    IR_SELECT,      /* a = maske, b = doğru, c = yanlış: (b & a) | (c & ~a) bit düzeyinde.
                     * F32 için maske tam olmalı (lane başına 0 / ~0): kısmi maske wasm'ın belirsiz
                     * NaN işaret/yük bitlerini sızdırır. lower yalnızca karşılaştırma maskesi üretir. */
    IR_F2I,         /* doygun kesme (NaN → 0) */
    IR_I2F,         /* işaretli dönüştürme, en yakına yuvarlama */
    IR_BITCAST_F2I, IR_BITCAST_I2F,
    IR_DERIV_X,     /* kaba: tüm lane'lere v[1] - v[0] (F32); işlenen uniform ise sabit 0.
                     * Sonuç lane'ler arasında aynıdır → uniform bayrağı 1 (quad-sabit). */
    IR_DERIV_Y,     /* kaba: tüm lane'lere v[2] - v[0] */
    IR_OP_COUNT
} IrOp;

enum { IR_T_F32 = 1, IR_T_I32 = 2 };

typedef u32 IrRef;

typedef struct IrInst {
    u8  op;
    u8  type;
    u8  uniform;    /* değer quad'ın tüm lane'lerinde aynı mı (uniform'lar, sabitler, türevler ve
                     * bunlardan türeyenler); codegen kaydırma/okuma için tek lane kullanabilir */
    u8  live;       /* opt: çıktıya ulaşıyor mu */
    u32 a, b, c;
    u32 imm;
} IrInst;

typedef struct IrFunc {
    IrInst* ins;
    u32     count, cap;
    u32*    htab;       /* CSE karma tablosu (ref'ler) */
    u32     hcap;
    IrRef   out;        /* paketlenmiş renk (I32) */
    IrRef   kill;       /* discard maskesi (I32 tam maske) → ShadeQuad.kill bitleri; 0 = discard yok */
    b32     overflow;
    b32     no_fold;    /* test: sabit katlama + CSE kapalı (optimize öncesi karşılaştırması) */
    u32     folded, cse_hits;
} IrFunc;

void  ir_init(IrFunc* f, Arena* a, u32 cap, b32 no_fold);
IrRef ir_emit(IrFunc* f, IrOp op, u32 type, IrRef a, IrRef b, IrRef c, u32 imm);
IrRef ir_const_f(IrFunc* f, f32 v);
IrRef ir_const_i(IrFunc* f, i32 v);
static inline IrRef ir_op1(IrFunc* f, IrOp op, u32 type, IrRef a) { return ir_emit(f, op, type, a, 0, 0, 0); }
static inline IrRef ir_op2(IrFunc* f, IrOp op, u32 type, IrRef a, IrRef b) { return ir_emit(f, op, type, a, b, 0, 0); }

u32  ir_result_type(IrOp op, u32 operand_type);
const char* ir_op_name(IrOp op);
u32  ir_op_arity(IrOp op);
b32  ir_verify(const IrFunc* f, StrBuf* err);
void ir_print(const IrFunc* f, StrBuf* out);
/* DCE: live bayraklarını doldurur, canlı komut sayısını döndürür */
u32  ir_mark_live(IrFunc* f);

/* Saf lane işlemi (kaynak/doku/türev hariç); sabit katlama ve ir_eval ortak kullanır. */
u32 ir_eval_lane(u32 op, u32 a, u32 b, u32 c);
b32 ir_op_is_pure_lane(u32 op);

#endif
