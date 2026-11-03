/* matoki/wgsl.c — IR → WGSL metni (matoki/wgsl.h, D42). Komut başına bir satır: `let vN: f32 = ...;`, referanslar da
 * adla: sabit bile olsa işlenen `let` olduğundan WGSL sabit değerlendirmesi (1/0 derleme hatası) devreye girmez.
 * Lane'e özgü semantik donanıma eşlenir: IR_DERIV_X/Y kaba türev, FRAG_X/Y @builtin(position) (zaten piksel + 0,5).
 * Maskeler I32 (tüm bitler 1 / 0) kalır, F32 select tam maske sözleşmesiyle (ir.h) WGSL select olur.
 * Quad-sabit maskeli kollar `if` içinde (ir_guard, D43): kol değerleri işlev başında `var`, türev içeren kol dallanmaz. */
#include "matoki/wgsl.h"
#include "matoki/sema.h"
#include "core/mem.h"
#include "matoki/ir_guard.h"
#include "matoki/matoki.h"

#define WGSL_LINE_BYTES 176u   /* komut başına üst sınır: en uzun satır + var bildirimi + if / else */
#define WGSL_HEAD_BYTES 2048u

static void put_hex(StrBuf* s, u32 v, u32 digits) {
    static const char H[] = "0123456789ABCDEF";
    for (u32 i = digits; i-- > 0;) sb_char(s, H[(v >> (i * 4)) & 15u]);
}

b32 wgsl_f32_literal(StrBuf* s, u32 bits) {
    u32 e = (bits >> 23) & 0xFFu, m = bits & 0x7FFFFFu;
    if (e == 0xFFu) return FALSE;
    if (bits >> 31) sb_char(s, '-');
    if (!e && !m) {
        sb_cstr(s, "0.0f");
        return TRUE;
    }
    sb_cstr(s, e ? "0x1." : "0x0.");
    put_hex(s, m << 1, 6);   /* 23 bit → 24 bit = 6 onaltılık */
    sb_char(s, 'p');
    i32 x = e ? (i32)e - 127 : -126;
    if (x >= 0) sb_char(s, '+');
    sb_i64(s, x);
    sb_char(s, 'f');
    return TRUE;
}

static void put_ref(StrBuf* s, IrRef r) {
    sb_char(s, 'v');
    sb_u64(s, r);
}

static void put2(StrBuf* s, IrRef a, const char* op, IrRef b) {
    put_ref(s, a);
    sb_cstr(s, op);
    put_ref(s, b);
}

static void put_call1(StrBuf* s, const char* fn, IrRef a) {
    sb_cstr(s, fn);
    sb_char(s, '(');
    put_ref(s, a);
    sb_char(s, ')');
}

static void put_call2(StrBuf* s, const char* fn, IrRef a, IrRef b) {
    sb_cstr(s, fn);
    sb_char(s, '(');
    put2(s, a, ", ", b);
    sb_char(s, ')');
}

static void put_mask(StrBuf* s, IrRef a, const char* op, IrRef b) {
    sb_cstr(s, "mask(");
    put2(s, a, op, b);
    sb_char(s, ')');
}

/* (a << / >> (b & 31)); kaydırma miktarı WGSL'de u32 */
static void put_shift(StrBuf* s, IrRef a, const char* op, IrRef b) {
    put_ref(s, a);
    sb_cstr(s, op);
    sb_cstr(s, "(bitcast<u32>(");
    put_ref(s, b);
    sb_cstr(s, ") & 31u)");
}

static const char SWZ[] = "xyzw";

/* uniform bloğundaki tek sözcük: u[ofs / 16][(ofs % 16) / 4] */
static void put_uniform(StrBuf* s, u32 ofs) {
    sb_cstr(s, "u[");
    sb_u64(s, ofs / 16u);
    sb_cstr(s, "][");
    sb_u64(s, (ofs % 16u) / 4u);
    sb_char(s, ']');
}

static i32 tex_binding(const MatokiWgsl* w, u32 ofs) {
    for (u32 k = 0; k < w->tex_count; k++) {
        if (w->tex_ofs[k] == ofs) return (i32)k;
    }
    return -1;
}

static void put_varying(StrBuf* s, u32 slot) {
    sb_cstr(s, "in.v");
    sb_u64(s, slot / 4u);
    sb_char(s, '.');
    sb_char(s, SWZ[slot % 4u]);
}

/* bir komutun ifadesi; FALSE: bu arka uçta karşılığı yok */
static b32 put_expr(StrBuf* s, const MatokiWgsl* w, const IrInst* in, u32 stage) {
    switch (in->op) {
    case IR_UNIFORM:
        if (in->type == IR_T_F32) {
            sb_cstr(s, "bitcast<f32>(");
            put_uniform(s, in->imm);
            sb_char(s, ')');
        } else {
            sb_cstr(s, "bitcast<i32>(");
            put_uniform(s, in->imm);
            sb_char(s, ')');
        }
        return TRUE;
    case IR_VARYING:
        put_varying(s, in->imm);
        return TRUE;
    case IR_ATTR:
        sb_char(s, 'a');
        sb_u64(s, in->imm);
        return TRUE;
    case IR_FRAG_X: sb_cstr(s, "in.pos.x"); return TRUE;
    case IR_FRAG_Y: sb_cstr(s, "in.pos.y"); return TRUE;
    case IR_FRAG_Z: sb_cstr(s, "in.pos.z"); return TRUE;
    case IR_TEX_FIELD: {
        sb_cstr(s, "bitcast<i32>(t");
        sb_i64(s, tex_binding(w, in->imm & 0xFFFFu));
        sb_char(s, '[');
        sb_u64(s, (in->imm >> 16) / 4u);
        sb_cstr(s, "])");
        return TRUE;
    }
    case IR_TEXEL_LOAD:
        sb_cstr(s, "bitcast<i32>(t");
        sb_i64(s, tex_binding(w, in->imm));
        sb_char(s, '[');
        sb_u64(s, WGSL_TEX_HEADER);
        sb_cstr(s, "i + ");
        put_ref(s, in->a);
        sb_cstr(s, "])");
        return TRUE;
    case IR_TEX_WORD:
        sb_cstr(s, "bitcast<i32>(t");
        sb_i64(s, tex_binding(w, in->imm));
        sb_char(s, '[');
        put_ref(s, in->a);
        sb_cstr(s, "])");
        return TRUE;
    case IR_HW_SAMPLE:
        sb_cstr(s, "textureSampleLevel(h");
        sb_i64(s, tex_binding(w, in->imm & 0xFFFFu));
        sb_cstr(s, in->imm & (1u << 18) ? ", sc, vec2f(" : ", sw, vec2f(");
        put_ref(s, in->a);
        sb_cstr(s, ", ");
        put_ref(s, in->b);
        sb_cstr(s, "), f32(");
        put_ref(s, in->c);
        sb_cstr(s, ")).");
        sb_char(s, "rgba"[(in->imm >> 16) & 3u]);
        return TRUE;
    case IR_FADD: case IR_IADD: put2(s, in->a, " + ", in->b); return TRUE;
    case IR_FSUB: case IR_ISUB: put2(s, in->a, " - ", in->b); return TRUE;
    case IR_FMUL: case IR_IMUL: put2(s, in->a, " * ", in->b); return TRUE;
    case IR_FDIV: put2(s, in->a, " / ", in->b); return TRUE;
    case IR_FMIN: case IR_IMIN: put_call2(s, "min", in->a, in->b); return TRUE;
    case IR_FMAX: case IR_IMAX: put_call2(s, "max", in->a, in->b); return TRUE;
    case IR_FNEG: case IR_INEG: sb_char(s, '-'); put_ref(s, in->a); return TRUE;
    case IR_FABS: put_call1(s, "abs", in->a); return TRUE;
    case IR_FSQRT: put_call1(s, "sqrt", in->a); return TRUE;
    case IR_FFLOOR: put_call1(s, "floor", in->a); return TRUE;
    case IR_FCEIL: put_call1(s, "ceil", in->a); return TRUE;
    case IR_FTRUNC: put_call1(s, "trunc", in->a); return TRUE;
    case IR_FNEAREST: put_call1(s, "round", in->a); return TRUE;   /* WGSL round: yarıda çifte, wasm nearest gibi */
    case IR_FEQ: case IR_IEQ: put_mask(s, in->a, " == ", in->b); return TRUE;
    case IR_FNE: case IR_INE: put_mask(s, in->a, " != ", in->b); return TRUE;
    case IR_FLT: case IR_ILT: put_mask(s, in->a, " < ", in->b); return TRUE;
    case IR_FLE: case IR_ILE: put_mask(s, in->a, " <= ", in->b); return TRUE;
    case IR_FGT: case IR_IGT: put_mask(s, in->a, " > ", in->b); return TRUE;
    case IR_FGE: case IR_IGE: put_mask(s, in->a, " >= ", in->b); return TRUE;
    case IR_IAND: put2(s, in->a, " & ", in->b); return TRUE;
    case IR_IOR: put2(s, in->a, " | ", in->b); return TRUE;
    case IR_IXOR: put2(s, in->a, " ^ ", in->b); return TRUE;
    case IR_ISHL: put_shift(s, in->a, " << ", in->b); return TRUE;
    case IR_ISHR: put_shift(s, in->a, " >> ", in->b); return TRUE;
    case IR_ISHRU:
        sb_cstr(s, "bitcast<i32>(bitcast<u32>(");
        put_ref(s, in->a);
        sb_cstr(s, ") >> (bitcast<u32>(");
        put_ref(s, in->b);
        sb_cstr(s, ") & 31u))");
        return TRUE;
    case IR_SELECT:
        if (in->type == IR_T_F32) {
            sb_cstr(s, "select(");
            put2(s, in->c, ", ", in->b);
            sb_cstr(s, ", ");
            put_ref(s, in->a);
            sb_cstr(s, " != 0i)");
        } else {
            sb_char(s, '(');
            put2(s, in->b, " & ", in->a);
            sb_cstr(s, ") | (");
            put2(s, in->c, " & ~", in->a);
            sb_char(s, ')');
        }
        return TRUE;
    case IR_F2I: put_call1(s, "i32", in->a); return TRUE;
    case IR_I2F: put_call1(s, "f32", in->a); return TRUE;
    case IR_BITCAST_F2I: put_call1(s, "bitcast<i32>", in->a); return TRUE;
    case IR_BITCAST_I2F: put_call1(s, "bitcast<f32>", in->a); return TRUE;
    case IR_DERIV_X: put_call1(s, "dpdxCoarse", in->a); return stage == MATOKI_STAGE_FRAG;
    case IR_DERIV_Y: put_call1(s, "dpdyCoarse", in->a); return stage == MATOKI_STAGE_FRAG;
    default:
        return FALSE;
    }
}

static void put_const(StrBuf* s, const IrInst* in, u32 i) {
    if (in->type == IR_T_I32) {
        sb_cstr(s, "bitcast<i32>(0x");
        put_hex(s, in->imm, 8);
        sb_cstr(s, "u)");
        return;
    }
    if (!wgsl_f32_literal(s, in->imm)) {
        /* inf / NaN sabit ifadede yasak: `let` çalışma zamanı değeridir (log2'nin 0 ve NaN dalları) */
        sb_cstr(s, "bitcast<f32>(k");
        sb_u64(s, i);
        sb_char(s, ')');
    }
}

static b32 wgsl_fail(MatokiError* err, const char* msg) {
    matoki_error(err, 1, 1, msg, NULL, 0);
    return FALSE;
}

static void put_inst(StrBuf* s, const IrFunc* f, const MatokiWgsl* w, u32 stage, u32 i, b32 is_var, b32* ok) {
    const IrInst* in = &f->ins[i];
    if (in->op == IR_CONST && in->type == IR_T_F32 && ((in->imm >> 23) & 0xFFu) == 0xFFu) {
        sb_cstr(s, "  let k");
        sb_u64(s, i);
        sb_cstr(s, ": u32 = 0x");
        put_hex(s, in->imm, 8);
        sb_cstr(s, "u;\n");
    }
    if (is_var) {
        sb_cstr(s, "  v");
        sb_u64(s, i);
        sb_cstr(s, " = ");
    } else {
        sb_cstr(s, "  let v");
        sb_u64(s, i);
        sb_cstr(s, in->type == IR_T_F32 ? ": f32 = " : ": i32 = ");
    }
    if (in->op == IR_CONST) put_const(s, in, i);
    else if (!put_expr(s, w, in, stage)) *ok = FALSE;
    sb_cstr(s, ";\n");
}

static void put_node(StrBuf* s, const IrFunc* f, const MatokiWgsl* w, const IrGuards* g, u32 stage, u32 node, b32* ok) {
    for (u32 i = g->nodes[node].head; i; i = g->next[i]) {
        if (g->group_at[i]) {
            u32 gr = g->group_at[i] - 1u;
            if (g->branch[gr]) {
                sb_cstr(s, "  if (");
                put_ref(s, g->mask[gr]);
                sb_cstr(s, " != 0i) {\n");
            }
            put_node(s, f, w, g, stage, ir_guard_true(gr), ok);
            if (g->branch[gr]) sb_cstr(s, "  } else {\n");
            put_node(s, f, w, g, stage, ir_guard_false(gr), ok);
            if (g->branch[gr]) sb_cstr(s, "  }\n");
        }
        put_inst(s, f, w, stage, i, node != 0, ok);
    }
}

/* işlev gövdesi: kola düşen değerler işlev kapsamında `var` (select kolun dışında okur), kalanı `let` */
static void put_body(StrBuf* s, const IrFunc* f, const MatokiWgsl* w, const IrGuards* g, u32 stage, b32* ok) {
    for (u32 i = 1; i < f->count; i++) {
        if (!f->ins[i].live || !g->node[i]) continue;
        sb_cstr(s, "  var v");
        sb_u64(s, i);
        sb_cstr(s, f->ins[i].type == IR_T_F32 ? ": f32;\n" : ": i32;\n");
    }
    put_node(s, f, w, g, stage, 0, ok);
}

static const char VOUT_DECL[] =
    "struct VOut {\n  @builtin(position) pos: vec4f,\n  @location(0) v0: vec4f,\n  @location(1) v1: vec4f,\n};\n";

b32 matoki_wgsl(Arena* a, const MatokiProgram* p, MatokiWgsl* out, MatokiError* err) {
    /* kaynaktan derlenmiş programda IR ayrıca indirilir: bilinear okuma donanıma gider (D50). JIT IR'ı yazılımın kâhini olarak kalır */
    IrFunc gpu_ir;
    const IrFunc* f = &p->ir;
    usize mark0 = arena_mark(a);
    mem_zero(err, sizeof(*err));
    if (p->sema.frag || p->sema.vert) {
        ir_init(&gpu_ir, a, p->ir.count + 1024u, FALSE);
        MatokiProgram* mp = (MatokiProgram*)p;
        if (!lower_module_gpu(a, &mp->module, &p->sema, p->stage, &gpu_ir, err)) return FALSE;
        ir_mark_live(&gpu_ir);
        f = &gpu_ir;
    }
    b32 vert = p->stage == MATOKI_STAGE_VERT;
    mem_zero(out, sizeof(*out));
    mem_zero(err, sizeof(*err));
    out->uniform_size = p->sema.uniform_size;
    if (out->uniform_size > WGSL_MAX_UNIFORM) return wgsl_fail(err, "gpu: uniform bloğu 256 baytı aşıyor");
    if (p->sema.varying_slots > WGSL_MAX_VARYINGS) return wgsl_fail(err, "gpu: varying yuvası 8'i aşıyor");
    if (p->sema.attr_slots > WGSL_MAX_ATTRS) return wgsl_fail(err, "gpu: köşe girdisi 8'i aşıyor");
    /* canlı IR'ın okuduğu dokular → binding sırası */
    for (u32 i = 1; i < f->count; i++) {
        const IrInst* in = &f->ins[i];
        if (!in->live) continue;
        if (in->op != IR_TEX_FIELD && in->op != IR_TEXEL_LOAD && in->op != IR_TEX_WORD && in->op != IR_HW_SAMPLE) continue;
        if (vert) return wgsl_fail(err, "gpu: köşe aşamasında doku yok");
        u32 ofs = in->op == IR_TEX_FIELD || in->op == IR_HW_SAMPLE ? (in->imm & 0xFFFFu) : in->imm;
        if (tex_binding(out, ofs) >= 0) continue;
        if (out->tex_count == WGSL_MAX_TEX) return wgsl_fail(err, "gpu: parça başına en çok 3 doku");
        out->tex_ofs[out->tex_count++] = ofs;
    }
    u32 live = 0;
    for (u32 i = 1; i < f->count; i++) live += f->ins[i].live ? 1u : 0u;
    u32 cap = WGSL_HEAD_BYTES + live * WGSL_LINE_BYTES;
    usize mark = arena_mark(a);
    char* buf = (char*)arena_try(a, cap, 1);
    if (!buf) return wgsl_fail(err, "gpu: WGSL metni için bellek yok");
    IrGuards guards;
    ir_guard_build(a, f, NULL, TRUE, &guards);
    StrBuf sb;
    StrBuf* s = &sb;
    sb_init(s, buf, cap);
    sb_cstr(s, VOUT_DECL);
    sb_cstr(s, vert ? "@group(0) @binding(0)" : "@group(0) @binding(1)");
    sb_cstr(s, " var<uniform> u: array<vec4u, 16>;\n");
    for (u32 k = 0; k < out->tex_count; k++) {
        sb_cstr(s, "@group(1) @binding(");
        sb_u64(s, k);
        sb_cstr(s, ") var<storage, read> t");
        sb_u64(s, k);
        sb_cstr(s, ": array<u32>;\n");
    }
    if (!vert) {
        /* bilinear okuma için aynı yuvanın donanım dokusu + sabit örnekleyiciler (web/gpu.mjs bağlar, D50) */
        for (u32 k = 0; k < out->tex_count; k++) {
            sb_cstr(s, "@group(1) @binding(");
            sb_u64(s, k + WGSL_MAX_TEX);
            sb_cstr(s, ") var h");
            sb_u64(s, k);
            sb_cstr(s, ": texture_2d<f32>;\n");
        }
        sb_cstr(s, "@group(1) @binding(6) var sw: sampler;\n@group(1) @binding(7) var sc: sampler;\n");
    }
    sb_cstr(s, "fn mask(b: bool) -> i32 { return select(0i, -1i, b); }\n");
    b32 ok = TRUE;
    if (vert) {
        sb_cstr(s, "@vertex fn vs(");
        for (u32 k = 0; k < WGSL_MAX_ATTRS; k++) {
            if (k) sb_cstr(s, ", ");
            sb_cstr(s, "@location(");
            sb_u64(s, k);
            sb_cstr(s, ") a");
            sb_u64(s, k);
            sb_cstr(s, ": f32");
        }
        sb_cstr(s, ") -> VOut {\n");
        put_body(s, f, out, &guards, p->stage, &ok);
        sb_cstr(s, "  var o: VOut;\n");
        sb_cstr(s, "  o.pos.x = ");
        put_ref(s, f->out);
        sb_cstr(s, ";\n");
        for (u32 k = 0; k < f->nxout; k++) {
            u32 ofs = f->xout_ofs[k];
            if (ofs < VOUT_VAR_OFS) {
                sb_cstr(s, "  o.pos.");
                sb_char(s, SWZ[(ofs - VOUT_POS_OFS) / 16u]);
            } else {
                u32 slot = (ofs - VOUT_VAR_OFS) / 16u;
                sb_cstr(s, "  o.v");
                sb_u64(s, slot / 4u);
                sb_char(s, '.');
                sb_char(s, SWZ[slot % 4u]);
            }
            sb_cstr(s, " = ");
            put_ref(s, f->xout[k]);
            sb_cstr(s, ";\n");
        }
        sb_cstr(s, "  return o;\n}\n");
    } else {
        sb_cstr(s, "@fragment fn fs(in: VOut) -> @location(0) vec4f {\n");
        put_body(s, f, out, &guards, p->stage, &ok);
        if (f->kill) {
            sb_cstr(s, "  if (");
            put_ref(s, f->kill);
            sb_cstr(s, " != 0i) { discard; }\n");
        }
        /* paketlenmiş renk: JIT ile aynı tamsayı; bayt / 255 → unorm hedefte aynı bayt */
        sb_cstr(s, "  let c = bitcast<u32>(");
        put_ref(s, f->out);
        sb_cstr(s, ");\n  return vec4f(f32(c & 255u), f32((c >> 8u) & 255u), f32((c >> 16u) & 255u), f32(c >> 24u)) / 255.0f;\n}\n");
    }
    if (!ok) return wgsl_fail(err, "gpu: IR komutunun WGSL karşılığı yok");
    if (s->len >= cap) return wgsl_fail(err, "iç hata: WGSL tamponu taştı");
    /* metin arenanın başına: GPU IR'ı, üst sınırın kullanılmayanı ve dal planı geri (ileri kopya: hedef kaynağın önünde) */
    char* text = buf - (mark - mark0);
    for (u32 i = 0; i < s->len; i++) text[i] = buf[i];
    arena_reset(a, mark0 + s->len);
    out->text = text;
    out->len = s->len;
    return TRUE;
}
