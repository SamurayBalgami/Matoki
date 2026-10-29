/* sim/sim.h — Deterministik sabit adımlı simülasyon çekirdeği, rollback ve replay (M16_A).
 * Oyun durumu tek bir bitişik Arena bölgesinde (state) tutulur; anlık görüntü (snapshot)
 * bu bölgenin halka tampona düz memcpy kopyasıdır. Durum içindeki çapraz referanslar ham
 * işaretçi (T*) yerine dizi indeksi, EcsEntity tutamağı veya bölge tabanından bayt ofseti
 * (SimOffset) kullanmalıdır; böylece snapshot'lar taşınabilir ve durum özeti (hash) adres-bağımsız olur.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/sim/sim.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef SIM_SIM_H
#define SIM_SIM_H

#include "core/types.h"
#include "core/arena.h"
#include "phys/phys.h"
#include "scene/ecs.h"

#define SIM_DEFAULT_HZ             60u
#define SIM_DEFAULT_DT             (1.0f / 60.0f)
#define SIM_MAX_PLAYERS            8u
#define SIM_DEFAULT_SNAPSHOT_RING  16u
#define SIM_MAX_SNAPSHOT_RING      64u
#define SIM_INPUT_RING             256u
#define SIM_MAX_FRAME_STEPS        64u

#define SIM_HASH_SEED              0xCBF29CE484222325ULL
#define SIM_REPLAY_MAGIC           0x524D4953u   /* "SIMR" (little-endian) */
#define SIM_REPLAY_VERSION         1u

/* ----------------------------------------------------------------------------
 * Ofset / İndeks Modeli (Ham İşaretçi Yerine Ofset Kullanımı)
 *
 * Durum bölgesi (sim->state) içinde ham C işaretçisi (void*, Entity* vb.) tutmak yasaktır:
 *  1) İki eş (peer) simülasyonda veya wasm32 vs 64-bit host'ta taban adresleri farklıdır;
 *     ham işaretçi baytları doğrudan durum özetini (sim_hash) bozar.
 *  2) Snapshot geri yüklemesi veya ağ üzerinden durum aktarımı ham işaretçileri geçersiz kılar.
 *
 * Bunun yerine:
 *  - Sabit diziler içinde u32 eleman indeksi veya nesilli EcsEntity tutamağı kullanın.
 *  - Durum arenası içinde dinamik alt bölgeler için SimOffset (durum tabanından bayt ofseti)
 *    ve SIM_PTR / SIM_PTR_CONST makrolarını kullanın.
 * ---------------------------------------------------------------------------- */

typedef u32 SimOffset;
#define SIM_NULL_OFFSET U32_MAX

static inline void* sim_offset_ptr(void* state_base, SimOffset off) {
    return (off == SIM_NULL_OFFSET || !state_base) ? NULL : ((u8*)state_base + (usize)off);
}

static inline const void* sim_offset_ptr_const(const void* state_base, SimOffset off) {
    return (off == SIM_NULL_OFFSET || !state_base) ? NULL : ((const u8*)state_base + (usize)off);
}

static inline SimOffset sim_ptr_offset(const void* state_base, const void* ptr) {
    if (!state_base || !ptr) return SIM_NULL_OFFSET;
    return (SimOffset)((const u8*)ptr - (const u8*)state_base);
}

#define SIM_PTR(base, T, off)       ((T*)sim_offset_ptr((base), (off)))
#define SIM_PTR_CONST(base, T, off) ((const T*)sim_offset_ptr_const((base), (off)))

/* ----------------------------------------------------------------------------
 * Oyuncu Girdisi (16 bayt, dolgusuz, yalnızca tamsayı alanlar)
 * ---------------------------------------------------------------------------- */

#define SIM_BTN_UP       (1u << 0)
#define SIM_BTN_DOWN     (1u << 1)
#define SIM_BTN_LEFT     (1u << 2)
#define SIM_BTN_RIGHT    (1u << 3)
#define SIM_BTN_JUMP     (1u << 4)
#define SIM_BTN_FIRE     (1u << 5)
#define SIM_BTN_ACTION   (1u << 6)
#define SIM_BTN_SPRINT   (1u << 7)

typedef struct {
    u32 buttons;   /* SIM_BTN_* bit maskesi */
    i16 move_x;    /* yatay hareket ekseni (-32767 .. 32767) */
    i16 move_y;    /* ileri/geri hareket ekseni (-32767 .. 32767) */
    i16 look_dx;   /* bakış yaw deltası */
    i16 look_dy;   /* bakış pitch deltası */
    u32 aux;       /* silah / yetenek / ek komut kodu */
} SimInput;

_Static_assert(sizeof(SimInput) == 16, "SimInput 16 byte olmali");

static inline SimInput sim_input_zero(void) {
    SimInput in = { 0u, 0, 0, 0, 0, 0u };
    return in;
}

static inline b32 sim_input_eq(SimInput a, SimInput b) {
    return a.buttons == b.buttons &&
           a.move_x  == b.move_x  &&
           a.move_y  == b.move_y  &&
           a.look_dx == b.look_dx &&
           a.look_dy == b.look_dy &&
           a.aux     == b.aux;
}

/* ----------------------------------------------------------------------------
 * Geri Çağrılar ve Yapılandırma
 * ---------------------------------------------------------------------------- */

/* Tek bir sabit zaman adımını (dt = sim->fixed_dt) ilerleten deterministik simülasyon fonksiyonu */
typedef void (*SimStepFn)(void* state, const SimInput* inputs, u32 player_count, f32 dt, void* user);

/* İsteğe bağlı özel durum özeti (hash) fonksiyonu. NULL ise sim->state baytları doğrudan özetlenir. */
typedef u64 (*SimHashFn)(const void* state, u32 state_size, void* user);

typedef struct {
    u32       state_size;    /* oyun durumu bölgesinin bayt boyutu (> 0) */
    u32       player_count;  /* oyuncu sayısı (1 .. SIM_MAX_PLAYERS, 0 ise 2) */
    u32       snapshot_cap;  /* snapshot halka kapasitesi (2'nin kuvveti, 0 ise 16) */
    f32       fixed_dt;      /* sabit adım süresi (0 ise 1/60 s) */
    SimStepFn step_fn;       /* deterministik adım geri çağrısı */
    SimHashFn hash_fn;       /* NULL ise ham state bölgesi özetlenir */
    void*     user;          /* salt-okunur yardımcı bağlam (statik mesh/BVH vb.) */
} SimConfig;

typedef struct {
    u32 frame;        /* bu yuvadaki girdinin kare numarası (boşsa U32_MAX) */
    b32 confirmed;    /* TRUE: gerçek girdi, FALSE: tahmin edilen girdi */
    SimInput input;
} SimInputSlot;

typedef struct {
    u32   frame;      /* anlık görüntünün alındığı kare başlangıcı (boşsa U32_MAX) */
    usize arena_used; /* o kare başındaki state_arena.used değeri */
    u8*   data;       /* [state_size] baytlık durum kopyası */
} SimSnapshotSlot;

typedef struct {
    u64 total_frames;        /* ileri doğru simüle edilen toplam kare */
    u64 rollback_count;      /* gerçekleşen toplam geri sarma (rollback) sayısı */
    u64 resim_frames;        /* geri sarma sırasında yeniden simüle edilen toplam kare */
    u64 predicted_inputs;    /* tahmin edilen (tekrarlanan) oyuncu girdisi sayısı */
    u64 mispredicted_inputs; /* gerçek girdi geldiğinde tahmini yanlış çıkan girdi sayısı */
    u32 max_rollback_depth;  /* tek seferde geri sarılan en büyük kare derinliği */
} SimStats;

struct SimReplay;

typedef struct {
    u8*              state;          /* aktif oyun durumu bölgesi [state_size] */
    u32              state_size;
    Arena            state_arena;    /* durum bölgesi üzerinde alt ayırma arenası */

    u32              player_count;
    f32              fixed_dt;
    f32              accum;
    u32              current_frame;  /* sıradaki simüle edilecek kare (0, 1, 2, ...) */
    u32              rollback_frame; /* bekleyen en eski geri sarma karesi (yoksa U32_MAX) */

    SimStepFn        step_fn;
    SimHashFn        hash_fn;
    void*            user;

    u32              snapshot_cap;   /* 2'nin kuvveti (ör. 16) */
    u32              snapshot_mask;  /* snapshot_cap - 1 */
    SimSnapshotSlot* snap_ring;      /* [snapshot_cap] */
    u8*              snap_pool;      /* [snapshot_cap * state_size] */

    /* Oyuncu başına kare numaralı girdi halkası */
    SimInputSlot     input_ring[SIM_MAX_PLAYERS][SIM_INPUT_RING];

    /* Son SIM_INPUT_RING karenin simülasyon sonrası durum özetleri (desync tespiti için) */
    u64              frame_hashes[SIM_INPUT_RING];
    u32              frame_hash_nums[SIM_INPUT_RING];

    struct SimReplay* replay_rec;    /* bağlıysa her gerçek/simüle kareyi kaydeder */
    SimStats         stats;
} Sim;

/* ----------------------------------------------------------------------------
 * Girdi Kaydı ve Oynatma (Replay)
 * ---------------------------------------------------------------------------- */

typedef struct SimReplay {
    u32       seed;
    u32       player_count;
    u32       frame_count;
    u32       max_frames;
    f32       fixed_dt;
    SimInput* inputs;        /* [max_frames * player_count] kare-öncelikli düz dizi */
} SimReplay;

/* ----------------------------------------------------------------------------
 * Simülasyon Yaşam Döngüsü ve Adımlama API'si
 * ---------------------------------------------------------------------------- */

void      sim_init(Sim* sim, Arena* a, const SimConfig* cfg);
void      sim_reset(Sim* sim);

void*     sim_state(Sim* sim);
const void* sim_state_const(const Sim* sim);
Arena*    sim_state_arena(Sim* sim);
SimOffset sim_state_alloc(Sim* sim, u32 size, u32 align);

/* Anlık görüntü (snapshot) kaydetme ve geri yükleme (düz memcpy) */
void      sim_save_snapshot(Sim* sim, u32 frame);
b32       sim_restore_snapshot(Sim* sim, u32 frame);

/* Oyuncu girdisi gönderme:
 * frame < current_frame ise gecikmeli gerçek girdidir; daha önce kullanılan (tahmini)
 * girdiden farklıysa rollback_frame işaretlenir. */
b32       sim_submit_input(Sim* sim, u32 player, u32 frame, SimInput input);

/* Verilen oyuncu ve kare için girdi okuması (gerçek yoksa önceki kareden tahmin eder) */
SimInput  sim_get_input(const Sim* sim, u32 player, u32 frame, b32* out_confirmed);

/* Bekleyen geri sarmayı (varsa) en eski yanlış tahmin karesine dönüp current_frame'e
 * kadar yeniden simüle ederek çözer. Dönüş: yeniden simüle edilen kare sayısı. */
u32       sim_resolve_rollback(Sim* sim);

/* Belirtilen hedef kareye zorunlu geri sarar ve current_frame'e kadar yeniden simüle eder.
 * Dönüş: başarılıysa TRUE. */
b32       sim_rollback_to(Sim* sim, u32 target_frame);

/* Tek bir sabit zaman adımı (current_frame) ilerletir (önce bekleyen rollback varsa çözer). */
void      sim_step(Sim* sim);

/* Değişken dt'yi biriktirip sabit adımlarla ilerletir; koşulan adım sayısını döner. */
u32       sim_update(Sim* sim, f32 dt);

/* ----------------------------------------------------------------------------
 * Durum Özeti (Hash) ve Senkron Kaybı (Desync) Tespiti
 * ---------------------------------------------------------------------------- */

u64       sim_hash_init(void);
u64       sim_hash_update(u64 h, const void* data, usize len);
u64       sim_hash_final(u64 h);
u64       sim_hash(const void* data, usize len);

u64       sim_hash_u32(u64 h, u32 v);
u64       sim_hash_f32(u64 h, f32 v);
u64       sim_hash_vec3(u64 h, Vec3 v);

/* PhysWorld (bizim fizik: sim_det.c) ve EcsWorld için işaretçi adreslerinden bağımsız kanonik durum özetleyiciler */
u64       sim_hash_phys(u64 h, const PhysWorld* w);
u64       sim_hash_ecs(u64 h, const EcsWorld* w);

/* Mevcut durumun 64-bit özetini hesaplar */
u64       sim_state_hash(const Sim* sim);

/* Geçmiş bir karenin (simülasyon/rollback sonrası) kaydedilmiş özetini döner; yoksa 0 */
u64       sim_frame_hash(const Sim* sim, u32 frame);

/* Belirtilen karede uzak eşin özeti ile yerel özeti karşılaştırır; farklıysa TRUE (desync!) döner */
b32       sim_check_desync(const Sim* sim, u32 frame, u64 remote_hash);

/* ----------------------------------------------------------------------------
 * Replay API'si
 * ---------------------------------------------------------------------------- */

void            sim_replay_init(SimReplay* rep, Arena* a, u32 player_count,
                                u32 max_frames, u32 seed, f32 fixed_dt);
void            sim_replay_clear(SimReplay* rep);
void            sim_attach_replay(Sim* sim, SimReplay* rep);
b32             sim_replay_record(SimReplay* rep, u32 frame, const SimInput* inputs, u32 player_count);
const SimInput* sim_replay_get(const SimReplay* rep, u32 frame);

/* İkili (binary) serileştirme ve geri yükleme */
u32             sim_replay_save(const SimReplay* rep, u8* buf, u32 cap);
b32             sim_replay_load(SimReplay* rep, Arena* a, const u8* buf, u32 len);

/* Kaydedilmiş tüm kareleri sim üzerinde baştan sona oynatır; her karenin özetini out_hashes'e yazar */
b32             sim_replay_run(Sim* sim, const SimReplay* rep, u64* out_hashes, u32 max_hashes);

/* ----------------------------------------------------------------------------
 * Belirlenimcilik sahnesi (sim/sim_det.c, M13 faz 5): onların F0_G sahnesinin bizim fizik + ECS ile karşılığı.
 * Durum bölgesinde PhysWorld (soft çözücü, SIMD renkli) + EcsWorld; 2 oyuncu kutusunu girdiyle iter / zıplatır,
 * atış tuşu sandığa itki uygular. Tüm ayırmalar durum arenasından → anlık görüntü düz kopya.
 * ---------------------------------------------------------------------------- */

#define SIM_DET_PLAYERS       2u
#define SIM_DET_DEFAULT_TICKS 10000u
#define SIM_DET_DEFAULT_SEED  0x20261029u

typedef struct {
    u32 total_ticks;
    u64 initial_hash;
    u64 final_hash;
    u64 combined_hash;         /* tüm tick hash'lerinin kümülatif FNV-1a karması */
    u64 checkpoint_hashes[10]; /* her total_ticks/10 tick'te durum özeti */
    b32 has_nan;
} SimDetResult;

u32          sim_det_state_size(void);          /* durum bölgesi (bayt) */
void         sim_det_config(SimConfig* cfg);    /* state_size, oyuncu, adım ve özet fonksiyonları */
void         sim_det_setup(Sim* sim, u32 seed); /* sim_reset sonrası sahneyi kurar */
SimInput     sim_det_input(u32 player, u32 frame, u32 seed);   /* betiklenmiş oyuncu girdisi */
b32          sim_det_has_nan(const Sim* sim);
SimDetResult sim_det_run(Arena* a, u32 total_ticks, u32 seed, u64* out_hashes, u32 max_hashes);

#endif
