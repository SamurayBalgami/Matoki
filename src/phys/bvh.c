/* phys/bvh.c — dinamik AABB ağacı: ekleme (yüzey alanı maliyeti), çıkarma, taşıma, AVL dengesi,
 * kutu sorgusu ve ışın taraması. Box2D v2.4 b2DynamicTree algoritmasının 3B uyarlaması (alan = yüzey
 * alanının yarısı). Yığın tabanlı gezinme (özyineleme yok), sabit yığın 256 (dengeli ağaçta yükseklik ~2·log n). */
#include "phys/bvh.h"
#include "core/mem.h"
#include "core/sys.h"

static inline Vec3 vmin(Vec3 a, Vec3 b) { return v3(m_minf(a.x, b.x), m_minf(a.y, b.y), m_minf(a.z, b.z)); }
static inline Vec3 vmax(Vec3 a, Vec3 b) { return v3(m_maxf(a.x, b.x), m_maxf(a.y, b.y), m_maxf(a.z, b.z)); }

static inline f32 area(Vec3 mn, Vec3 mx) {
    Vec3 e = v3_sub(mx, mn);
    return e.x * e.y + e.y * e.z + e.z * e.x;
}

static inline b32 overlap(Vec3 amn, Vec3 amx, Vec3 bmn, Vec3 bmx) {
    return amn.x <= bmx.x && amx.x >= bmn.x && amn.y <= bmx.y && amx.y >= bmn.y && amn.z <= bmx.z && amx.z >= bmn.z;
}

void tree_init(DynTree* t, Arena* a, u32 cap, f32 margin) {
    t->nodes = ARENA_ARRAY(a, TreeNode, cap ? cap : 1);
    t->cap = cap ? cap : 1;
    t->margin = margin;
    tree_clear(t);
}

void tree_clear(DynTree* t) {
    t->root = TREE_NULL;
    t->count = 0;
    t->leaves = 0;
    for (u32 i = 0; i < t->cap; i++) {
        t->nodes[i].parent = i + 1 < t->cap ? i + 1 : TREE_NULL;
        t->nodes[i].height = -1;
    }
    t->free_list = 0;
}

static u32 alloc_node(DynTree* t) {
    if (t->free_list == TREE_NULL) sys_panic("bvh: düğüm havuzu dolu");
    u32 n = t->free_list;
    TreeNode* nd = &t->nodes[n];
    t->free_list = nd->parent;
    nd->parent = nd->child1 = nd->child2 = TREE_NULL;
    nd->height = 0;
    nd->body = TREE_NULL;
    t->count++;
    return n;
}

static void free_node(DynTree* t, u32 n) {
    t->nodes[n].parent = t->free_list;
    t->nodes[n].height = -1;
    t->free_list = n;
    t->count--;
}

/* A dengesizse döndür; dönüş: alt ağacın yeni kökü */
static u32 balance(DynTree* t, u32 iA) {
    TreeNode* A = &t->nodes[iA];
    if (A->child1 == TREE_NULL || A->height < 2) return iA;
    u32 iB = A->child1, iC = A->child2;
    TreeNode* B = &t->nodes[iB];
    TreeNode* C = &t->nodes[iC];
    i32 bal = C->height - B->height;
    if (bal > 1) {   /* C'yi yukarı al */
        u32 iF = C->child1, iG = C->child2;
        TreeNode* F = &t->nodes[iF];
        TreeNode* G = &t->nodes[iG];
        C->child1 = iA;
        C->parent = A->parent;
        A->parent = iC;
        if (C->parent != TREE_NULL) {
            TreeNode* P = &t->nodes[C->parent];
            if (P->child1 == iA) P->child1 = iC;
            else P->child2 = iC;
        } else {
            t->root = iC;
        }
        if (F->height > G->height) {
            C->child2 = iF;
            A->child2 = iG;
            G->parent = iA;
            A->mn = vmin(B->mn, G->mn); A->mx = vmax(B->mx, G->mx);
            C->mn = vmin(A->mn, F->mn); C->mx = vmax(A->mx, F->mx);
            A->height = 1 + (B->height > G->height ? B->height : G->height);
            C->height = 1 + (A->height > F->height ? A->height : F->height);
        } else {
            C->child2 = iG;
            A->child2 = iF;
            F->parent = iA;
            A->mn = vmin(B->mn, F->mn); A->mx = vmax(B->mx, F->mx);
            C->mn = vmin(A->mn, G->mn); C->mx = vmax(A->mx, G->mx);
            A->height = 1 + (B->height > F->height ? B->height : F->height);
            C->height = 1 + (A->height > G->height ? A->height : G->height);
        }
        return iC;
    }
    if (bal < -1) {   /* B'yi yukarı al */
        u32 iD = B->child1, iE = B->child2;
        TreeNode* D = &t->nodes[iD];
        TreeNode* E = &t->nodes[iE];
        B->child1 = iA;
        B->parent = A->parent;
        A->parent = iB;
        if (B->parent != TREE_NULL) {
            TreeNode* P = &t->nodes[B->parent];
            if (P->child1 == iA) P->child1 = iB;
            else P->child2 = iB;
        } else {
            t->root = iB;
        }
        if (D->height > E->height) {
            B->child2 = iD;
            A->child1 = iE;
            E->parent = iA;
            A->mn = vmin(C->mn, E->mn); A->mx = vmax(C->mx, E->mx);
            B->mn = vmin(A->mn, D->mn); B->mx = vmax(A->mx, D->mx);
            A->height = 1 + (C->height > E->height ? C->height : E->height);
            B->height = 1 + (A->height > D->height ? A->height : D->height);
        } else {
            B->child2 = iE;
            A->child1 = iD;
            D->parent = iA;
            A->mn = vmin(C->mn, D->mn); A->mx = vmax(C->mx, D->mx);
            B->mn = vmin(A->mn, E->mn); B->mx = vmax(A->mx, E->mx);
            A->height = 1 + (C->height > D->height ? C->height : D->height);
            B->height = 1 + (A->height > E->height ? A->height : E->height);
        }
        return iB;
    }
    return iA;
}

static void fix_upwards(DynTree* t, u32 i) {
    while (i != TREE_NULL) {
        i = balance(t, i);
        TreeNode* n = &t->nodes[i];
        const TreeNode* c1 = &t->nodes[n->child1];
        const TreeNode* c2 = &t->nodes[n->child2];
        n->height = 1 + (c1->height > c2->height ? c1->height : c2->height);
        n->mn = vmin(c1->mn, c2->mn);
        n->mx = vmax(c1->mx, c2->mx);
        i = n->parent;
    }
}

static void insert_leaf(DynTree* t, u32 leaf) {
    if (t->root == TREE_NULL) {
        t->root = leaf;
        t->nodes[leaf].parent = TREE_NULL;
        return;
    }
    Vec3 lmn = t->nodes[leaf].mn, lmx = t->nodes[leaf].mx;
    /* kardeş seçimi: yeni iç düğümün maliyeti vs çocuğa inmenin alt sınır maliyeti */
    u32 i = t->root;
    while (t->nodes[i].child1 != TREE_NULL) {
        const TreeNode* n = &t->nodes[i];
        u32 c1 = n->child1, c2 = n->child2;
        f32 a = area(n->mn, n->mx);
        f32 ca = area(vmin(n->mn, lmn), vmax(n->mx, lmx));
        f32 cost = 2.0f * ca;
        f32 inherit = 2.0f * (ca - a);
        const TreeNode* n1 = &t->nodes[c1];
        const TreeNode* n2 = &t->nodes[c2];
        f32 u1 = area(vmin(n1->mn, lmn), vmax(n1->mx, lmx));
        f32 u2 = area(vmin(n2->mn, lmn), vmax(n2->mx, lmx));
        f32 cost1 = (n1->child1 == TREE_NULL ? u1 : u1 - area(n1->mn, n1->mx)) + inherit;
        f32 cost2 = (n2->child1 == TREE_NULL ? u2 : u2 - area(n2->mn, n2->mx)) + inherit;
        if (cost < cost1 && cost < cost2) break;
        i = cost1 < cost2 ? c1 : c2;
    }
    u32 sib = i;
    u32 old_parent = t->nodes[sib].parent;
    u32 np = alloc_node(t);
    TreeNode* p = &t->nodes[np];
    p->parent = old_parent;
    p->mn = vmin(t->nodes[sib].mn, lmn);
    p->mx = vmax(t->nodes[sib].mx, lmx);
    p->height = t->nodes[sib].height + 1;
    p->child1 = sib;
    p->child2 = leaf;
    t->nodes[sib].parent = np;
    t->nodes[leaf].parent = np;
    if (old_parent != TREE_NULL) {
        TreeNode* op = &t->nodes[old_parent];
        if (op->child1 == sib) op->child1 = np;
        else op->child2 = np;
    } else {
        t->root = np;
    }
    fix_upwards(t, t->nodes[leaf].parent);
}

static void remove_leaf(DynTree* t, u32 leaf) {
    if (leaf == t->root) {
        t->root = TREE_NULL;
        return;
    }
    u32 parent = t->nodes[leaf].parent;
    u32 grand = t->nodes[parent].parent;
    u32 sib = t->nodes[parent].child1 == leaf ? t->nodes[parent].child2 : t->nodes[parent].child1;
    if (grand != TREE_NULL) {
        TreeNode* g = &t->nodes[grand];
        if (g->child1 == parent) g->child1 = sib;
        else g->child2 = sib;
        t->nodes[sib].parent = grand;
        free_node(t, parent);
        fix_upwards(t, grand);
    } else {
        t->root = sib;
        t->nodes[sib].parent = TREE_NULL;
        free_node(t, parent);
    }
}

u32 tree_insert(DynTree* t, Vec3 mn, Vec3 mx, u32 body) {
    u32 n = alloc_node(t);
    Vec3 m = v3(t->margin, t->margin, t->margin);
    t->nodes[n].mn = v3_sub(mn, m);
    t->nodes[n].mx = v3_add(mx, m);
    t->nodes[n].body = body;
    t->nodes[n].height = 0;
    insert_leaf(t, n);
    t->leaves++;
    return n;
}

void tree_remove(DynTree* t, u32 proxy) {
    remove_leaf(t, proxy);
    free_node(t, proxy);
    t->leaves--;
}

b32 tree_move(DynTree* t, u32 proxy, Vec3 mn, Vec3 mx) {
    TreeNode* n = &t->nodes[proxy];
    if (n->mn.x <= mn.x && n->mn.y <= mn.y && n->mn.z <= mn.z && mx.x <= n->mx.x && mx.y <= n->mx.y && mx.z <= n->mx.z) {
        return FALSE;
    }
    remove_leaf(t, proxy);
    Vec3 m = v3(t->margin, t->margin, t->margin);
    n->mn = v3_sub(mn, m);
    n->mx = v3_add(mx, m);
    insert_leaf(t, proxy);
    return TRUE;
}

u32 tree_query(const DynTree* t, Vec3 mn, Vec3 mx, u32* out, u32 max) {
    if (t->root == TREE_NULL) return 0;
    u32 stack[256];
    u32 sp = 0, k = 0;
    stack[sp++] = t->root;
    while (sp) {
        const TreeNode* n = &t->nodes[stack[--sp]];
        if (!overlap(n->mn, n->mx, mn, mx)) continue;
        if (n->child1 == TREE_NULL) {
            if (k < max) out[k++] = n->body;
        } else if (sp + 2 <= 256) {
            stack[sp++] = n->child1;
            stack[sp++] = n->child2;
        }
    }
    return k;
}

static b32 ray_box(Vec3 o, Vec3 inv, Vec3 mn, Vec3 mx, f32 tmax) {
    f32 t0 = 0.0f, t1 = tmax;
    const f32 oo[3] = { o.x, o.y, o.z }, id[3] = { inv.x, inv.y, inv.z };
    const f32 lo[3] = { mn.x, mn.y, mn.z }, hi[3] = { mx.x, mx.y, mx.z };
    for (u32 k = 0; k < 3; k++) {
        f32 a = (lo[k] - oo[k]) * id[k], b = (hi[k] - oo[k]) * id[k];
        if (a > b) { f32 s = a; a = b; b = s; }
        t0 = m_maxf(t0, a);
        t1 = m_minf(t1, b);
        if (t0 > t1) return FALSE;
    }
    return TRUE;
}

void tree_raycast(const DynTree* t, Vec3 o, Vec3 d, f32 tmax, TreeRayFn fn, void* ctx) {
    if (t->root == TREE_NULL) return;
    Vec3 inv = v3(d.x != 0.0f ? 1.0f / d.x : 3.4e38f, d.y != 0.0f ? 1.0f / d.y : 3.4e38f, d.z != 0.0f ? 1.0f / d.z : 3.4e38f);
    u32 stack[256];
    u32 sp = 0;
    stack[sp++] = t->root;
    while (sp) {
        const TreeNode* n = &t->nodes[stack[--sp]];
        if (!ray_box(o, inv, n->mn, n->mx, tmax)) continue;
        if (n->child1 == TREE_NULL) {
            fn(ctx, n->body, &tmax);
        } else if (sp + 2 <= 256) {
            stack[sp++] = n->child1;
            stack[sp++] = n->child2;
        }
    }
}

i32 tree_height(const DynTree* t) { return t->root == TREE_NULL ? 0 : t->nodes[t->root].height; }

f32 tree_area_ratio(const DynTree* t) {
    if (t->root == TREE_NULL) return 0.0f;
    f32 root = area(t->nodes[t->root].mn, t->nodes[t->root].mx), sum = 0.0f;
    for (u32 i = 0; i < t->cap; i++) {
        const TreeNode* n = &t->nodes[i];
        if (n->height > 0) sum += area(n->mn, n->mx);
    }
    return root > 0.0f ? sum / root : 0.0f;
}

b32 tree_validate(const DynTree* t) {
    if (t->root == TREE_NULL) return t->leaves == 0;
    if (t->nodes[t->root].parent != TREE_NULL) return FALSE;
    u32 stack[256], sp = 0, leaves = 0, nodes = 0;
    stack[sp++] = t->root;
    while (sp) {
        u32 i = stack[--sp];
        const TreeNode* n = &t->nodes[i];
        nodes++;
        if (n->child1 == TREE_NULL) {
            if (n->height != 0) return FALSE;
            leaves++;
            continue;
        }
        const TreeNode* c1 = &t->nodes[n->child1];
        const TreeNode* c2 = &t->nodes[n->child2];
        if (c1->parent != i || c2->parent != i) return FALSE;
        i32 h = 1 + (c1->height > c2->height ? c1->height : c2->height);
        if (n->height != h) return FALSE;   /* (tek döndürme tam AVL garantisi vermez; denge testte ölçülür) */
        Vec3 mn = vmin(c1->mn, c2->mn), mx = vmax(c1->mx, c2->mx);
        if (mn.x != n->mn.x || mn.y != n->mn.y || mn.z != n->mn.z || mx.x != n->mx.x || mx.y != n->mx.y || mx.z != n->mx.z) return FALSE;
        if (sp + 2 > 256) return FALSE;
        stack[sp++] = n->child1;
        stack[sp++] = n->child2;
    }
    return leaves == t->leaves && nodes == t->count;
}
