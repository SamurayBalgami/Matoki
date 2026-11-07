/* phys/joints.c — eklemler: top-mafsal, menteşe (sınır + motor), mesafe (çubuk / ip / aralık).
 * Eşitlik satırları (top 3, menteşe 3 + 2 açısal, çubuk 1) ağaç oluşturan eklemlerde doğrudan çözülür (D60, Baraff 1996):
 * eklemler yapraktan köke elenir, her gövde 6×6 etkin ters kütle W taşır → J W Jᵀ sistemi O(n)'de tam, kütle oranından bağımsız.
 * Döngü kapatan eklemler ağaç tepkili Schur tümleyeniyle tam (≤ 8); motor, ip ve fazla döngü Gauss-Seidel (satır başına, K⁻¹ blok).
 * Kollar her alt adımda güncel; ağaç satırları katı + bias (relax yok: enerji), kayma 1 mm'yi aşarsa konumda izdüşülür. */
#include "phys/joints.h"
#include "core/mem.h"
#include "core/sys.h"

/* ---- doğrudan çözücü durumu ---- */
#define JT_ROWS 6
#define JT_LOOPS 8          /* tam çözülen döngü kapatan eklem sayısı */
#define JT_LOOP_ROWS 24     /* ve toplam satırları (Schur tümleyeni yoğun) */
#define JOINT_STAB_ITERS 2
#define JOINT_BLOCK_ITERS 4   /* döngü varken ağaç ↔ Gauss-Seidel blok turu */
#define JOINT_STAB_TOL 1e-3f   /* bu kadarlık kayma bias'a bırakılır (m, rad); fazlası konumda izdüşülür */

typedef struct JtJoint {
    u32  id;
    i32  up, down;          /* yuva; down < 0 → eklemin alt ucu statik (dünya) */
    u32  su;                /* yukarı gövdenin J tarafı: 0 = a, 1 = b */
    u32  k;                 /* satır sayısı */
    i32  lim;               /* menteşe açı sınırı satırı (5): +1 alt, −1 üst, 0 yok */
    f32  J[2][JT_ROWS][6];  /* taraf (a, b) × satır × (doğrusal, açısal) */
    Vec3 t1, t2;            /* menteşe açısal satır yönleri */
    f32  sinv[JT_ROWS * JT_ROWS];   /* eleme anındaki S⁻¹ */
    f32  h[JT_ROWS][6];            /* W_up Jᵀ_up (eleme anındaki W) */
    f32  z[JT_ROWS];               /* sağ taraf → çözüm */
    f32  x[JT_ROWS];               /* son toplam Δλ (ağaç + döngü tepkisi) */
} JtJoint;

typedef struct JtSlot {
    u32 body;
    f32 W[36];
    f32 u[6], f[6];
    f32 p[6], p1[6];        /* genel itki: döngü / toplam */
} JtSlot;

typedef struct JointTree {
    JtJoint* jt;            /* eleme sırası: yapraktan köke */
    u32      count;
    JtSlot*  slot;
    u32      slot_count;
    u32*     body_slot;     /* gövde → yuva + 1 (0 = yok); kurulumdan sonra sıfırlanır */
    u32*     uf;            /* yuva birleşim-bul */
    u8*      state;         /* eklem: 0 dışarıda, 1 ağaçta, 2 yönü atandı, 3 + açı sınırı ağaçta, 4 döngü (tam) */
    u32*     adj_start;     /* yuva → ağaç eklemleri (CSR) */
    u32*     adj;
    u32*     queue;
    u8*      seen;
    b32      factored;
    u32      gs_count;      /* ağaç dışı (Gauss-Seidel) eklem sayısı */
    /* döngü kapatan eklemler: up = a'nın yuvası, down = b'nin, su = 0; ağaç tepkili Schur tümleyeni K (D60) */
    JtJoint  lp[JT_LOOPS];
    u32      loop_count, loop_rows;
    b32      kvalid;
    f32      kinv[JT_LOOP_ROWS * JT_LOOP_ROWS];
    f32      lz[JT_LOOP_ROWS];
} JointTree;

#define JOINT_HERTZ   60.0f
#define JOINT_DAMPING 2.0f

static inline Body* jb(PhysWorld* w, u32 i) { return i == PHYS_NO_BODY ? &w->world_body : &w->bodies[i]; }
static inline const Body* jbc(const PhysWorld* w, u32 i) { return i == PHYS_NO_BODY ? &w->world_body : &w->bodies[i]; }

/* ---- küçük 3x3 cebir (sütun-öncelikli) ---- */

static Mat3 skew(Vec3 r) {
    Mat3 s;
    s.m[0] = 0.0f;  s.m[1] = r.z;   s.m[2] = -r.y;
    s.m[3] = -r.z;  s.m[4] = 0.0f;  s.m[5] = r.x;
    s.m[6] = r.y;   s.m[7] = -r.x;  s.m[8] = 0.0f;
    return s;
}


static Mat3 mat3_inverse(const Mat3* a) {
    Vec3 c0 = v3(a->m[0], a->m[1], a->m[2]), c1 = v3(a->m[3], a->m[4], a->m[5]), c2 = v3(a->m[6], a->m[7], a->m[8]);
    Vec3 r0 = v3_cross(c1, c2), r1 = v3_cross(c2, c0), r2 = v3_cross(c0, c1);
    f32 det = v3_dot(c0, r0);
    f32 inv = det != 0.0f ? 1.0f / det : 0.0f;
    /* tersin satırları r0, r1, r2 → sütun-öncelikli */
    Mat3 o;
    o.m[0] = r0.x * inv; o.m[3] = r0.y * inv; o.m[6] = r0.z * inv;
    o.m[1] = r1.x * inv; o.m[4] = r1.y * inv; o.m[7] = r1.z * inv;
    o.m[2] = r2.x * inv; o.m[5] = r2.y * inv; o.m[8] = r2.z * inv;
    return o;
}

void joints_init(PhysWorld* w, Arena* a) {
    JointTree* T = ARENA_NEW(a, JointTree);
    u32 n = w->joint_cap, ns = 2 * w->joint_cap;
    T->jt = ARENA_ARRAY(a, JtJoint, n);
    T->slot = ARENA_ARRAY(a, JtSlot, ns);
    T->body_slot = ARENA_ARRAY(a, u32, w->cap);
    T->uf = ARENA_ARRAY(a, u32, ns);
    T->state = ARENA_ARRAY(a, u8, n);
    T->adj_start = ARENA_ARRAY(a, u32, ns + 1);
    T->adj = ARENA_ARRAY(a, u32, 2 * n);
    T->queue = ARENA_ARRAY(a, u32, ns);
    T->seen = ARENA_ARRAY(a, u8, ns);
    w->jtree = T;
}

/* ---- API ---- */

JointDesc phys_joint_desc(JointType type, u32 a, u32 b, Vec3 anchor) {
    JointDesc d;
    mem_zero(&d, sizeof(d));
    d.type = type;
    d.a = a;
    d.b = b;
    d.anchor_a = d.anchor_b = anchor;
    d.axis = v3(0, 1, 0);
    return d;
}

u32 phys_add_joint(PhysWorld* w, const JointDesc* d) {
    if (w->joint_count >= w->joint_cap) sys_panic("phys: eklem kapasitesi dolu");
    u32 id = w->joint_count++;
    Joint* j = &w->joints[id];
    mem_zero(j, sizeof(*j));
    j->type = d->type;
    j->a = d->a;
    j->b = d->b;
    const Body* A = jbc(w, d->a);
    const Body* B = jbc(w, d->b);
    Vec3 pb = d->type == JOINT_DISTANCE ? d->anchor_b : d->anchor_a;
    j->la = mat3_tmul_v3(&A->rot_m, v3_sub(d->anchor_a, A->pos));
    j->lb = mat3_tmul_v3(&B->rot_m, v3_sub(pb, B->pos));
    Vec3 ax = v3_norm(d->axis);
    Vec3 t1, t2;
    ortho_basis(ax, &t1, &t2);
    j->axis_a = mat3_tmul_v3(&A->rot_m, ax);
    j->axis_b = mat3_tmul_v3(&B->rot_m, ax);
    j->ref_a = mat3_tmul_v3(&A->rot_m, t1);
    j->ref_b = mat3_tmul_v3(&B->rot_m, t1);
    j->min_length = d->min_length;
    j->max_length = d->max_length;
    if (d->type == JOINT_DISTANCE && d->min_length == 0.0f && d->max_length == 0.0f) {
        j->min_length = j->max_length = v3_len(v3_sub(d->anchor_b, d->anchor_a));
    }
    j->limit = d->limit;
    j->lower = d->lower;
    j->upper = d->upper;
    j->motor = d->motor;
    j->motor_speed = d->motor_speed;
    j->max_motor_torque = d->max_motor_torque;
    j->collide_connected = d->collide_connected;
    j->length = v3_len(v3_sub(pb, d->anchor_a));
    if (d->b != PHYS_NO_BODY) phys_wake_body(w, d->b);
    if (d->a != PHYS_NO_BODY) phys_wake_body(w, d->a);
    return id;
}

b32 joints_block_pair(const PhysWorld* w, u32 a, u32 b) {
    for (u32 k = 0; k < w->joint_count; k++) {
        const Joint* j = &w->joints[k];
        if (j->collide_connected) continue;
        if ((j->a == a && j->b == b) || (j->a == b && j->b == a)) return TRUE;
    }
    return FALSE;
}

static inline Vec3 world_anchor(const Body* b, Vec3 l) { return v3_add(b->pos, mat3_mul_v3(&b->rot_m, l)); }

f32 phys_joint_error(const PhysWorld* w, u32 id) {
    const Joint* j = &w->joints[id];
    const Body* A = jbc(w, j->a);
    const Body* B = jbc(w, j->b);
    f32 len = v3_len(v3_sub(world_anchor(B, j->lb), world_anchor(A, j->la)));
    if (j->type != JOINT_DISTANCE) return len;
    if (len > j->max_length) return len - j->max_length;
    if (len < j->min_length) return j->min_length - len;
    return 0.0f;
}

static inline void apply_lin(Body* A, Body* B, Vec3 ra, Vec3 rb, Vec3 P) {
    A->vel = v3_sub(A->vel, v3_scale(P, A->inv_mass));
    A->ang_vel = v3_sub(A->ang_vel, mat3_mul_v3(&A->inv_inertia_world, v3_cross(ra, P)));
    B->vel = v3_add(B->vel, v3_scale(P, B->inv_mass));
    B->ang_vel = v3_add(B->ang_vel, mat3_mul_v3(&B->inv_inertia_world, v3_cross(rb, P)));
}

static inline void apply_ang(Body* A, Body* B, Vec3 L) {
    A->ang_vel = v3_sub(A->ang_vel, mat3_mul_v3(&A->inv_inertia_world, L));
    B->ang_vel = v3_add(B->ang_vel, mat3_mul_v3(&B->inv_inertia_world, L));
}

static Vec3 distance_dir(const Body* A, const Body* B, const Joint* j, f32* len) {
    Vec3 d = v3_sub(world_anchor(B, j->lb), world_anchor(A, j->la));
    *len = v3_len(d);
    return *len > 1e-6f ? v3_scale(d, 1.0f / *len) : v3(0, 1, 0);
}

static inline b32 dyn(const Body* b) { return b->inv_mass > 0.0f; }

static inline b32 tree_eligible(const Joint* j) {
    return j->type == JOINT_BALL || j->type == JOINT_HINGE || (j->type == JOINT_DISTANCE && j->min_length == j->max_length);
}

static u32 jt_find(u32* p, u32 x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static i32 slot_of(JointTree* T, const PhysWorld* w, u32 body) {
    if (body == PHYS_NO_BODY || !dyn(&w->bodies[body])) return -1;
    if (!T->body_slot[body]) {
        u32 s = T->slot_count++;
        T->slot[s].body = body;
        T->uf[s] = s;
        T->body_slot[body] = s + 1;
    }
    return (i32)T->body_slot[body] - 1;
}

/* Ağaç: dinamik gövdeler düğüm, eşitlik eklemleri kenar (statik uç düğüm değildir). Eklem sırasıyla birleşim-bul;
 * aynı bileşeni ikinci kez bağlayan eklem döngü kapatır → Gauss-Seidel. Her bileşen en küçük yuvadan BFS'le yönlenir;
 * eleme sırası BFS'in tersi (alt ağaç hep önce). */
static void tree_build(PhysWorld* w, JointTree* T) {
    T->count = 0;
    T->slot_count = 0;
    T->gs_count = 0;
    T->loop_count = T->loop_rows = 0;
    for (u32 k = 0; k < w->joint_count; k++) {
        Joint* j = &w->joints[k];
        T->state[k] = 0;
        b32 da = j->a != PHYS_NO_BODY && dyn(&w->bodies[j->a]), db = j->b != PHYS_NO_BODY && dyn(&w->bodies[j->b]);
        if (!da && !db) continue;
        if (!tree_eligible(j) || j->a == j->b) { T->gs_count++; continue; }
        i32 sa = slot_of(T, w, j->a), sb = slot_of(T, w, j->b);
        if (sa >= 0 && sb >= 0) {
            u32 ra = jt_find(T->uf, (u32)sa), rb = jt_find(T->uf, (u32)sb);
            if (ra == rb) {
                u32 kr = j->type == JOINT_BALL ? 3u : j->type == JOINT_HINGE ? 5u : 1u;
                if (T->loop_count < JT_LOOPS && T->loop_rows + kr <= JT_LOOP_ROWS) {
                    JtJoint* q = &T->lp[T->loop_count++];
                    q->id = (u32)(j - w->joints);
                    q->up = sa;
                    q->down = sb;
                    q->su = 0;
                    q->lim = 0;
                    T->loop_rows += kr;
                    T->state[q->id] = 4;
                } else {
                    T->gs_count++;
                }
                continue;
            }
            if (ra < rb) T->uf[rb] = ra;
            else T->uf[ra] = rb;
        }
        T->state[k] = 1;
        T->count++;
    }
    if (!T->count) {
        for (u32 s = 0; s < T->slot_count; s++) T->body_slot[T->slot[s].body] = 0;
        T->slot_count = 0;
        return;   /* döngü ağaçsız olamaz (kapattığı bileşen ağaç eklemleriyle kurulmuş) */
    }
    /* CSR komşuluk */
    u32 ns = T->slot_count;
    for (u32 s = 0; s <= ns; s++) T->adj_start[s] = 0;
    for (u32 k = 0; k < w->joint_count; k++) {
        if (T->state[k] != 1) continue;
        const Joint* j = &w->joints[k];
        if (j->a != PHYS_NO_BODY && T->body_slot[j->a]) T->adj_start[T->body_slot[j->a]]++;
        if (j->b != PHYS_NO_BODY && T->body_slot[j->b]) T->adj_start[T->body_slot[j->b]]++;
    }
    for (u32 s = 0; s < ns; s++) {
        T->adj_start[s + 1] += T->adj_start[s];
        T->queue[s] = T->adj_start[s];   /* doldurma imleci; BFS'ten önce işi biter */
    }
    for (u32 k = 0; k < w->joint_count; k++) {
        if (T->state[k] != 1) continue;
        const Joint* j = &w->joints[k];
        if (j->a != PHYS_NO_BODY && T->body_slot[j->a]) T->adj[T->queue[T->body_slot[j->a] - 1]++] = k;
        if (j->b != PHYS_NO_BODY && T->body_slot[j->b]) T->adj[T->queue[T->body_slot[j->b] - 1]++] = k;
    }
    for (u32 s = 0; s < ns; s++) T->seen[s] = 0;
    u32 nb = 0;
    for (u32 root = 0; root < ns; root++) {
        if (T->seen[root]) continue;
        u32 qh = 0, qt = 0;
        T->queue[qt++] = root;
        T->seen[root] = 1;
        while (qh < qt) {
            u32 p = T->queue[qh++];
            for (u32 e = T->adj_start[p]; e < T->adj_start[p + 1]; e++) {
                u32 k = T->adj[e];
                if (T->state[k] != 1) continue;
                T->state[k] = 2;
                const Joint* j = &w->joints[k];
                i32 sa = j->a != PHYS_NO_BODY ? (i32)T->body_slot[j->a] - 1 : -1;
                i32 sb = j->b != PHYS_NO_BODY ? (i32)T->body_slot[j->b] - 1 : -1;
                JtJoint* q = &T->jt[nb];
                q->id = k;
                q->su = sa == (i32)p ? 0u : 1u;
                q->up = (i32)p;
                q->down = q->su == 0 ? sb : sa;
                if (q->down >= 0) {
                    T->seen[q->down] = 1;
                    T->queue[qt++] = (u32)q->down;
                }
                nb++;
            }
        }
    }
    /* BFS sırasını ters çevir (yapraktan köke) */
    for (u32 i = 0, e = nb - 1; i < e; i++, e--) {
        JtJoint t = T->jt[i];
        T->jt[i] = T->jt[e];
        T->jt[e] = t;
    }
    for (u32 s = 0; s < ns; s++) T->body_slot[T->slot[s].body] = 0;
}

/* tarafın 6 sütunlu satırı · (v, ω) */
static inline f32 row_dot(const f32* r, Vec3 v, Vec3 om) {
    return r[0] * v.x + r[1] * v.y + r[2] * v.z + r[3] * om.x + r[4] * om.y + r[5] * om.z;
}

static inline void set_row(f32* r, Vec3 lin, Vec3 ang) {
    r[0] = lin.x; r[1] = lin.y; r[2] = lin.z;
    r[3] = ang.x; r[4] = ang.y; r[5] = ang.z;
}

static inline void w_mul(const f32* W, const f32* x, f32* o) {
    for (u32 i = 0; i < 6; i++) {
        const f32* r = W + i * 6;
        o[i] = r[0] * x[0] + r[1] * x[1] + r[2] * x[2] + r[3] * x[3] + r[4] * x[4] + r[5] * x[5];
    }
}

static inline f32 dot6(const f32* a, const f32* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
}

/* simetrik k×k (k ≤ 5) tersi, LDLᵀ ile; göreli pivot 1e-6'nın altındaysa o yön 0 (fazla kısıt: ör. aynı gövdede
 * iki dünya eklemi) */
static void sym_inverse(const f32* S, u32 k, f32* out) {
    f32 L[JT_ROWS * JT_ROWS], d[JT_ROWS], di[JT_ROWS];
    for (u32 i = 0; i < k; i++) {
        for (u32 c = 0; c < i; c++) {
            f32 v = S[i * k + c];
            for (u32 m = 0; m < c; m++) v -= L[i * k + m] * L[c * k + m] * d[m];
            L[i * k + c] = v * di[c];
        }
        f32 v = S[i * k + i];
        for (u32 m = 0; m < i; m++) v -= L[i * k + m] * L[i * k + m] * d[m];
        d[i] = v;
        di[i] = v > 1e-6f * S[i * k + i] && v > 0.0f ? 1.0f / v : 0.0f;
        if (di[i] == 0.0f) d[i] = 0.0f;
        L[i * k + i] = 1.0f;
    }
    /* L⁻¹ (birim alt üçgen) */
    f32 Li[JT_ROWS * JT_ROWS];
    for (u32 c = 0; c < k; c++) {
        for (u32 i = 0; i < k; i++) {
            if (i < c) { Li[i * k + c] = 0.0f; continue; }
            if (i == c) { Li[i * k + c] = 1.0f; continue; }
            f32 v = 0.0f;
            for (u32 m = c; m < i; m++) v -= L[i * k + m] * Li[m * k + c];
            Li[i * k + c] = v;
        }
    }
    for (u32 i = 0; i < k; i++) {
        for (u32 c = 0; c < k; c++) {
            f32 v = 0.0f;
            for (u32 m = 0; m < k; m++) v += Li[m * k + i] * di[m] * Li[m * k + c];
            out[i * k + c] = v;
        }
    }
}

static f32 hinge_angle(const Joint* j, const Body* A, const Body* B) {
    Vec3 aw = mat3_mul_v3(&A->rot_m, j->axis_a);
    Vec3 refa = mat3_mul_v3(&A->rot_m, j->ref_a), refb = mat3_mul_v3(&B->rot_m, j->ref_b);
    return m_atan2f(v3_dot(v3_cross(refa, refb), aw), v3_dot(refa, refb));
}

/* eşitlik satırları (sınırsız): top 3, menteşe 5, çubuk 1 */
static void joint_rows(PhysWorld* w, JtJoint* q) {
    Joint* j = &w->joints[q->id];
    Body* A = jb(w, j->a);
    Body* B = jb(w, j->b);
    if (j->type == JOINT_DISTANCE) {
        f32 len;
        Vec3 u = distance_dir(A, B, j, &len);
        set_row(q->J[0][0], v3_scale(u, -1.0f), v3_scale(v3_cross(j->ra, u), -1.0f));
        set_row(q->J[1][0], u, v3_cross(j->rb, u));
        q->k = 1;
        return;
    }
    for (u32 i = 0; i < 3; i++) {
        Vec3 e = v3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f);
        set_row(q->J[0][i], v3_scale(e, -1.0f), v3_scale(v3_cross(j->ra, e), -1.0f));
        set_row(q->J[1][i], e, v3_cross(j->rb, e));
    }
    q->k = 3;
    if (j->type == JOINT_HINGE) {
        Vec3 aw = mat3_mul_v3(&A->rot_m, j->axis_a);
        ortho_basis(aw, &q->t1, &q->t2);
        set_row(q->J[0][3], v3(0, 0, 0), v3_scale(q->t1, -1.0f));
        set_row(q->J[1][3], v3(0, 0, 0), q->t1);
        set_row(q->J[0][4], v3(0, 0, 0), v3_scale(q->t2, -1.0f));
        set_row(q->J[1][4], v3(0, 0, 0), q->t2);
        q->k = 5;
    }
}

static void tree_rows(PhysWorld* w, JointTree* T) {
    for (u32 l = 0; l < T->loop_count; l++) joint_rows(w, &T->lp[l]);
    for (u32 n = 0; n < T->count; n++) {
        JtJoint* q = &T->jt[n];
        Joint* j = &w->joints[q->id];
        Body* A = jb(w, j->a);
        Body* B = jb(w, j->b);
        joint_rows(w, q);
        if (j->type == JOINT_HINGE) {
            Vec3 aw = mat3_mul_v3(&A->rot_m, j->axis_a);
            /* sınırın dışındaysa açı sınırı eşitlik satırı olur (etkin küme; yanlış yönde itkide tree_solve atar) */
            q->lim = 0;
            if (j->limit) {
                j->angle = hinge_angle(j, A, B);
                q->lim = j->angle <= j->lower ? 1 : j->angle >= j->upper ? -1 : 0;
            }
            if (q->lim) {
                set_row(q->J[0][5], v3(0, 0, 0), v3_scale(aw, -1.0f));
                set_row(q->J[1][5], v3(0, 0, 0), aw);
                q->k = 6;
            }
            T->state[q->id] = q->lim ? 3 : 2;
        }
    }
    T->factored = FALSE;
    T->kvalid = FALSE;
}

/* J W Jᵀ eleme (yapraktan köke): S = Jd Wd Jdᵀ + Ju Wu Juᵀ, W_up ← W_up − h S⁻¹ hᵀ, h = W_up Jᵀ_up */
static void tree_factor(PhysWorld* w, JointTree* T) {
    for (u32 s = 0; s < T->slot_count; s++) {
        JtSlot* sl = &T->slot[s];
        const Body* b = &w->bodies[sl->body];
        const Mat3* I = &b->inv_inertia_world;
        for (u32 i = 0; i < 36; i++) sl->W[i] = 0.0f;
        sl->W[0] = sl->W[7] = sl->W[14] = b->inv_mass;
        for (u32 r = 0; r < 3; r++) {
            for (u32 c = 0; c < 3; c++) sl->W[(3 + r) * 6 + 3 + c] = I->m[c * 3 + r];
        }
    }
    for (u32 n = 0; n < T->count; n++) {
        JtJoint* q = &T->jt[n];
        u32 k = q->k, su = q->su, sd = 1u - su;
        f32* Wu = T->slot[q->up].W;
        f32 S[JT_ROWS * JT_ROWS];
        for (u32 r = 0; r < k; r++) w_mul(Wu, q->J[su][r], q->h[r]);
        for (u32 r = 0; r < k; r++) {
            for (u32 c = 0; c < k; c++) S[r * k + c] = dot6(q->J[su][r], q->h[c]);
        }
        if (q->down >= 0) {
            const f32* Wd = T->slot[q->down].W;
            f32 t[JT_ROWS][6];
            for (u32 r = 0; r < k; r++) w_mul(Wd, q->J[sd][r], t[r]);
            for (u32 r = 0; r < k; r++) {
                for (u32 c = 0; c < k; c++) S[r * k + c] += dot6(q->J[sd][r], t[c]);
            }
        }
        sym_inverse(S, k, q->sinv);
        f32 hs[JT_ROWS][6];
        for (u32 r = 0; r < k; r++) {
            for (u32 a = 0; a < 6; a++) {
                f32 v = 0.0f;
                for (u32 c = 0; c < k; c++) v += q->sinv[r * k + c] * q->h[c][a];
                hs[r][a] = v;
            }
        }
        for (u32 a = 0; a < 6; a++) {
            for (u32 b = 0; b < 6; b++) {
                f32 v = 0.0f;
                for (u32 r = 0; r < k; r++) v += q->h[r][a] * hs[r][b];
                Wu[a * 6 + b] -= v;
            }
        }
    }
    T->factored = TRUE;
}

/* (J W Jᵀ) x = b; b q->z'de gelir, x q->z'ye yazılır. İleri: z −= J (Σ h S⁻¹ z) gövde birikimleriyle;
 * geri: x = S⁻¹ (z − hᵀ f_up), f = sonraki eklemlerin Jᵀ x'i */
static void tree_substitute(PhysWorld* w, JointTree* T) {
    if (!T->factored) tree_factor(w, T);
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 i = 0; i < 6; i++) T->slot[s].u[i] = T->slot[s].f[i] = 0.0f;
    }
    for (u32 n = 0; n < T->count; n++) {
        JtJoint* q = &T->jt[n];
        u32 k = q->k, su = q->su, sd = 1u - su;
        f32* uu = T->slot[q->up].u;
        const f32* ud = q->down >= 0 ? T->slot[q->down].u : NULL;
        for (u32 r = 0; r < k; r++) {
            q->z[r] -= dot6(q->J[su][r], uu);
            if (ud) q->z[r] -= dot6(q->J[sd][r], ud);
        }
        for (u32 r = 0; r < k; r++) {
            f32 y = 0.0f;
            for (u32 c = 0; c < k; c++) y += q->sinv[r * k + c] * q->z[c];
            for (u32 a = 0; a < 6; a++) uu[a] += q->h[r][a] * y;
        }
    }
    for (u32 n = T->count; n-- > 0;) {
        JtJoint* q = &T->jt[n];
        u32 k = q->k, su = q->su, sd = 1u - su;
        f32* fu = T->slot[q->up].f;
        f32 t[JT_ROWS], x[JT_ROWS];
        for (u32 r = 0; r < k; r++) t[r] = q->z[r] - dot6(q->h[r], fu);
        for (u32 r = 0; r < k; r++) {
            f32 v = 0.0f;
            for (u32 c = 0; c < k; c++) v += q->sinv[r * k + c] * t[c];
            x[r] = v;
        }
        f32* fd = q->down >= 0 ? T->slot[q->down].f : NULL;
        for (u32 r = 0; r < k; r++) {
            for (u32 a = 0; a < 6; a++) {
                fu[a] += q->J[su][r][a] * x[r];
                if (fd) fd[a] += q->J[sd][r][a] * x[r];
            }
            q->z[r] = x[r];
        }
    }
}

/* gövde başına net genel itki Σ Jᵀ x → yuva u'su. Ağır yükte eklem itkileri büyük ve hafif gövdede birbirini götürür;
 * tek tek uygulamak (özellikle dönüşte) bu farkı doğrusal olmayan biçimde bozar → önce toplanır, sonra bir kez uygulanır. */
static void tree_gather(JointTree* T) {
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 i = 0; i < 6; i++) T->slot[s].u[i] = 0.0f;
    }
    for (u32 n = 0; n < T->count; n++) {
        const JtJoint* q = &T->jt[n];
        u32 su = q->su, sd = 1u - su;
        f32* uu = T->slot[q->up].u;
        f32* ud = q->down >= 0 ? T->slot[q->down].u : NULL;
        for (u32 r = 0; r < q->k; r++) {
            for (u32 a = 0; a < 6; a++) {
                uu[a] += q->J[su][r][a] * q->z[r];
                if (ud) ud[a] += q->J[sd][r][a] * q->z[r];
            }
        }
    }
}

/* ağaç ekleminin satır hataları (güncel pozlarla); çubukta güncel uzunluğu da yazar */
static void tree_error(PhysWorld* w, const JtJoint* q, f32* C) {
    Joint* j = &w->joints[q->id];
    const Body* A = jbc(w, j->a);
    const Body* B = jbc(w, j->b);
    Vec3 e = v3_sub(world_anchor(B, j->lb), world_anchor(A, j->la));
    if (j->type == JOINT_DISTANCE) {
        f32 len = v3_len(e);
        j->length = len;
        C[0] = len - j->min_length;
        return;
    }
    C[0] = e.x; C[1] = e.y; C[2] = e.z;
    if (j->type == JOINT_HINGE) {
        Vec3 err = v3_cross(mat3_mul_v3(&A->rot_m, j->axis_a), mat3_mul_v3(&B->rot_m, j->axis_b));
        C[3] = v3_dot(err, q->t1);
        C[4] = v3_dot(err, q->t2);
        if (q->lim) {
            f32 a = hinge_angle(j, A, B);
            C[5] = q->lim > 0 ? m_minf(a - j->lower, 0.0f) : m_maxf(a - j->upper, 0.0f);   /* yalnız sınır dışı */
        }
    }
}

/* W₀ x (W₀ = diag(m⁻¹ I, I⁻¹)) */
static inline void w0_mul(const Body* b, const f32* x, f32* o) {
    Vec3 a = mat3_mul_v3(&b->inv_inertia_world, v3(x[3], x[4], x[5]));
    o[0] = x[0] * b->inv_mass; o[1] = x[1] * b->inv_mass; o[2] = x[2] * b->inv_mass;
    o[3] = a.x; o[4] = a.y; o[5] = a.z;
}

/* döngü ekleminin i. satırı · W₀ (yuva itkisi): p (total = FALSE) ya da p1 (TRUE) */
static f32 loop_row_dv(PhysWorld* w, JointTree* T, const JtJoint* q, u32 i, b32 total) {
    f32 v = 0.0f, d[6];
    const i32 sl[2] = { q->up, q->down };
    for (u32 s = 0; s < 2; s++) {
        if (sl[s] < 0) continue;
        const JtSlot* t = &T->slot[sl[s]];
        w0_mul(&w->bodies[t->body], total ? t->p1 : t->p, d);
        v += dot6(q->J[s][i], d);
    }
    return v;
}

/* Ağacın tepkisi: yuvalara p genel itkisi uygulanınca ağaç kısıtlarını koruyan itki u (J_T W₀ (p + u) = 0) → p += u.
 * acc: ağaç eklemlerinin Δλ'sı x'e eklenir. */
static void tree_respond(PhysWorld* w, JointTree* T, b32 acc) {
    for (u32 n = 0; n < T->count; n++) {
        JtJoint* q = &T->jt[n];
        f32 dv[2][6];
        const i32 sl[2] = { q->su == 0 ? q->up : q->down, q->su == 0 ? q->down : q->up };
        for (u32 s = 0; s < 2; s++) {
            if (sl[s] >= 0) w0_mul(&w->bodies[T->slot[sl[s]].body], T->slot[sl[s]].p, dv[s]);
        }
        for (u32 r = 0; r < q->k; r++) {
            f32 v = 0.0f;
            for (u32 s = 0; s < 2; s++) {
                if (sl[s] >= 0) v += dot6(q->J[s][r], dv[s]);
            }
            q->z[r] = -v;
        }
    }
    tree_substitute(w, T);
    if (acc) {
        for (u32 n = 0; n < T->count; n++) {
            for (u32 r = 0; r < T->jt[n].k; r++) T->jt[n].x[r] += T->jt[n].z[r];
        }
    }
    tree_gather(T);
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 i = 0; i < 6; i++) T->slot[s].p[i] += T->slot[s].u[i];
    }
}

/* yoğun simetrik ters (Gauss-Jordan, köşegen pivot; göreli 1e-6 altı pivot → o yön 0: fazla kısıt) */
static void dense_inverse(f32* A, u32 n, f32* out) {
    f32 scale = 0.0f;
    for (u32 i = 0; i < n; i++) scale = m_maxf(scale, m_absf(A[i * n + i]));
    for (u32 i = 0; i < n * n; i++) out[i] = 0.0f;
    for (u32 i = 0; i < n; i++) out[i * n + i] = 1.0f;
    for (u32 c = 0; c < n; c++) {
        f32 pv = A[c * n + c];
        if (!(pv > 1e-6f * scale)) {
            for (u32 i = 0; i < n; i++) {
                A[c * n + i] = A[i * n + c] = 0.0f;
                out[c * n + i] = out[i * n + c] = 0.0f;
            }
            continue;
        }
        f32 inv = 1.0f / pv;
        for (u32 i = 0; i < n; i++) {
            A[c * n + i] *= inv;
            out[c * n + i] *= inv;
        }
        for (u32 r = 0; r < n; r++) {
            f32 f = A[r * n + c];
            if (r == c || f == 0.0f) continue;
            for (u32 i = 0; i < n; i++) {
                A[r * n + i] -= f * A[c * n + i];
                out[r * n + i] -= f * out[c * n + i];
            }
        }
    }
}

/* Döngü satırlarının ağaç tepkili Schur tümleyeni K = J_L (W₀ − W₀ J_Tᵀ A_T⁻¹ J_T W₀) J_Lᵀ; her sütun bir ağaç çözümü */
static void loop_factor(PhysWorld* w, JointTree* T) {
    u32 m = T->loop_rows, col = 0;
    f32 K[JT_LOOP_ROWS * JT_LOOP_ROWS];
    for (u32 l = 0; l < T->loop_count; l++) {
        const JtJoint* q = &T->lp[l];
        for (u32 i = 0; i < q->k; i++, col++) {
            for (u32 s = 0; s < T->slot_count; s++) {
                for (u32 a = 0; a < 6; a++) T->slot[s].p[a] = 0.0f;
            }
            if (q->up >= 0) for (u32 a = 0; a < 6; a++) T->slot[q->up].p[a] += q->J[0][i][a];
            if (q->down >= 0) for (u32 a = 0; a < 6; a++) T->slot[q->down].p[a] += q->J[1][i][a];
            tree_respond(w, T, FALSE);
            u32 row = 0;
            for (u32 l2 = 0; l2 < T->loop_count; l2++) {
                const JtJoint* q2 = &T->lp[l2];
                for (u32 i2 = 0; i2 < q2->k; i2++, row++) K[row * m + col] = loop_row_dv(w, T, q2, i2, FALSE);
            }
        }
    }
    dense_inverse(K, m, T->kinv);
    T->kvalid = TRUE;
}

/* Tüm eşitlik sistemi (ağaç + döngü). Sağ taraflar: ağaçta q->z, döngüde T->lz. Çıktı: eklemlerin toplam Δλ'sı x'te,
 * yuvaların toplam genel itkisi p1'de. Ağaç doğrudan; döngü satırları ağaç tepkisi içerilerek K⁻¹ ile → ikisi de tam. */
static void sys_solve(PhysWorld* w, JointTree* T) {
    tree_substitute(w, T);
    for (u32 n = 0; n < T->count; n++) {
        for (u32 r = 0; r < T->jt[n].k; r++) T->jt[n].x[r] = T->jt[n].z[r];
    }
    tree_gather(T);
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 i = 0; i < 6; i++) T->slot[s].p1[i] = T->slot[s].u[i];
    }
    if (!T->loop_count) return;
    if (!T->kvalid) loop_factor(w, T);
    f32 res[JT_LOOP_ROWS], lam[JT_LOOP_ROWS];
    u32 m = T->loop_rows, row = 0;
    for (u32 l = 0; l < T->loop_count; l++) {
        const JtJoint* q = &T->lp[l];
        for (u32 i = 0; i < q->k; i++, row++) res[row] = T->lz[row] - loop_row_dv(w, T, q, i, TRUE);
    }
    for (u32 r = 0; r < m; r++) {
        f32 v = 0.0f;
        for (u32 c = 0; c < m; c++) v += T->kinv[r * m + c] * res[c];
        lam[r] = v;
    }
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 a = 0; a < 6; a++) T->slot[s].p[a] = 0.0f;
    }
    row = 0;
    for (u32 l = 0; l < T->loop_count; l++) {
        JtJoint* q = &T->lp[l];
        for (u32 i = 0; i < q->k; i++, row++) {
            q->x[i] = lam[row];
            if (q->up >= 0) for (u32 a = 0; a < 6; a++) T->slot[q->up].p[a] += q->J[0][i][a] * lam[row];
            if (q->down >= 0) for (u32 a = 0; a < 6; a++) T->slot[q->down].p[a] += q->J[1][i][a] * lam[row];
        }
    }
    tree_respond(w, T, TRUE);
    for (u32 s = 0; s < T->slot_count; s++) {
        for (u32 i = 0; i < 6; i++) T->slot[s].p1[i] += T->slot[s].p[i];
    }
}

/* eklemin Δλ'sını biriken itkilere ekler (warm start) */
static void accum_impulse(PhysWorld* w, const JtJoint* q) {
    Joint* j = &w->joints[q->id];
    const f32* x = q->x;
    if (j->type == JOINT_DISTANCE) {
        j->len_imp += x[0];
        return;
    }
    j->lin_imp = v3_add(j->lin_imp, v3(x[0], x[1], x[2]));
    if (j->type == JOINT_HINGE) j->ang_imp = v3_add(j->ang_imp, v3_add(v3_scale(q->t1, x[3]), v3_scale(q->t2, x[4])));
    if (q->lim) j->limit_imp += x[5];
}

/* hız sağ tarafı −(J v + β C) */
static void vel_rhs(PhysWorld* w, const JtJoint* q, f32 br, f32* z) {
    Joint* j = &w->joints[q->id];
    const Body* A = jb(w, j->a);
    const Body* B = jb(w, j->b);
    f32 C[JT_ROWS];
    if (br != 0.0f) tree_error(w, q, C);
    for (u32 r = 0; r < q->k; r++) {
        f32 cdot = row_dot(q->J[0][r], A->vel, A->ang_vel) + row_dot(q->J[1][r], B->vel, B->ang_vel);
        z[r] = -(cdot + (br != 0.0f ? br * C[r] : 0.0f));
    }
}

/* Ağaç (ve tam döngü) eklemlerinin eşitlik satırları, hız düzeyinde tam çözüm: (J W Jᵀ) Δλ = −(J v + β C). Katı (yük
 * altında esneme yok); kayma bias hızıyla düzeltilir ve bias hızı relax'ta silinmez → merkezcil terimi bir alt adım
 * gecikmeyle verir, enerji korunur (bias'sız teğet izdüşümü alt adım başına θ²/2 kaybettirirdi). */
static void tree_solve(PhysWorld* w, JointTree* T, f32 br) {
    if (!T->count) return;
    for (u32 attempt = 0; attempt < 2; attempt++) {
        for (u32 n = 0; n < T->count; n++) vel_rhs(w, &T->jt[n], br, T->jt[n].z);
        u32 row = 0;
        for (u32 l = 0; l < T->loop_count; l++) {
            vel_rhs(w, &T->lp[l], br, &T->lz[row]);
            row += T->lp[l].k;
        }
        sys_solve(w, T);
        /* etkin küme: sınır itkisi çekmeye dönerse satır atılır, warm start'ın sınır itkisi geri alınır, yeniden çözülür */
        b32 drop = FALSE;
        for (u32 n = 0; n < T->count && attempt == 0; n++) {
            JtJoint* q = &T->jt[n];
            if (!q->lim) continue;
            Joint* j = &w->joints[q->id];
            f32 nw = j->limit_imp + q->x[5];
            if ((q->lim > 0 && nw >= 0.0f) || (q->lim < 0 && nw <= 0.0f)) continue;
            Body* A = jb(w, j->a);
            Body* B = jb(w, j->b);
            apply_ang(A, B, v3_scale(mat3_mul_v3(&A->rot_m, j->axis_a), -j->limit_imp));
            j->limit_imp = 0.0f;
            q->lim = 0;
            q->k = 5;
            T->state[q->id] = 2;
            drop = TRUE;
        }
        if (!drop) break;
        T->factored = FALSE;
        T->kvalid = FALSE;
    }
    for (u32 n = 0; n < T->count; n++) accum_impulse(w, &T->jt[n]);
    for (u32 l = 0; l < T->loop_count; l++) accum_impulse(w, &T->lp[l]);
    for (u32 s = 0; s < T->slot_count; s++) {
        Body* b = &w->bodies[T->slot[s].body];
        const f32* p = T->slot[s].p1;
        b->vel = v3_add(b->vel, v3_scale(v3(p[0], p[1], p[2]), b->inv_mass));
        b->ang_vel = v3_add(b->ang_vel, mat3_mul_v3(&b->inv_inertia_world, v3(p[3], p[4], p[5])));
    }
}

void joints_prepare(PhysWorld* w, f32 h) {
    UNUSED(h);
    for (u32 k = 0; k < w->joint_count; k++) {
        Joint* j = &w->joints[k];
        Body* A = jb(w, j->a);
        Body* B = jb(w, j->b);
        j->ra = mat3_mul_v3(&A->rot_m, j->la);
        j->rb = mat3_mul_v3(&B->rot_m, j->lb);
        if (j->type != JOINT_DISTANCE) {
            Mat3 sa = skew(j->ra), sb = skew(j->rb);
            Mat3 ta = mat3_mul(&sa, &A->inv_inertia_world), tb = mat3_mul(&sb, &B->inv_inertia_world);
            Mat3 ka = mat3_mul(&ta, &sa), kb = mat3_mul(&tb, &sb);
            Mat3 K;
            f32 m = A->inv_mass + B->inv_mass;
            for (u32 i = 0; i < 9; i++) K.m[i] = -ka.m[i] - kb.m[i];
            K.m[0] += m; K.m[4] += m; K.m[8] += m;
            j->k_inv = mat3_inverse(&K);
        }
    }
    JointTree* T = (JointTree*)w->jtree;
    tree_build(w, T);
    tree_rows(w, T);
}

void joints_warm_start(PhysWorld* w) {
    for (u32 k = 0; k < w->joint_count; k++) {
        Joint* j = &w->joints[k];
        Body* A = jb(w, j->a);
        Body* B = jb(w, j->b);
        if (j->type == JOINT_DISTANCE) {
            f32 len;
            Vec3 u = distance_dir(A, B, j, &len);
            apply_lin(A, B, j->ra, j->rb, v3_scale(u, j->len_imp));
            continue;
        }
        apply_lin(A, B, j->ra, j->rb, j->lin_imp);
        if (j->type == JOINT_HINGE) {
            Vec3 ax = mat3_mul_v3(&A->rot_m, j->axis_a);
            apply_ang(A, B, v3_add(j->ang_imp, v3_scale(ax, j->motor_imp + j->limit_imp)));
        }
    }
}

typedef struct Soft {
    f32 bias_rate, mass_scale, impulse_scale;
} Soft;

static Soft joint_soft(f32 h) {
    Soft s;
    f32 omega = M_TAU_F * JOINT_HERTZ;
    f32 a1 = 2.0f * JOINT_DAMPING + h * omega;
    f32 a2 = h * omega * a1;
    f32 a3 = 1.0f / (1.0f + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

/* C konum hatası için (bias, kütle ölçeği, itki ölçeği) */
static inline void coeffs(Soft s, b32 use_bias, b32 soft, f32 h, f32* bias_rate, f32* ms, f32* is) {
    if (!use_bias) { *bias_rate = 0.0f; *ms = 1.0f; *is = 0.0f; return; }
    if (soft) { *bias_rate = s.bias_rate; *ms = s.mass_scale; *is = s.impulse_scale; return; }
    *bias_rate = 0.2f / h;   /* Baumgarte */
    *ms = 1.0f;
    *is = 0.0f;
}

static inline f32 ang_mass(const Body* A, const Body* B, Vec3 t) {
    f32 k = v3_dot(t, mat3_mul_v3(&A->inv_inertia_world, t)) + v3_dot(t, mat3_mul_v3(&B->inv_inertia_world, t));
    return k > 0.0f ? 1.0f / k : 0.0f;
}

/* menteşe ekseni boyunca motor ve açı sınırı (tek yönlü → Gauss-Seidel); açıyı da ölçer */
static void hinge_axial(Joint* j, Body* A, Body* B, Vec3 aw, f32 h, f32 br, f32 ms, f32 is, b32 limit) {
    Vec3 refa = mat3_mul_v3(&A->rot_m, j->ref_a), refb = mat3_mul_v3(&B->rot_m, j->ref_b);
    j->angle = m_atan2f(v3_dot(v3_cross(refa, refb), aw), v3_dot(refa, refb));
    f32 am = ang_mass(A, B, aw);
    if (j->motor) {
        f32 cdot = v3_dot(v3_sub(B->ang_vel, A->ang_vel), aw);
        f32 lam = -am * (cdot - j->motor_speed);
        f32 maxi = j->max_motor_torque * h;
        f32 nw = m_clampf(j->motor_imp + lam, -maxi, maxi);
        lam = nw - j->motor_imp;
        j->motor_imp = nw;
        apply_ang(A, B, v3_scale(aw, lam));
    }
    if (j->limit && limit) {
        f32 C = 0.0f;
        i32 side = 0;
        if (j->angle <= j->lower) { C = j->angle - j->lower; side = 1; }
        else if (j->angle >= j->upper) { C = j->angle - j->upper; side = -1; }
        if (side) {
            f32 cdot = v3_dot(v3_sub(B->ang_vel, A->ang_vel), aw);
            f32 lam = -am * ms * (cdot + br * C) - is * j->limit_imp;
            f32 nw = j->limit_imp + lam;
            nw = side > 0 ? m_maxf(nw, 0.0f) : m_minf(nw, 0.0f);
            lam = nw - j->limit_imp;
            j->limit_imp = nw;
            apply_ang(A, B, v3_scale(aw, lam));
        } else {
            j->limit_imp = 0.0f;
        }
    }
}

/* Gauss-Seidel geçişi: ağaç dışı eklemler (döngü kapatan, ip) tümüyle; ağaç menteşelerinin sınır/motor satırları */
static void gs_pass(PhysWorld* w, JointTree* T, f32 h, f32 br, f32 ms, f32 is) {
    for (u32 k = 0; k < w->joint_count; k++) {
        Joint* j = &w->joints[k];
        Body* A = jb(w, j->a);
        Body* B = jb(w, j->b);
        if (A->inv_mass == 0.0f && B->inv_mass == 0.0f) continue;
        b32 tree = T->state[k] != 0;   /* eşitlik satırları aşağıda doğrudan; burada yalnız sınır/motor */
        if (tree && j->type != JOINT_HINGE) continue;

        if (j->type == JOINT_DISTANCE) {
            f32 len;
            Vec3 u = distance_dir(A, B, j, &len);
            j->length = len;
            b32 rigid = j->min_length == j->max_length;
            f32 C = 0.0f;
            i32 side = 0;   /* 0 çubuk, +1 alt sınır (itki >= 0), -1 üst sınır (itki <= 0) */
            if (rigid) C = len - j->min_length;
            else if (len < j->min_length) { C = len - j->min_length; side = 1; }
            else if (len > j->max_length) { C = len - j->max_length; side = -1; }
            else {
                /* aralıkta: serbest, ama yaklaşma hızına sınırı geçmeyecek kadar izin ver (spekülatif) */
                j->len_imp = 0.0f;
                continue;
            }
            Vec3 ca = v3_cross(j->ra, u), cb = v3_cross(j->rb, u);
            f32 kk = A->inv_mass + B->inv_mass + v3_dot(ca, mat3_mul_v3(&A->inv_inertia_world, ca)) +
                     v3_dot(cb, mat3_mul_v3(&B->inv_inertia_world, cb));
            f32 mass = kk > 0.0f ? 1.0f / kk : 0.0f;
            Vec3 dv = v3_sub(v3_add(B->vel, v3_cross(B->ang_vel, j->rb)), v3_add(A->vel, v3_cross(A->ang_vel, j->ra)));
            f32 cdot = v3_dot(u, dv);
            f32 lam = -mass * ms * (cdot + br * C) - is * j->len_imp;
            f32 nw = j->len_imp + lam;
            if (side > 0) nw = m_maxf(nw, 0.0f);
            if (side < 0) nw = m_minf(nw, 0.0f);
            lam = nw - j->len_imp;
            j->len_imp = nw;
            apply_lin(A, B, j->ra, j->rb, v3_scale(u, lam));
            continue;
        }

        /* menteşe: önce açısal kısıtlar (motor, sınır, eksen hizası), sonra nokta */
        if (j->type == JOINT_HINGE) {
            Vec3 aw = mat3_mul_v3(&A->rot_m, j->axis_a);
            Vec3 bw = mat3_mul_v3(&B->rot_m, j->axis_b);
            hinge_axial(j, A, B, aw, h, br, ms, is, T->state[k] != 3);
            if (tree) continue;
            Vec3 t1, t2;
            ortho_basis(aw, &t1, &t2);
            Vec3 err = v3_cross(aw, bw);
            const Vec3 ts[2] = { t1, t2 };
            for (u32 i = 0; i < 2; i++) {
                Vec3 t = ts[i];
                f32 cdot = v3_dot(v3_sub(B->ang_vel, A->ang_vel), t);
                f32 acc = v3_dot(j->ang_imp, t);
                f32 lam = -ang_mass(A, B, t) * ms * (cdot + br * v3_dot(err, t)) - is * acc;
                j->ang_imp = v3_add(j->ang_imp, v3_scale(t, lam));
                apply_ang(A, B, v3_scale(t, lam));
            }
        }

        /* nokta kısıtı (top-mafsal ve menteşe) */
        Vec3 C = v3_sub(world_anchor(B, j->lb), world_anchor(A, j->la));
        Vec3 dv = v3_sub(v3_add(B->vel, v3_cross(B->ang_vel, j->rb)), v3_add(A->vel, v3_cross(A->ang_vel, j->ra)));
        Vec3 rhs = v3_add(dv, v3_scale(C, br));
        Vec3 lam = v3_sub(v3_scale(mat3_mul_v3(&j->k_inv, rhs), -ms), v3_scale(j->lin_imp, is));
        j->lin_imp = v3_add(j->lin_imp, lam);
        apply_lin(A, B, j->ra, j->rb, lam);
    }
}

/* ağaç eşitlik satırları (doğrudan), ardından sınır/motor yeniden */
static void tree_pass(PhysWorld* w, JointTree* T, f32 h, f32 br, f32 ms, f32 is) {
    /* sınır/motor ağaçtan sonra bir kez daha: nokta kısıtının torku eksen hızını değiştirir, sınır son sözü söylemeli */
    tree_solve(w, T, br);
    for (u32 n = 0; n < T->count; n++) {
        Joint* j = &w->joints[T->jt[n].id];
        if (j->type != JOINT_HINGE || !(j->limit || j->motor)) continue;
        Body* A = jb(w, j->a);
        Body* B = jb(w, j->b);
        hinge_axial(j, A, B, mat3_mul_v3(&A->rot_m, j->axis_a), h, br, ms, is, T->state[T->jt[n].id] != 3);
    }
}

void joints_solve(PhysWorld* w, f32 h, b32 use_bias, b32 soft) {
    Soft s = joint_soft(h);
    f32 br, ms, is;
    coeffs(s, use_bias, soft, h, &br, &ms, &is);
    JointTree* T = (JointTree*)w->jtree;
    /* Ağaç eklemleri yalnız bias geçişinde çözülür: relax bias hızını silseydi merkezcil etkiyi de silerdi (sarkaç 10 s'de
     * %21 enerji kaybı); kayma toleransı aşarsa joints_stabilize konumda düzeltir. Döngü kapatan eklem varsa ağaç (tam) ile
     * Gauss-Seidel blokları JOINT_BLOCK_ITERS kez sırayla çözülür (blok Gauss-Seidel). */
    b32 tree = use_bias && T->count;
    u32 passes = tree && T->gs_count ? JOINT_BLOCK_ITERS : 1;
    for (u32 p = 0; p < passes; p++) {
        gs_pass(w, T, h, br, ms, is);
        if (tree) tree_pass(w, T, h, br, ms, is);
    }
}

/* tolerans dışı kayma (bias'ın düzeltemediği kısım); hepsi tolerans içindeyse FALSE */
static b32 excess_rows(PhysWorld* w, const JtJoint* q, f32* z) {
    b32 any = FALSE;
    f32 C[JT_ROWS];
    tree_error(w, q, C);
    for (u32 r = 0; r < q->k; r++) {
        f32 e = C[r] > JOINT_STAB_TOL ? C[r] - JOINT_STAB_TOL : C[r] < -JOINT_STAB_TOL ? C[r] + JOINT_STAB_TOL : 0.0f;
        z[r] = -e;
        any |= e != 0.0f;
    }
    return any;
}

static b32 tree_excess(PhysWorld* w, JointTree* T) {
    b32 any = FALSE;
    for (u32 n = 0; n < T->count; n++) any |= excess_rows(w, &T->jt[n], T->jt[n].z);
    u32 row = 0;
    for (u32 l = 0; l < T->loop_count; l++) {
        any |= excess_rows(w, &T->lp[l], &T->lz[row]);
        row += T->lp[l].k;
    }
    return any;
}

/* Konum güvenlik ağı (post-stabilizasyon, Cline & Pai 2003): kayma toleransı aşarsa fazlası C → ±tol'a kütle ağırlıklı
 * izdüşümle (J W Jᵀ Δλ = −C_fazla, Δx = W Jᵀ Δλ) aynı ağaçla silinir; JOINT_STAB_ITERS Newton adımı, her adımda güncel J.
 * Hızlara dokunmaz. Olağan hareketde (kayma < tol) hiç çalışmaz: bias'ın gecikmeli merkezcil etkisi (enerji) korunur;
 * aşırı kütle oranında (1000:1, 4 alt adım) bias + relax'ın büyüyen kaymasını durdurur. */
void joints_stabilize(PhysWorld* w) {
    JointTree* T = (JointTree*)w->jtree;
    for (u32 it = 0; it < JOINT_STAB_ITERS; it++) {
        if (!T->count || !tree_excess(w, T)) return;
        joints_prepare(w, 0.0f);   /* güncel kollar, satırlar, çarpanlar (ağaç aynı → sıra aynı) */
        tree_excess(w, T);
        sys_solve(w, T);
        for (u32 s = 0; s < T->slot_count; s++) {
            Body* b = &w->bodies[T->slot[s].body];
            const f32* p = T->slot[s].p1;
            b->pos = v3_add(b->pos, v3_scale(v3(p[0], p[1], p[2]), b->inv_mass));
            b->rot = quat_integrate(b->rot, mat3_mul_v3(&b->inv_inertia_world, v3(p[3], p[4], p[5])), 1.0f);
            phys_body_update_derived(b);
        }
    }
}

