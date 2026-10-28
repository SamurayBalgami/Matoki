/* phys/soft.h — XPBD yumuşak cisimler: kumaş (mesafe kısıtları) ve jöle (tetrahedron hacim + kenar).
 * "Küçük adımlar" (Macklin 2019): adım başına N alt adım, her alt adımda tek Gauss-Seidel turu, λ sıfırdan.
 * Uyum (compliance) α = 1/k [m/N]; α̃ = α/h². Parçacıklar rijit dünyayla (phys/phys.h) iki yönlü çarpışır:
 * gövde pozu adım başı → sonu arası alt adımlara bölünür, parçacık dışarı itilir, gövdeye eşit ve ters itki.
 * Sıra sabit (dizi sırası) → belirleyici. */
#ifndef PHYS_SOFT_H
#define PHYS_SOFT_H
#include "core/arena.h"
#include "phys/phys.h"

enum { SOFT_STRETCH, SOFT_SHEAR, SOFT_BEND, SOFT_EDGE, SOFT_TETHER /* tek yönlü: yalnızca l > rest iken */ };

typedef struct SoftDist {
    u32 a, b;
    f32 rest;
    f32 compliance;
    u32 kind;   /* SOFT_* */
} SoftDist;

typedef struct SoftTet {
    u32 v[4];
    f32 rest_vol;
} SoftTet;

typedef struct SoftBody {
    u32       count;
    Vec3*     x;          /* konum */
    Vec3*     prev;       /* alt adım başı konum */
    Vec3*     v;          /* hız */
    f32*      w;          /* ters kütle; 0 = iğneli (sabit) */
    SoftDist* dist;
    u32       dist_count;
    SoftTet*  tets;
    u32       tet_count;
    f32       volume_compliance;
    u32*      tris;       /* yüzey üçgenleri (çizim, rüzgâr); dışarıdan CCW */
    u32       tri_count;
    f32       radius;     /* parçacık çarpışma yarıçapı */
    f32       friction;
    f32       damping;    /* hız sönümü (1/s) */
    f32       drag;       /* rüzgâr: normal yönlü sürükleme katsayısı (½ρC_d); 0 = rüzgârsız */
    f32       mass;       /* toplam (iğnesiz parçacıklar) */
    Vec3      aabb_min, aabb_max;
    u32       contacts;   /* son adımda rijit temas eden parçacık-alt adım sayısı */
    /* Faz 8: kumaş */
    Vec3*     x0;         /* dinlenme konumları (öz-çarpışmada dinlenmede yakın çiftler hariç) */
    b32       self_collide;   /* parçacık-parçacık öz-çarpışma (uzamsal karma) */
    b32       tri_collide;    /* rijit gövde ↔ yüzey üçgeni teması (yavaş gövdeler; GJK/EPA dar faz) */
    b32       catch_fast;     /* hızlı gövdeyi (> 5 m/s) ağ gibi yakala: iz silindiri + iki yönlü tutma */
    u32*      hash_start; /* karma tablosu: hücre başlangıçları (hash_cap + 1) */
    u32*      hash_items;
    u32       hash_cap;
} SoftBody;

/* hızlı rijit gövdenin yakaladığı kumaş parçacığı (ağ gibi): gövdeye göre sabit, kumaş gerginliği itkiyle
 * gövdeye döner; gövde yavaşlayınca / geri dönünce bırakılır */
typedef struct SoftAttach {
    u32  soft, particle, body;
    Vec3 offset;   /* parçacık − gövde merkezi (dünya, yakalama anında) */
    Vec3 dir;      /* yakalama anındaki hareket yönü */
} SoftAttach;

#define SOFT_MAX_ATTACH 256

typedef struct SoftWorld {
    SoftBody** bodies;
    u32        count, cap;
    SoftAttach* attach;
    u32        attach_count;
    u32        substeps;
    Vec3       gravity;
    Vec3       wind;      /* m/s; kumaşa etkir (drag > 0) */
    b32        two_way;   /* dinamik rijit gövdelere itki uygula */
    f64        step_ms;
    u32        rigid_contacts, soft_contacts;
    u32        tri_contacts;   /* rijit ↔ kumaş üçgeni (alt adım toplamı) */
    u32        self_contacts;  /* kumaş öz-çarpışma çifti (alt adım toplamı) */
} SoftWorld;

void soft_world_init(SoftWorld* sw, Arena* a, u32 cap);
u32  soft_world_add(SoftWorld* sw, SoftBody* b);
/* rijit dünya bu dt ile az önce adımlandıysa (pos0/rot0 adım başı) gövde pozları alt adımlara bölünür */
void soft_world_step(SoftWorld* sw, PhysWorld* rigid, f32 dt);

/* Kumaş: origin köşesinden du (nu parçacık) ve dv (nv parçacık) kenarları boyunca ızgara.
 * Kısıtlar: yapısal (komşu), kesme (köşegen), bükülme (iki ötedeki). Öz-çarpışma ve rijit-üçgen teması açık;
 * öz-çarpışma için parçacık alt adımda yarıçaptan fazla ilerleyemez (hız sınırı r/h). */
SoftBody* soft_cloth(Arena* a, Vec3 origin, Vec3 du, Vec3 dv, u32 nu, u32 nv, f32 mass,
                     f32 stretch_compliance, f32 shear_compliance, f32 bend_compliance);
/* Jöle küp: dış boyut size (parçacık kafesi size·cells/(cells+1), yarıçap kafes adımının yarısı → parçacık
 * zarfı tam size), kenar başına cells hücre (her hücre 5 tetrahedron). Çizimde yüzey normal boyunca
 * radius kadar dışarı itilmeli. */
SoftBody* soft_jelly(Arena* a, Vec3 center, f32 size, u32 cells, f32 mass, f32 edge_compliance, f32 volume_compliance);
/* parçacığı sabitle (ters kütle 0) */
void soft_pin(SoftBody* b, u32 i);
/* Uzun menzilli bağlar (Müller ve ark. 2012): iğnelerden sonra çağrılır; her serbest parçacık en yakın
 * iğneye dinlenme uzaklığından fazla uzaklaşamaz. Az iterasyonda zincir boyu uzamayı keser. Kapasite: arena. */
void soft_add_tethers(Arena* a, SoftBody* b);
void soft_set_velocity(SoftBody* b, Vec3 v);

f32  soft_volume(const SoftBody* b);          /* tetrahedronların işaretli hacim toplamı */
f32  soft_rest_volume(const SoftBody* b);
f32  soft_max_stretch(const SoftBody* b);     /* en büyük |l − l0| / l0 (yapısal / kenar kısıtları) */
Vec3 soft_momentum(const SoftBody* b);
u32  soft_hash(const SoftWorld* sw);

#endif
