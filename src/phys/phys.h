/* phys/phys.h — MATOKI fizik motoru (M9 Faz 1): rijit cisimler, küre/kutu/düzlem, SAT,
 * sweep-and-prune, warm start'lı sequential impulse. Sabit dt ile belirleyici.
 * Manifold normali A'dan B'ye; B'ye +P, A'ya −P itki uygulanır. depth > 0 = girişim,
 * depth < 0 = spekülatif boşluk (PHYS_MARGIN içinde). */
#ifndef PHYS_PHYS_H
#define PHYS_PHYS_H
#include "core/arena.h"
#include "phys/pmath.h"
#include "phys/bvh.h"

/* Sıra önemli: phys_collide çiftleri (küçük, büyük) sırasına koyar. */
typedef enum ShapeType {
    SHAPE_SPHERE,
    SHAPE_CAPSULE,   /* yerel y ekseni boyunca doğru parçası (±half_height) + radius */
    SHAPE_BOX,
    SHAPE_HULL,      /* dışbükey gövde (phys/shapes.h) */
    SHAPE_MESH,      /* statik üçgen mesh + BVH (phys/shapes.h) */
    SHAPE_PLANE      /* sonsuz, yalnızca statik: dot(n, p) = d */
} ShapeType;

struct ConvexHull;
struct TriMesh;
struct Core;

#define PHYS_MARGIN      0.02f   /* spekülatif temas mesafesi */
#define PHYS_MAX_POINTS  4
#define PHYS_COLORS      16      /* kısıt grafiği renkleri; sığmayanlar taşma rengine (skaler) */

typedef struct BodyDesc {
    ShapeType shape;
    Vec3 pos;
    Quat rot;
    Vec3 half;          /* kutu yarı boyutları */
    f32  radius;        /* küre, kapsül */
    f32  half_height;   /* kapsül çekirdek yarı boyu */
    const struct ConvexHull* hull;
    const struct TriMesh* mesh;
    Vec3 plane_n;       /* düzlem (birim) */
    f32  plane_d;
    f32  mass;          /* 0 → statik */
    f32  friction;
    f32  restitution;
    Vec3 vel, ang_vel;
} BodyDesc;

#define BODY_ASLEEP   1u
#define BODY_NO_SLEEP 2u   /* asla uyutma (ör. oyuncu aracı) */
#define BODY_BULLET   4u   /* CCD dinamik gövdelere karşı da (mermi); normalde yalnızca statiklere karşı */

typedef struct Body {
    ShapeType shape;
    u32  flags;
    f32  sleep_time;    /* eşiğin altında geçen süre (s) */
    u32  island;        /* uyutulduğu adanın kimliği */
    /* Çözücü bloğu: 16 bayt hizalı, ardışık 32 f32 (128 bayt). SIMD temas çözücüsü 8 vektör yükleyip
     * 4×4 devrikle toplar (phys/solver_simd_wasm.c); alan sırası oradaki statik denetimlerle eşleşmeli. */
    _Alignas(16) Vec3 vel;
    f32  inv_mass;
    Vec3 ang_vel;
    f32  pad0;
    Vec3 pos;
    f32  pad1;
    Mat3 inv_inertia_world;
    Mat3 rot_m;
    f32  pad2[2];
    Quat rot;
    Vec3 inv_inertia_local;   /* köşegen, asal eksenlerde (hull: hull->inertia_axes; diğerleri: yerel eksenler) */
    Vec3 half;
    f32  radius;
    f32  half_height;
    const struct ConvexHull* hull;
    const struct TriMesh* mesh;
    Vec3 plane_n;
    f32  plane_d;
    f32  friction, restitution;
    Vec3 aabb_min, aabb_max;
    Vec3 force, torque;   /* biriken dış kuvvet; her (alt) adımda entegre, adım sonunda sıfırlanır */
    /* CCD */
    f32  min_extent;      /* merkezden yüzeye en kısa uzaklık (iç yarıçap) */
    f32  max_extent;      /* merkezden en uzak yüzey noktası */
    Vec3 pos0;            /* adım başı pozu (süpürme başlangıcı) */
    Quat rot0;
    u32  proxy;           /* dünya BVH yaprağı (statik ya da dinamik ağaçta); düzlem: TREE_NULL */
} Body;

typedef struct ContactPoint {
    Vec3 pos;           /* dünya (iki yüzeyin ortası) */
    Vec3 local_a;       /* A'nın yerel uzayında (warm start eşleme) */
    f32  depth;
    f32  jn, jt1, jt2;  /* biriken itkiler */
    Vec3 ra, rb;
    f32  mass_n, mass_t1, mass_t2;
    f32  bias;          /* SI: hedef normal hız */
    /* soft step */
    Vec3 la, lb;        /* gövdelerin yerel uzayında çapa (merkeze göre) */
    f32  s0;            /* hazırlıkta ayrılma = -depth */
    f32  rel_vel;       /* hazırlıkta normal göreli hız (restitüsyon) */
    f32  max_jn;
} ContactPoint;

typedef struct Manifold {
    u32  a, b;          /* gövde indeksleri, a < b */
    u32  sub;           /* mesh üçgeni + 1 (aynı çiftin birden çok manifoldu); 0 = tek */
    Vec3 normal;        /* A → B */
    Vec3 t1, t2;
    f32  friction, restitution;
    u32  count;
    ContactPoint p[PHYS_MAX_POINTS];
} Manifold;

typedef struct PhysStats {
    u32 bodies;
    u32 pairs;          /* geniş fazdan geçen */
    u32 manifolds;
    u32 contacts;
    u32 warm_hits;      /* önceki adımdan eşleşen temas */
    u32 awake;          /* uyanık hareketli gövde */
    u32 islands;
    u32 tree_moves;     /* şişman kutusundan taşıp yeniden eklenen yaprak */
    u32 pset;           /* kalıcı çift kümesi boyu (BVH) */
    f64 broad_ms;       /* geniş faz (ağaç güncelleme + çift bulma + sıralama) */
    u32 colors;         /* kullanılan renk sayısı */
    u32 overflow;       /* taşma rengindeki manifold */
    u32 ccd_bodies;     /* süpürülen hızlı gövde */
    u32 ccd_hits;       /* TOI'ye geri çekilen */
    f64 step_ms;
    f64 collide_ms;     /* geniş + dar faz */
    f64 solve_ms;       /* ön hesap + iterasyonlar + entegrasyon */
} PhysStats;

typedef enum PhysBroadphase {
    PHYS_BP_SAP,         /* sweep-and-prune + doğrusal raycast / CCD / yumuşak aday tarama (Faz 1–6) */
    PHYS_BP_BVH          /* statik + dinamik AABB ağaçları (D21) */
} PhysBroadphase;

#define PHYS_MAX_PLANES 16
#define PHYS_TREE_MARGIN 0.1f   /* dinamik ağaç şişman AABB payı (m) */

typedef enum PhysSolver {
    PHYS_SOLVER_SI,      /* sequential impulse + Baumgarte (Faz 1) */
    PHYS_SOLVER_SOFT,    /* alt adımlı soft step + relax (Box2D v3 tarzı) */
    PHYS_SOLVER_COUNT
} PhysSolver;

/* ---- eklemler (phys/joints.c) ---- */
typedef enum JointType {
    JOINT_BALL,       /* top-mafsal: ortak nokta */
    JOINT_HINGE,      /* menteşe: ortak nokta + ortak eksen; açı sınırı, motor */
    JOINT_DISTANCE    /* mesafe: [min, max] (ip: min = 0; çubuk: min = max) */
} JointType;

typedef struct JointDesc {
    JointType type;
    u32  a, b;               /* b = PHYS_NO_BODY → dünyaya sabit */
    Vec3 anchor_a, anchor_b; /* dünya; ball/hinge'de anchor_b yok sayılır (= anchor_a) */
    Vec3 axis;               /* menteşe ekseni (dünya) */
    f32  min_length, max_length;   /* mesafe; ikisi de 0 → oluşturulduğundaki uzunlukta çubuk */
    b32  limit;
    f32  lower, upper;       /* menteşe açı sınırı (rad) */
    b32  motor;
    f32  motor_speed;        /* rad/s */
    f32  max_motor_torque;   /* N·m */
    b32  collide_connected;
} JointDesc;

typedef struct Joint {
    JointType type;
    u32  a, b;
    Vec3 la, lb;             /* yerel çapalar */
    Vec3 axis_a, axis_b;     /* yerel menteşe eksenleri */
    Vec3 ref_a, ref_b;       /* yerel açı referansı (eksene dik) */
    f32  min_length, max_length;
    b32  limit, motor, collide_connected;
    f32  lower, upper, motor_speed, max_motor_torque;
    /* çözücü durumu (adımlar arası warm start) */
    Vec3 ra, rb;
    Mat3 k_inv;
    Vec3 lin_imp;
    Vec3 ang_imp;            /* menteşe eksenine dik açısal itki (dünya vektörü) */
    f32  motor_imp;
    f32  limit_imp;
    f32  len_imp;
    f32  angle;              /* son ölçülen menteşe açısı */
    f32  length;             /* son ölçülen mesafe */
} Joint;

typedef struct PhysWorld {
    Body*     bodies;
    u32       count, cap;
    Joint*    joints;
    u32       joint_count, joint_cap;
    Body      world_body;   /* PHYS_NO_BODY eklem ucu için statik gövde */
    Vec3      gravity;
    /* gezegen çekimi (planet_gm > 0): sabit g yerine g(h) = GM / (R + h)², h = gövde merkezinin y'si (zemin y = 0),
     * yön −y. Kısa mesafede sabit g'ye iner; uzun düşüşte (h ~ R) ivme yükseklikle azalır (D32). */
    f64       planet_gm;    /* m³/s² (Dünya 3.986004e14) */
    f64       planet_radius;/* m (Dünya 6.371e6) */
    PhysSolver solver;
    u32       substeps;     /* soft: alt adım sayısı */
    f32       contact_hertz, contact_damping;
    b32       allow_sleep;
    f32       sleep_lin, sleep_ang, sleep_delay;   /* m/s, rad/s, s */
    u32*      uf_parent;    /* ada: birleşim-bul */
    f32*      uf_min_time;
    u32*      pairs;        /* geniş faz adayları (a, b) */
    u32       pair_count, pair_cap;
    u32       island_seq;
    u32       iterations;
    b32       warm_start;
    b32       speculative;  /* boşluklu temaslarda boşluk/dt kadar yaklaşmaya izin ver */
    b32       continuous;   /* CCD: hızlı gövdeleri adım sonunda ilk çarpma anına (TOI) geri çek */
    b32       mesh_edge_fix;/* mesh iç kenar düzeltmesi (aktif olmayan kenarda yüz normali) */
    f32       baumgarte;
    f32       slop;
    Manifold* cur;
    Manifold* prev;
    u32       cur_count, prev_count, manifold_cap;
    u32*      prev_index;   /* çift anahtarı → prev indeksi (açık adresli) */
    u32       prev_index_cap;
    u32*      order;        /* SAP sırası (düzlem olmayan gövdeler) */
    u32       order_count;
    /* geniş faz: çiftler her iki yolda (a < b) anahtarıyla sıralanır → manifold sırası geniş fazdan bağımsız */
    PhysBroadphase broadphase;
    DynTree   static_tree;  /* inv_mass = 0 (kutu, hull, mesh, küre...) — hiç taşınmaz */
    DynTree   dyn_tree;     /* hareketli gövdeler, şişman AABB */
    u32*      query_buf;
    u32       query_cap;
    u32*      pair_tmp;     /* radix sıralama tamponu */
    u32       plane_ids[PHYS_MAX_PLANES];
    u32       plane_count;
    f32       max_disp;     /* son adımda dinamik gövdelerin en büyük yer değiştirmesi (yumuşak aday sorgusu) */
    f32       step_dt;      /* geçerli adımın dt'si (kürelerin hıza bağlı spekülatif payı) */
    /* BVH kalıcı çift kümesi: şişman AABB'leri kesişen tüm (dinamik, dinamik|statik) çiftler. Yalnızca
     * şişman kutusundan taşıp yeniden eklenen ("taşınan") vekiller ağacı sorgular (Box2D yaklaşımı). */
    u32*      pset;         /* anahtar a·2¹⁶ + b (a < b), sırasız */
    u32       pset_count;
    u32*      pset_hash;    /* açık adresli: anahtar + 1 */
    u32       pset_hash_cap;
    b32       pset_valid;
    u32*      moved;        /* taşınan gövdeler */
    u32       moved_count;
    u8*       moved_flag;
    /* soft çözücü: kısıt grafiği renklendirmesi (D20). Aynı renkteki manifoldlar ortak dinamik gövde
     * paylaşmaz → renk içi sıra sonucu değiştirmez; SIMD 4 manifoldu birlikte çözer, skalerle bit-bit aynı. */
    b32       graph_color;  /* manifoldları renk sırasıyla çöz (kapalı: oluşturulma sırası, Faz 1–5) */
    b32       simd;         /* renk gruplarını 4'lü SIMD ile çöz (graph_color gerekir; wasm simd128) */
    u32*      color_order;  /* renk sırasına göre manifold indeksleri */
    u32       color_start[PHYS_COLORS + 2];   /* renk c: [color_start[c], color_start[c+1]); son = taşma */
    u64*      color_bits;   /* renk başına dinamik gövde bit kümesi */
    u32       color_words;
    u8*       manifold_color;
    void*     wide;         /* SIMD geniş kısıtları (solver_simd_wasm.c) */
    u32       wide_cap, wide_count;
    u32       wide_color_start[PHYS_COLORS + 1];
    PhysStats stats;
} PhysWorld;

void phys_world_init(PhysWorld* w, Arena* a, u32 max_bodies, u32 max_manifolds);
void phys_world_set_solver(PhysWorld* w, i32 solver);
/* y yüksekliğindeki çekim ivmesi (gezegen kapalıysa w->gravity) */
Vec3 phys_gravity_at(const PhysWorld* w, Vec3 pos);
/* dinamik ağacı gövdelerin güncel AABB'leriyle eşitle (BVH modunda adım içinde kendiliğinden çağrılır) */
void phys_update_tree(PhysWorld* w);
/* Sıkı AABB'si [mn, mx] ile kesişen gövdeler (düzlemler hariç), artan indeks sırasıyla. BVH modunda iki
 * ağaç sorgusu + sıralama, SAP modunda doğrusal tarama; sonuç kümesi iki modda aynı. Dönüş = bulunan
 * (max'a kesilir). Geçici tamponu (pair_tmp) kullanır: geniş faz sırasında çağrılmamalı. */
u32  phys_query_aabb(PhysWorld* w, Vec3 mn, Vec3 mx, u32* out, u32 max);
void phys_wake_all(PhysWorld* w);
f32  phys_max_penetration(const PhysWorld* w);
void phys_world_clear(PhysWorld* w);
u32  phys_add_body(PhysWorld* w, const BodyDesc* d);
void phys_step(PhysWorld* w, f32 dt);

BodyDesc phys_desc_box(Vec3 pos, Vec3 half, f32 mass);
BodyDesc phys_desc_sphere(Vec3 pos, f32 radius, f32 mass);
BodyDesc phys_desc_plane(Vec3 n, f32 d);
BodyDesc phys_desc_capsule(Vec3 pos, f32 radius, f32 half_height, f32 mass);
/* hull köşeleri hacim merkezine göredir; pos = merkezin dünya konumu */
BodyDesc phys_desc_hull(Vec3 pos, const struct ConvexHull* hull, f32 mass);
BodyDesc phys_desc_mesh(Vec3 pos, const struct TriMesh* mesh);   /* her zaman statik */

u32  phys_add_joint(PhysWorld* w, const JointDesc* d);
JointDesc phys_joint_desc(JointType type, u32 a, u32 b, Vec3 anchor);
/* eklem çapalarının şu anki ayrılığı (m) — test/istatistik */
f32  phys_joint_error(const PhysWorld* w, u32 joint);

/* Dahili (collide.c ↔ world.c): genel çekirdek çarpışması ve gövde çekirdeği */
void phys_body_core(const Body* b, struct Core* out);
u32  phys_collide_cores(const struct Core* a, const struct Core* b, Manifold* m);
/* mesh × dinamik: üçgen başına manifold yazar (sub = üçgen + 1); dönüş = yazılan manifold.
 * edge_fix: iç kenar düzeltmesi (PhysWorld.mesh_edge_fix) */
u32  phys_collide_mesh(const Body* a, const Body* b, Manifold* out, u32 max, b32 edge_fix);
/* phys_collide gibi; küre-küre ve küre-düzlemde boşluk payı PHYS_MARGIN yerine 'margin' (hıza bağlı
 * spekülatif temas: adım içinde kapanacak boşluk önceden manifold olur). Diğer çiftler phys_collide. */
u32  phys_collide_margin(const Body* a, const Body* b, Manifold* m, f32 margin);

/* ---- sorgular ve dış etkiler (phys/query.c) ---- */
typedef struct RayHit {
    u32  body;
    f32  t;          /* origin + dir * t */
    Vec3 point;
    Vec3 normal;     /* yüzeyin dış normali */
} RayHit;

#define PHYS_NO_BODY 0xFFFFFFFFu

/* dir birim olmalı. ignore = PHYS_NO_BODY veya hariç tutulacak gövde. En yakın isabeti döndürür. */
b32  phys_raycast(const PhysWorld* w, Vec3 origin, Vec3 dir, f32 max_t, u32 ignore, RayHit* out);
/* Dünya noktasında itki (statik gövdede etkisiz); uyuyan gövdeyi (adasıyla) uyandırır. */
void phys_apply_impulse_at(PhysWorld* w, u32 body, Vec3 impulse, Vec3 point);
/* Kuvvet (N): bir sonraki phys_step boyunca (soft'ta her alt adımda) uygulanır */
void phys_apply_force_at(PhysWorld* w, u32 body, Vec3 force, Vec3 point);
void phys_wake_body(PhysWorld* w, u32 body);
Vec3 phys_point_velocity(const Body* b, Vec3 point);
/* dir yönündeki bir itki için noktadaki etkin kütle (1 / (1/m + n·(I⁻¹(r×n))×r)) */
f32  phys_effective_mass(const Body* b, Vec3 point, Vec3 dir);

/* Test/araç: dar faz. a, b alanlarını çağıran doldurur; dönüş = nokta sayısı. */
u32  phys_collide(const Body* a, const Body* b, Manifold* m);
void phys_body_update_derived(Body* b);
f64  phys_kinetic_energy(const PhysWorld* w);
u32  phys_state_hash(const PhysWorld* w);

#endif
