/* phys/gjk.h — destek fonksiyonlu dışbükey çekirdekler, GJK mesafesi ve EPA girişimi.
 * Her şekil "çekirdek + yarıçap": küre = nokta + r, kapsül = doğru parçası + r, kutu / hull /
 * mesh üçgeni = çokyüzlü + 0. GJK çekirdekler arası en yakın noktaları verir; çekirdekler kesişiyorsa
 * EPA en küçük ayırma normalini ve derinliği verir. Normal kuralı: A'dan B'ye. */
#ifndef PHYS_GJK_H
#define PHYS_GJK_H
#include "phys/shapes.h"

enum { CORE_POINT, CORE_SEGMENT, CORE_POLY };

typedef struct Core {
    u32  kind;
    f32  radius;
    Vec3 p0, p1;               /* nokta / doğru parçası uçları (dünya) */
    const ConvexHull* hull;    /* çokyüzlü: yerel köşeler ve yüzler */
    Vec3 scale;                /* yerel köşe ölçeği (kutu: yarı boyutlar; hull: 1) */
    Vec3 pos;
    Mat3 rot;
    /* üçgen için yerinde depolanan çokyüzlü */
    Vec3       tri_v[3];
    HullFace   tri_f[2];
    u32        tri_i[6];
    ConvexHull tri_h;
} Core;

void core_triangle(Core* c, Vec3 a, Vec3 b, Vec3 cc);
Vec3 core_support(const Core* c, Vec3 dir);
/* Çokyüzlü köşesi / yüz normali dünya uzayında */
static inline Vec3 core_vertex(const Core* c, u32 i) {
    Vec3 v = c->hull->verts[i];
    return v3_add(c->pos, mat3_mul_v3(&c->rot, v3(v.x * c->scale.x, v.y * c->scale.y, v.z * c->scale.z)));
}
static inline Vec3 core_face_normal(const Core* c, u32 f) {
    /* eksen ölçeklemesi (kutu) yüz normallerini değiştirmez çünkü yüzler eksen hizalı; hull'da ölçek 1 */
    return mat3_mul_v3(&c->rot, c->hull->faces[f].n);
}

typedef struct GjkResult {
    b32  overlap;
    f32  dist;
    Vec3 pa, pb;          /* çekirdekler üzerinde en yakın noktalar */
    u32  iters;
    u32  exit;            /* 0 yakınsadı, 1 tekrar eden köşe, 2 tur sınırı, 3 içeride, 4 ilerleme durdu */
    /* EPA için son simplex */
    u32  n;
    Vec3 w[4], a[4], b[4];
} GjkResult;

void gjk_distance(const Core* A, const Core* B, GjkResult* r);

typedef struct EpaResult {
    b32  ok;
    Vec3 normal;          /* A → B */
    f32  depth;           /* çekirdek girişimi */
    Vec3 pa, pb;
} EpaResult;

void epa_penetration(const Core* A, const Core* B, const GjkResult* g, EpaResult* r);

#endif
