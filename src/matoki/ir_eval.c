/* matoki/ir_eval.c — IR yorumlayıcısı (4 lane, skaler).
 * Kaynak işlemlerinin (uniform/varying/frag/doku) ve türevlerin semantiği burada tanımlanır;
 * saf lane işlemleri ir_eval_lane'den gelir (sabit katlama ile ortak). Tüm komutlar
 * değerlendirilir (ölü olanlar da); doku okumaları lower tarafından sınırlar içinde tutulur. */
#include "matoki/ir_eval.h"
#include "core/math.h"

void ir_eval(const IrFunc* f, const ShadeQuad* q, const void* uniforms, u32 out[4], Arena* scratch) {
    usize mark = arena_mark(scratch);
    u32 (*v)[4] = (u32(*)[4])arena_alloc_nozero(scratch, sizeof(u32) * 4 * f->count, 16);
    const u8* uni = (const u8*)uniforms;
    for (u32 i = 1; i < f->count; i++) {
        const IrInst* in = &f->ins[i];
        u32* r = v[i];
        switch (in->op) {
        case IR_CONST:
            for (u32 l = 0; l < 4; l++) r[l] = in->imm;
            break;
        case IR_UNIFORM: {
            u32 x = *(const u32*)(uni + in->imm);
            for (u32 l = 0; l < 4; l++) r[l] = x;
            break;
        }
        case IR_VARYING:
            for (u32 l = 0; l < 4; l++) r[l] = m_f32_bits(q->var[in->imm][l]);
            break;
        case IR_FRAG_X:
            for (u32 l = 0; l < 4; l++) r[l] = m_f32_bits((f32)(q->x + (i32)(l & 1)) + 0.5f);
            break;
        case IR_FRAG_Y:
            for (u32 l = 0; l < 4; l++) r[l] = m_f32_bits((f32)(q->y + (i32)(l >> 1)) + 0.5f);
            break;
        case IR_FRAG_Z:
            for (u32 l = 0; l < 4; l++) r[l] = m_f32_bits(q->z[l]);
            break;
        case IR_TEX_FIELD: {
            const u8* tex = *(const u8* const*)(uni + (in->imm & 0xFFFF));
            u32 x = *(const u32*)(tex + (in->imm >> 16));
            for (u32 l = 0; l < 4; l++) r[l] = x;
            break;
        }
        case IR_TEXEL_LOAD: {
            const Texture* tex = *(const Texture* const*)(uni + in->imm);
            for (u32 l = 0; l < 4; l++) r[l] = tex->px[v[in->a][l]];
            break;
        }
        case IR_DERIV_X:
        case IR_DERIV_Y: {
            if (f->ins[in->a].uniform) {
                for (u32 l = 0; l < 4; l++) r[l] = 0;
                break;
            }
            const u32* x = v[in->a];
            u32 k = in->op == IR_DERIV_X ? 1 : 2;
            u32 d = m_f32_bits(m_bits_f32(x[k]) - m_bits_f32(x[0]));
            for (u32 l = 0; l < 4; l++) r[l] = d;
            break;
        }
        default: {
            const u32* a = v[in->a];
            const u32* b = v[in->b];
            const u32* c = v[in->c];
            for (u32 l = 0; l < 4; l++) r[l] = ir_eval_lane(in->op, a[l], b[l], c[l]);
            break;
        }
        }
    }
    for (u32 l = 0; l < 4; l++) out[l] = v[f->out][l];
    arena_reset(scratch, mark);
}

void ir_eval_shade(const ShadeQuad* q, const void* ctx, u32 out[4]) {
    const IrEvalShader* s = (const IrEvalShader*)ctx;
    ir_eval(s->f, q, s->uniforms, out, s->scratch);
}
