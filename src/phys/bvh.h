/* phys/bvh.h — dinamik AABB ağacı (Box2D v2.4 b2DynamicTree'nin 3B karşılığı): geniş faz, raycast, CCD.
 * Yapraklar "şişman" AABB (sıkı kutu ± margin) taşır; gövde şişman kutusundan çıkmadıkça ağaç değişmez.
 * Ekleme: yüzey alanı maliyetiyle kardeş seçimi; yukarı yürürken AVL tarzı döndürmelerle denge.
 * Düğümler sabit kapasiteli havuzda (arena), boş liste ile; işlem sırası aynıysa ağaç aynı → belirleyici. */
#ifndef PHYS_BVH_H
#define PHYS_BVH_H
#include "core/arena.h"
#include "core/math.h"

#define TREE_NULL 0xFFFFFFFFu

typedef struct TreeNode {
    Vec3 mn, mx;      /* şişman AABB (iç düğüm: çocukların birleşimi) */
    u32  parent;      /* boş listede: sonraki boş düğüm */
    u32  child1, child2;
    i32  height;      /* yaprak 0, boş −1 */
    u32  body;        /* yaprak: gövde indeksi */
} TreeNode;

typedef struct DynTree {
    TreeNode* nodes;
    u32       cap, count;   /* count: kullanılan düğüm */
    u32       root, free_list;
    f32       margin;
    u32       leaves;
} DynTree;

void tree_init(DynTree* t, Arena* a, u32 cap, f32 margin);
void tree_clear(DynTree* t);
u32  tree_insert(DynTree* t, Vec3 mn, Vec3 mx, u32 body);   /* dönüş: vekil (yaprak düğüm) */
void tree_remove(DynTree* t, u32 proxy);
/* sıkı kutu şişman kutunun dışına taştıysa yeniden ekler (dönüş TRUE) */
b32  tree_move(DynTree* t, u32 proxy, Vec3 mn, Vec3 mx);
/* şişman kutusu [mn, mx] ile kesişen yaprakların gövdeleri (sırasız); dönüş = bulunan (max'a kesilir) */
u32  tree_query(const DynTree* t, Vec3 mn, Vec3 mx, u32* out, u32 max);
/* ışın: her aday yaprakta fn çağrılır; fn tmax'ı küçültebilir (budama) */
typedef void (*TreeRayFn)(void* ctx, u32 body, f32* tmax);
void tree_raycast(const DynTree* t, Vec3 o, Vec3 d, f32 tmax, TreeRayFn fn, void* ctx);

/* test / istatistik */
i32  tree_height(const DynTree* t);
f32  tree_area_ratio(const DynTree* t);   /* iç düğüm alanları toplamı / kök alanı */
b32  tree_validate(const DynTree* t);     /* ebeveyn bağları, yükseklikler, kapsama, yaprak sayısı */

#endif
