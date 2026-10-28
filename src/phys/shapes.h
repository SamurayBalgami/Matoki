/* phys/shapes.h — karmaşık şekil verileri: dışbükey gövde (hull) ve statik üçgen mesh + BVH.
 * Hull: nokta bulutundan kaba kuvvetle (≤ 64 köşe) yüzleri çıkarılır, eş düzlemli yüzler birleşir,
 * yüz köşeleri dışarıdan bakınca CCW sıralanır; köşeler hacim merkezine taşınır (gövde konumu =
 * kütle merkezi). Mesh: ağaç yapısı AABB BVH (orta nokta bölme, yaprak ≤ 4 üçgen); AABB ve ışın sorgusu. */
#ifndef PHYS_SHAPES_H
#define PHYS_SHAPES_H
#include "core/arena.h"
#include "core/math.h"

#define HULL_MAX_VERTS 64
#define HULL_MAX_FACES 128

typedef struct HullFace {
    Vec3 n;           /* dış normal (yerel) */
    f32  d;           /* dot(n, p) = d */
    u32  first, count;/* idx[] içinde köşe dizini, dışarıdan CCW */
} HullFace;

typedef struct ConvexHull {
    Vec3*     verts;
    u32       vcount;
    HullFace* faces;
    u32       fcount;
    u32*      idx;
    u32       icount;
    Vec3      offset;   /* girdi koordinatlarında hacim merkezi (köşeler buna göre kaydırıldı) */
    f32       volume;
    Vec3      aabb_min, aabb_max;
    Vec3      inertia;        /* birim kütle başına asal eylemsizlik momentleri */
    Mat3      inertia_axes;   /* asal eksenler (sütunlar, yerel uzayda; det = +1) */
    f32       inner_radius;   /* merkezden en yakın yüze uzaklık (CCD) */
    f32       outer_radius;   /* merkezden en uzak köşe (CCD) */
} ConvexHull;

/* NULL: dejenere (düzlemsel / < 4 nokta / sınır aşımı) */
ConvexHull* hull_create(Arena* a, const Vec3* pts, u32 n);
/* Simetrik 3×3 (sütun-öncelikli) özayrışım, Jacobi: s = Q diag(eig) Qᵀ; Q sütunları özvektör, det(Q) = +1 */
void mat3_sym_eigen(const Mat3* s, Mat3* q, Vec3* eig);
/* Birim küp [-1,1]^3 (kutu çekirdeği için; ölçek yarı boyutlarla) */
const ConvexHull* hull_unit_cube(void);

typedef struct BvhNode {
    Vec3 mn, mx;
    u32  left;        /* iç düğüm: çocuklar left, left+1 */
    u32  first;       /* yaprak: üçgen aralığı */
    u32  count;       /* 0 = iç düğüm */
} BvhNode;

typedef struct TriMesh {
    Vec3*    verts;
    u32      vcount;
    u32*     idx;     /* üçgen başına 3; BVH sırasına göre */
    u32      tri_count;
    BvhNode* nodes;
    u32      node_count;
    u8*      edge_flags;   /* üçgen başına: bit k = kenar (v_k → v_k+1) aktif (dışbükey / sınır) */
    Vec3     aabb_min, aabb_max;
} TriMesh;

/* Komşu üçgenle dihedral açısı bundan küçük (cos büyük) dışbükey kenarlar pasif sayılır (~5°) */
#define TRIMESH_ACTIVE_EDGE_COS 0.996f

/* Komşuluk köşe indeksleriyle bulunur: komşu üçgenler köşeleri paylaşmalı (aynı konumdaki ayrı
 * köşeler kenar komşusu sayılmaz → o kenar aktif kalır) */
TriMesh* trimesh_create(Arena* a, const Vec3* verts, u32 vcount, const u32* idx, u32 tri_count);
/* Yerel uzayda AABB ile kesişen üçgenler (indeksleri out'a; dönüş = bulunan, max'a kesilir) */
u32 trimesh_query_aabb(const TriMesh* m, Vec3 mn, Vec3 mx, u32* out, u32 max);
/* Yerel uzayda ışın; çift yüzlü; normal ışına bakar */
b32 trimesh_raycast(const TriMesh* m, Vec3 o, Vec3 d, f32 tmax, f32* t, Vec3* n, u32* tri);
static inline void trimesh_tri(const TriMesh* m, u32 t, Vec3* a, Vec3* b, Vec3* c) {
    *a = m->verts[m->idx[t * 3]];
    *b = m->verts[m->idx[t * 3 + 1]];
    *c = m->verts[m->idx[t * 3 + 2]];
}

#endif
