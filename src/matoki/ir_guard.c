/* matoki/ir_guard.c — quad-sabit dallanma planı (ir_guard.h). Ters sırada tek geçiş: SSA'da bir komutun tüm kullanıcıları
 * ondan sonra gelir, yani ona ulaşıldığında düğümü kesinleşmiştir: kullanıcıların kenar düğümlerinin en yakın ortak atası.
 * Grup yalnız bitişik select'lerden oluşur (aralarında canlı komut yok): böylece kol üyelerinin hepsi grubun ilk select'inden
 * önce gelir ve dal o noktada açılabilir. Maske tam olmalı (0 / ~0): kısmi maskede select iki kolu da okur. */
#include "matoki/ir_guard.h"

#define GUARD_NONE 0xFFFFFFFFu

static u32 guard_lca(const IrGuardNode* nodes, u32 x, u32 y) {
    while (x != y) {
        if (nodes[x].depth >= nodes[y].depth) x = nodes[x].parent;
        else y = nodes[y].parent;
    }
    return x;
}

static void guard_merge(IrGuards* g, IrRef r, u32 edge) {
    g->node[r] = g->node[r] == GUARD_NONE ? edge : guard_lca(g->nodes, g->node[r], edge);
}

/* forward: değer kesin 0 / ~0 mı (karşılaştırmalar ve mantık birleşimleri) */
static b32 guard_full_op(const IrFunc* f, const u8* full, u32 i) {
    const IrInst* in = &f->ins[i];
    switch (in->op) {
    case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE:
    case IR_IEQ: case IR_INE: case IR_ILT: case IR_ILE: case IR_IGT: case IR_IGE:
        return TRUE;
    case IR_CONST:
        return in->type == IR_T_I32 && (in->imm == 0u || in->imm == 0xFFFFFFFFu);
    case IR_IAND: case IR_IOR: case IR_IXOR:
        return full[in->a] && full[in->b];
    case IR_SELECT:
        return in->type == IR_T_I32 && full[in->b] && full[in->c];
    default:
        return FALSE;
    }
}

void ir_guard_build(Arena* a, const IrFunc* f, const u8* skip, b32 no_deriv_arm, IrGuards* g) {
    u32 n = f->count, nsel = 0;
    u8* full = ARENA_ARRAY(a, u8, n);
    for (u32 i = 1; i < n; i++) {
        if (!f->ins[i].live) continue;
        full[i] = guard_full_op(f, full, i) ? 1 : 0;
        if (f->ins[i].op == IR_SELECT) nsel++;
    }
    g->node = ARENA_ARRAY(a, u32, n);
    g->next = ARENA_ARRAY(a, u32, n);
    g->group_at = ARENA_ARRAY(a, u32, n);
    g->nodes = ARENA_ARRAY(a, IrGuardNode, nsel * 2u + 1u);
    g->mask = ARENA_ARRAY(a, IrRef, nsel + 1u);
    g->branch = ARENA_ARRAY(a, u8, nsel + 1u);
    g->ngroups = 0;
    u32* group = ARENA_ARRAY(a, u32, n);   /* select'in grubu + 1 */
    u32* first = ARENA_ARRAY(a, u32, nsel + 1u);   /* grup → en küçük indeksli select */
    for (u32 i = 0; i < n; i++) g->node[i] = GUARD_NONE;
    guard_merge(g, f->out, 0);
    if (f->kill) guard_merge(g, f->kill, 0);
    for (u32 k = 0; k < f->nxout; k++) guard_merge(g, f->xout[k], 0);
    u32 prev = 0;
    for (u32 i = n - 1; i >= 1; i--) {
        const IrInst* in = &f->ins[i];
        if (!in->live) continue;
        if (g->node[i] == GUARD_NONE) g->node[i] = 0;
        u32 at = g->node[i];
        b32 sel = in->op == IR_SELECT && in->b != in->c && f->ins[in->a].uniform && full[in->a] && !(skip && skip[i]) &&
                  g->nodes[at].depth < IR_GUARD_MAX_DEPTH;
        if (sel) {
            u32 gr;
            if (prev && group[prev] && f->ins[prev].a == in->a && g->node[prev] == at) {
                gr = group[prev] - 1u;
            } else {
                gr = g->ngroups++;
                g->mask[gr] = in->a;
                for (u32 s = 0; s < 2; s++) {
                    IrGuardNode* c = &g->nodes[gr * 2u + 1u + s];
                    c->parent = at;
                    c->depth = g->nodes[at].depth + 1u;
                }
            }
            group[i] = gr + 1u;
            first[gr] = i;
            guard_merge(g, in->a, at);
            guard_merge(g, in->b, ir_guard_true(gr));
            guard_merge(g, in->c, ir_guard_false(gr));
        } else {
            u32 ar = ir_op_arity((IrOp)in->op);
            const u32 ops[3] = { in->a, in->b, in->c };
            for (u32 k = 0; k < ar; k++) guard_merge(g, ops[k], at);
        }
        prev = i;
    }
    /* üye listeleri (artan sıra), maliyet */
    for (u32 i = 1; i < n; i++) {
        if (!f->ins[i].live) continue;
        IrGuardNode* c = &g->nodes[g->node[i]];
        if (c->tail) g->next[c->tail] = i;
        else c->head = i;
        c->tail = i;
        c->cost++;
        if (f->ins[i].op == IR_DERIV_X || f->ins[i].op == IR_DERIV_Y || f->ins[i].op == IR_QUAD0) c->deriv++;
    }
    for (u32 gr = 0; gr < g->ngroups; gr++) g->group_at[first[gr]] = gr + 1u;
    for (u32 k = g->ngroups * 2u; k >= 1u; k--) {   /* alt düğümler üstlerinden sonra kuruldu: id > üst id */
        IrGuardNode* c = &g->nodes[k];
        g->nodes[c->parent].cost += c->cost;
        g->nodes[c->parent].deriv += c->deriv;
    }
    for (u32 gr = 0; gr < g->ngroups; gr++) {
        const IrGuardNode* t = &g->nodes[ir_guard_true(gr)];
        const IrGuardNode* e = &g->nodes[ir_guard_false(gr)];
        g->branch[gr] = t->cost + e->cost >= IR_GUARD_MIN_COST && !(no_deriv_arm && t->deriv + e->deriv) ? 1 : 0;
    }
}
