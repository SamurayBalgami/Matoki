/* tests/test_matoki_back.c — arka uç diferansiyel testi: ir_eval == codegen (JIT, node'da).
 * 500 rastgele IR programı; her biri katlama/CSE açık ve kapalı iki kez kurulur → 6 sonuç
 * (eval / jit shade / jit prelude+shade_fast × fold/nofold) tüm rastgele quad'larda bit-bit aynı olmalı. NaN yükü wasm'da
 * belirsiz olduğundan float→int bitcast öncesi NaN kanonikleştirilir (x == x ? x : 0). */
#include "test.h"
#include "core/math.h"
#include "core/rng.h"
#include "matoki/codegen.h"
#include "matoki/ir_eval.h"
#include "jit/jit.h"

#define UNI_F_COUNT 8
#define UNI_I_OFS   32
#define UNI_I_COUNT 4
#define UNI_TEX_OFS 48
#define UNI_SIZE    64

static const u32 SPECIAL_F[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0x00000001u, 0x80800000u, 0x7F7FFFFFu, 0x4B000000u, 0x4B000001u, 0x3EAAAAABu,
    0x437F0000u, 0x40490FDBu, 0xC2C80000u, 0x3F7FFFFFu, 0x3F800001u, 0x4EFFFFFFu, 0xCF000000u,
};
static const i32 SPECIAL_I[] = { 0, 1, -1, 2, 7, 8, 16, 24, 31, 32, 33, 255, 0x7FFFFFFF, -2147483647 - 1, 0x12345678, -100 };

static u32 rand_f_bits(Rng* r) {
    u32 k = rng_u32(r) % 4;
    if (k == 0) return SPECIAL_F[rng_u32(r) % ARRAY_COUNT(SPECIAL_F)];
    if (k == 1) return rng_u32(r);   /* rastgele bitler (NaN/denormal dahil) */
    return m_f32_bits(rng_range(r, -300.0f, 300.0f));
}

static i32 rand_i(Rng* r) {
    if (rng_u32(r) & 1) return SPECIAL_I[rng_u32(r) % ARRAY_COUNT(SPECIAL_I)];
    return (i32)rng_u32(r);
}

typedef struct Pools {
    IrRef f[1024];
    u32   nf;
    IrRef i[1024];
    u32   ni;
    u32   first_f, first_i;   /* döngüde üretilenlerin başlangıcı */
} Pools;

static IrRef pf(Rng* r, const Pools* p) { return p->f[rng_u32(r) % p->nf]; }
static IrRef pi(Rng* r, const Pools* p) { return p->i[rng_u32(r) % p->ni]; }
static void push_f(Pools* p, IrRef x) { if (x && p->nf < 1024) p->f[p->nf++] = x; }
static void push_i(Pools* p, IrRef x) { if (x && p->ni < 1024) p->i[p->ni++] = x; }

static IrRef canon(IrFunc* f, IrRef x) {
    IrRef eq = ir_emit(f, IR_FEQ, IR_T_I32, x, x, 0, 0);
    return ir_emit(f, IR_SELECT, IR_T_F32, eq, x, ir_const_f(f, 0.0f), 0);
}

static void build_random(IrFunc* f, u64 seed) {
    Rng r = rng_make(seed);
    Pools p;
    p.nf = p.ni = 0;
    for (u32 s = 0; s < 6; s++) push_f(&p, ir_emit(f, IR_VARYING, IR_T_F32, 0, 0, 0, s));
    for (u32 k = 0; k < UNI_F_COUNT; k++) push_f(&p, ir_emit(f, IR_UNIFORM, IR_T_F32, 0, 0, 0, k * 4));
    for (u32 k = 0; k < UNI_I_COUNT; k++) push_i(&p, ir_emit(f, IR_UNIFORM, IR_T_I32, 0, 0, 0, UNI_I_OFS + k * 4));
    for (u32 k = 0; k < 4; k++) push_f(&p, ir_const_f(f, m_bits_f32(rand_f_bits(&r))));
    for (u32 k = 0; k < 4; k++) push_i(&p, ir_const_i(f, rand_i(&r)));
    push_f(&p, ir_emit(f, IR_FRAG_X, IR_T_F32, 0, 0, 0, 0));
    push_f(&p, ir_emit(f, IR_FRAG_Y, IR_T_F32, 0, 0, 0, 0));
    push_f(&p, ir_emit(f, IR_FRAG_Z, IR_T_F32, 0, 0, 0, 0));
    push_i(&p, ir_emit(f, IR_TEX_FIELD, IR_T_I32, 0, 0, 0, UNI_TEX_OFS | (TEX_OFF_W << 16)));
    push_i(&p, ir_emit(f, IR_TEX_FIELD, IR_T_I32, 0, 0, 0, UNI_TEX_OFS | (TEX_OFF_WMASK << 16)));
    p.first_f = p.nf;
    p.first_i = p.ni;

    u32 steps = 20 + rng_u32(&r) % 110;
    for (u32 st = 0; st < steps; st++) {
        u32 kind = rng_u32(&r) % 14;
        switch (kind) {
        case 0: case 1: {   /* f ikili */
            IrOp op = (IrOp)(IR_FADD + rng_u32(&r) % (IR_FMAX - IR_FADD + 1));
            push_f(&p, ir_emit(f, op, IR_T_F32, pf(&r, &p), pf(&r, &p), 0, 0));
            break;
        }
        case 2: {           /* f tekli */
            IrOp op = (IrOp)(IR_FNEG + rng_u32(&r) % (IR_FNEAREST - IR_FNEG + 1));
            push_f(&p, ir_emit(f, op, IR_T_F32, pf(&r, &p), 0, 0, 0));
            break;
        }
        case 3: {           /* f karşılaştırma */
            IrOp op = (IrOp)(IR_FEQ + rng_u32(&r) % (IR_FGE - IR_FEQ + 1));
            push_i(&p, ir_emit(f, op, IR_T_I32, pf(&r, &p), pf(&r, &p), 0, 0));
            break;
        }
        case 4: case 5: {   /* i ikili (kaydırmalar dahil) */
            static const IrOp OPS[] = { IR_IADD, IR_ISUB, IR_IMUL, IR_IAND, IR_IOR, IR_IXOR, IR_ISHL, IR_ISHR, IR_ISHRU, IR_IMIN, IR_IMAX };
            IrOp op = OPS[rng_u32(&r) % ARRAY_COUNT(OPS)];
            push_i(&p, ir_emit(f, op, IR_T_I32, pi(&r, &p), pi(&r, &p), 0, 0));
            break;
        }
        case 6: {           /* i karşılaştırma / neg */
            if (rng_u32(&r) % 4 == 0) {
                push_i(&p, ir_emit(f, IR_INEG, IR_T_I32, pi(&r, &p), 0, 0, 0));
            } else {
                IrOp op = (IrOp)(IR_IEQ + rng_u32(&r) % (IR_IGE - IR_IEQ + 1));
                push_i(&p, ir_emit(f, op, IR_T_I32, pi(&r, &p), pi(&r, &p), 0, 0));
            }
            break;
        }
        case 7:             /* select: i32'de maske herhangi bir değer (bit düzeyinde); f32'de tam maske,
                             * çünkü kısmi maske wasm'ın belirsiz NaN işaret/yük bitlerini sızdırır (ir.h) */
            if (rng_u32(&r) & 1) {
                IrRef m = ir_emit(f, IR_INE, IR_T_I32, pi(&r, &p), ir_const_i(f, 0), 0, 0);
                push_f(&p, ir_emit(f, IR_SELECT, IR_T_F32, m, pf(&r, &p), pf(&r, &p), 0));
            } else {
                push_i(&p, ir_emit(f, IR_SELECT, IR_T_I32, pi(&r, &p), pi(&r, &p), pi(&r, &p), 0));
            }
            break;
        case 8:             /* dönüşümler */
            if (rng_u32(&r) & 1) push_i(&p, ir_emit(f, IR_F2I, IR_T_I32, pf(&r, &p), 0, 0, 0));
            else push_f(&p, ir_emit(f, IR_I2F, IR_T_F32, pi(&r, &p), 0, 0, 0));
            break;
        case 9:             /* bitcast'ler */
            if (rng_u32(&r) & 1) push_f(&p, ir_emit(f, IR_BITCAST_I2F, IR_T_F32, pi(&r, &p), 0, 0, 0));
            else push_i(&p, ir_emit(f, IR_BITCAST_F2I, IR_T_I32, canon(f, pf(&r, &p)), 0, 0, 0));
            break;
        case 10: case 11:   /* türevler */
            push_f(&p, ir_emit(f, (rng_u32(&r) & 1) ? IR_DERIV_X : IR_DERIV_Y, IR_T_F32, pf(&r, &p), 0, 0, 0));
            break;
        case 12: {          /* doku sözcüğü (mip ofset tablosu, sözcük 8..23) */
            IrRef w = ir_emit(f, IR_IAND, IR_T_I32, pi(&r, &p), ir_const_i(f, 15), 0, 0);
            w = ir_emit(f, IR_IADD, IR_T_I32, w, ir_const_i(f, TEX_WORD_LVOFS), 0, 0);
            push_i(&p, ir_emit(f, IR_TEX_WORD, IR_T_I32, w, 0, 0, UNI_TEX_OFS));
            break;
        }
        default: {          /* doku okuma (indeks 0..255; indeks quad-sabitse tek okuma yolu) */
            IrRef idx = ir_emit(f, IR_IAND, IR_T_I32, pi(&r, &p), ir_const_i(f, 255), 0, 0);
            push_i(&p, ir_emit(f, IR_TEXEL_LOAD, IR_T_I32, idx, 0, 0, UNI_TEX_OFS));
            break;
        }
        }
    }
    /* çıktı: üretilen her değerin (kanonik) bitlerinin XOR'u → hepsi canlı */
    IrRef acc = ir_const_i(f, 0x5A5A5A5A);
    for (u32 k = p.first_i; k < p.ni; k++) acc = ir_emit(f, IR_IXOR, IR_T_I32, acc, p.i[k], 0, 0);
    for (u32 k = p.first_f; k < p.nf; k++) {
        IrRef b = ir_emit(f, IR_BITCAST_F2I, IR_T_I32, canon(f, p.f[k]), 0, 0, 0);
        acc = ir_emit(f, IR_IXOR, IR_T_I32, ir_emit(f, IR_ISHL, IR_T_I32, acc, ir_const_i(f, 1), 0, 0), b, 0, 0);
    }
    f->out = acc;
}

static void random_quad(Rng* r, ShadeQuad* q) {
    q->x = (i32)(rng_u32(r) % 4096) - 1024;
    q->y = (i32)(rng_u32(r) % 4096) - 1024;
    q->mask = 0xF;
    q->kill = 0;
    for (u32 l = 0; l < 4; l++) {
        q->z[l] = m_bits_f32(rand_f_bits(r));
        for (u32 s = 0; s < SHADE_MAX_VARS; s++) q->var[s][l] = m_bits_f32(rand_f_bits(r));
    }
    /* bazen lane'ler eşit (uniform benzeri varying → türev 0) */
    if (rng_u32(r) % 4 == 0) {
        for (u32 s = 0; s < SHADE_MAX_VARS; s++) q->var[s][1] = q->var[s][2] = q->var[s][3] = q->var[s][0];
    }
}

/* Teşhis: ilk ayrışan komutu bul (her komutu sırayla çıktı yapıp eval ile jit'i karşılaştır). */
static void diagnose(IrFunc* f, Arena* a, const ShadeQuad* q, const void* uni, Arena* scratch) {
    IrRef saved = f->out;
    u32 saved_count = f->count;
    for (u32 i = 1; i < saved_count; i++) {
        f->count = saved_count;
        IrRef v = i;
        if (f->ins[i].type == IR_T_F32) v = ir_emit(f, IR_BITCAST_F2I, IR_T_I32, canon(f, i), 0, 0, 0);
        f->out = v;
        WasmModule wm;
        ShadeFn fn = codegen_wasm(f, a, &wm) ? jit_load_shade(wm.bytes, wm.len) : NULL;
        if (!fn) continue;
        u32 e[4], j[4];
        ir_eval(f, q, uni, e, scratch);
        fn(q, uni, j);
        if (e[0] != j[0] || e[1] != j[1] || e[2] != j[2] || e[3] != j[3]) {
            const IrInst* in = &f->ins[i];
            char buf[400];
            StrBuf sb;
            sb_init(&sb, buf, sizeof(buf));
            sb_cstr(&sb, "    ilk ayrışan: %");
            sb_u64(&sb, i);
            sb_char(&sb, ' ');
            sb_cstr(&sb, ir_op_name((IrOp)in->op));
            sb_cstr(&sb, in->uniform ? " (uniform)" : " (varying)");
            u32 ar = ir_op_arity((IrOp)in->op);
            const IrRef ops[3] = { in->a, in->b, in->c };
            for (u32 k = 0; k < ar; k++) {
                sb_cstr(&sb, " %");
                sb_u64(&sb, ops[k]);
                sb_char(&sb, '[');
                sb_cstr(&sb, ir_op_name((IrOp)f->ins[ops[k]].op));
                sb_cstr(&sb, f->ins[ops[k]].uniform ? ",u]" : ",v]");
            }
            sb_cstr(&sb, "  eval=");
            for (u32 l = 0; l < 4; l++) { sb_hex32(&sb, e[l]); sb_char(&sb, ' '); }
            sb_cstr(&sb, " jit=");
            for (u32 l = 0; l < 4; l++) { sb_hex32(&sb, j[l]); sb_char(&sb, ' '); }
            log_sb(&sb);
            /* işlenen değerleri (eval) */
            for (u32 k = 0; k < ar; k++) {
                f->count = saved_count;
                f->out = ops[k];
                if (f->ins[ops[k]].type == IR_T_F32) f->out = ir_emit(f, IR_BITCAST_F2I, IR_T_I32, ops[k], 0, 0, 0);
                ir_eval(f, q, uni, e, scratch);
                sb_init(&sb, buf, sizeof(buf));
                sb_cstr(&sb, "      işlenen ");
                sb_u64(&sb, k);
                sb_cstr(&sb, ": ");
                for (u32 l = 0; l < 4; l++) { sb_hex32(&sb, e[l]); sb_char(&sb, ' '); }
                log_sb(&sb);
            }
            break;
        }
    }
    f->count = saved_count;
    f->out = saved;
}

TEST(matoki_diff_eval_vs_codegen_500) {
    Arena* a = test_arena();
    Texture* tex = ARENA_NEW(a, Texture);
    tex->px = ARENA_ARRAY(a, u32, 256);
    tex->w = tex->h = 16;
    tex->w_mask = tex->h_mask = 15;
    tex->levels = 1;
    Rng tr = rng_make(99);
    for (u32 i = 0; i < 256; i++) tex->px[i] = rng_u32(&tr);
    for (u32 i = 0; i < TEX_MAX_LEVELS; i++) tex->lv_ofs[i] = rng_u32(&tr);
    u8* uni = (u8*)arena_alloc(a, UNI_SIZE, 16);
    ShadeQuad* quads = ARENA_ARRAY(a, ShadeQuad, 16);
    Arena scratch;
    arena_init(&scratch, arena_alloc(a, 1 << 20, 16), 1 << 20);
    usize mark = arena_mark(a);

    u32 programs = 0, mismatches = 0, jit_fail = 0, live_total = 0, folded = 0, ops_total = 0, hoisted = 0;
    for (u32 prog = 0; prog < 500; prog++) {
        arena_reset(a, mark);
        Rng r = rng_make(0xD1FF0000ull + prog);
        for (u32 k = 0; k < UNI_F_COUNT; k++) *(u32*)(uni + k * 4) = rand_f_bits(&r);
        for (u32 k = 0; k < UNI_I_COUNT; k++) *(i32*)(uni + UNI_I_OFS + k * 4) = rand_i(&r);
        *(Texture**)(uni + UNI_TEX_OFS) = tex;
        for (u32 k = 0; k < 16; k++) random_quad(&r, &quads[k]);

        IrFunc fn[2];
        JitShader jit[2];
        b32 ok[2];
        for (u32 v = 0; v < 2; v++) {
            ir_init(&fn[v], a, 8192, v == 1);
            build_random(&fn[v], 0xBEEF0000ull + prog);
            WasmModule wm;
            ok[v] = codegen_wasm(&fn[v], a, &wm) && jit_load(wm.bytes, wm.len, TRUE, &jit[v]);
            if (!ok[v]) jit_fail++;
            else hoisted += wm.hoisted;
            if (v == 0) {
                live_total += wm.live_insts;
                folded += fn[v].folded + fn[v].cse_hits;
            } else {
                ops_total += fn[v].count - 1;
            }
        }
        if (!ok[0] || !ok[1]) continue;
        /* hızlı yol: önsöz bu uniform'larla bir kez, sonra tüm quad'lar */
        jit[0].prelude(uni);
        jit[1].prelude(uni);
        for (u32 k = 0; k < 16; k++) {
            u32 o[6][4];
            ir_eval(&fn[0], &quads[k], uni, o[0], &scratch);
            ir_eval(&fn[1], &quads[k], uni, o[1], &scratch);
            jit[0].shade(&quads[k], uni, o[2]);
            jit[1].shade(&quads[k], uni, o[3]);
            jit[0].fast(&quads[k], uni, o[4]);
            jit[1].fast(&quads[k], uni, o[5]);
            for (u32 l = 0; l < 4; l++) {
                if (o[1][l] != o[0][l] || o[2][l] != o[0][l] || o[3][l] != o[0][l] || o[4][l] != o[0][l] || o[5][l] != o[0][l]) {
                    if (mismatches < 2) {
                        test_note("uyumsuz program", prog, 0);
                        diagnose(&fn[1], a, &quads[k], uni, &scratch);
                    }
                    mismatches++;
                    break;
                }
            }
        }
        programs++;
    }
    test_note("program", programs, 0);
    test_note("komut (katlamasız, toplam)", ops_total, 0);
    test_note("canlı komut (katlamalı, toplam)", live_total, 0);
    test_note("katlanan + CSE isabeti", folded, 0);
    test_note("önsöze alınan (toplam, iki varyant)", hoisted, 0);
    CHECK(jit_fail == 0);
    CHECK(programs == 500);
    CHECK(mismatches == 0);
}
