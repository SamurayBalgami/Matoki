/* gfx3d/demo.c — 3D demo: wasm export'ları ve sahneler.
 * Sahne 1: dönen tuğla küp + zemin + 1000 küplük ızgara (kırpma/derinlik testi).
 * Sahne 2: ufka uzanan dama tahtası zemin; M tuşu mipmap'i açar/kapatır (moiré). Sahne 7: FFT okyanus,
 * sahne 8: üst üste düşen toplar (gezegen çekimi g(h)), sahne 9: iskelet animasyonu (ECS + glTF, MatokiEngine'den).
 * JS (web/3d.js) yalnızca girişi iletir ve framebuffer'ı canvas'a kopyalar. */
#include "core/arena.h"
#include "core/embed.h"
#include "core/jobs.h"
#include "core/fmt.h"
#include "core/mem.h"
#include "core/sys.h"
#include "matoki/matoki.h"
#include "phys/phys.h"
#include "phys/character.h"
#include "phys/shapes.h"
#include "phys/vehicle.h"
#include "phys/soft.h"
#include "phys/ocean.h"
#include "phys/dem.h"
#include "core/rng.h"

#define ROCK_KINDS 4

#define PHYS_DT (1.0f / 60.0f)
#define KEY_G 71
#define KEY_T 84
#define KEY_V 86
#define KEY_N 78
#define KEY_K 75
#define KEY_Y 89
#define KEY_O 79
#define KEY_P 80
#define KEY_U 85
#define KEY_J 74
#define KEY_F2 113
#define KEY_B 66

#define SHADOW_SIZE   1024
#define SHADOW_RADIUS 22.0f   /* m: kameranın önünde gölge alan küre */
#define SHADOW_BIAS_M 0.08f   /* m: ≈ 2 texel */

/* sahne 8: 3 sütun × 3 değen top, Hertz temaslı DEM (D34) */
#define DROP_COLS  3
#define DROP_BALLS 3
#define DROP_RESET 7.0f

/* sahne 4 oyun modu */
enum { PLAY_DRIVE, PLAY_WALK, PLAY_FLY, PLAY_COUNT };
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "gfx3d/hud.h"
#include "gfx3d/shadow.h"
#include "img/image.h"
#include "img/procgen.h"
#include "scene/transform.h"
#include "anim/clip.h"
#include "anim/skin.h"
#include "asset/gltf.h"
#include "audio/audio.h"
#include "audio/impact.h"
#include "ui/editor.h"
#include "gpu/gcmd.h"

/* sahne 9: glTF kolu (bend_linear / STEP / CUBICSPLINE klipleri) + ECS hiyerarşisine bağlı dokunaçlar */
#define ANIM_ARMS   3
#define TENTACLES   8
#define TENT_JOINTS 10
#define TENT_RINGS  41
#define TENT_SEGS   12
#define TENT_LEN    4.0f

#define GRID_N 10
#define GRID_COUNT (GRID_N * GRID_N * GRID_N)

/* engine_stats() dizisinin yerleşimi (web/3d.js ile eşleşmeli) */
enum {
    ST_SCENE, ST_MIP, ST_CAM_MODE,
    ST_OBJ_IN, ST_OBJ_CULLED,
    ST_TRI_IN, ST_TRI_REJECTED, ST_TRI_NEAR, ST_TRI_GUARD, ST_TRI_EMITTED,
    ST_TRI_BACKFACE, ST_TRI_DEGEN, ST_TRI_DRAWN,
    ST_QUADS, ST_PIXELS, ST_FRAME_US, ST_ARENA_KB,
    ST_CAM_X, ST_CAM_Y, ST_CAM_Z,   /* ×100 (i32 olarak) */
    ST_RASTER_MODE, ST_BIN_ITEMS, ST_BIN_FULL, ST_SHADER_MODE,
    ST_PHYS_BODIES, ST_PHYS_CONTACTS, ST_PHYS_US, ST_PHYS_STEPS, ST_PHYS_AWAKE,
    ST_PLAY_MODE, ST_CAR_KMH, ST_CAR_WHEELS, ST_CHAR_GROUNDED,
    ST_SOFT_PARTICLES, ST_SOFT_US, ST_SOFT_CONTACTS,
    ST_CLEAR_US, ST_GEO_US, ST_FLUSH_US, ST_TOTAL_US,   /* kare profili (µs) */
    ST_PRELUDES,                                         /* çizim önsözü çağrısı */
    ST_MSAA, ST_RESOLVE_US,                              /* örnek sayısı, MSAA çözümleme (µs) */
    ST_VERT_MODE, ST_VERTS, ST_BATCHES,                  /* köşe aşaması: 0 C, 1 Matoki; köşe; geometri partisi */
    ST_VERT_US, ST_SETUP_US,                             /* köşe aşaması, üçgen kurulumu (µs) */
    ST_DRAWS_DROPPED, ST_TRIS_DROPPED,                   /* bellek dolduğu için atlanan çizim / üçgen (bu kare) */
    ST_FRAME_KB, ST_MEM_FAILS,                           /* kare arenası tepe (KB), toplam başarısız ayırma */
    ST_HUD_US,                                           /* motor içi panel çizimi (µs; kapalıyken 0) */
    ST_ANIM_US,                                          /* sahne 9: iskelet + köşe kaplama (µs) */
    ST_AUDIO_US, ST_AUDIO_VOICES, ST_IMPACTS,            /* ses: sentez (µs), çalan ses, toplam vuruş sesi */
    ST_AUDIO_UNDERRUNS,                                  /* worklet'in boş bulduğu toplam kare (kesinti) */
    ST_SHADOW_US,                                        /* gölge haritası geçişi (µs; kapalıyken 0) */
    ST_COUNT
};

typedef struct Demo {
    b32 ready;
    Arena perm, frame, bins;
    Arena fbmem;         /* framebuffer + MSAA örnekleri; engine_resize'da sıfırlanır */
    Framebuffer fb;
    Raster raster;
    Pipeline pipe;
    Camera cam;
    InputState in;
    i32 scene;
    b32 mip;
    f32 time;

    Mesh* cube;
    Mesh* ground;
    Mesh* big_floor;
    Texture* tex_bricks;
    Texture* tex_grid;
    Texture* tex_checker;
    Texture* tex_white;     /* 4×4 beyaz: düz renkli malzemeler (direk, cetvel) — ince gövdede doku uzamasın */
    LitUniforms u_pole;
    Material m_pole;
    LitUniforms u_bricks, u_grid[3], u_ground, u_floor, u_pillar;
    Material m_bricks, m_grid[3], m_ground, m_floor, m_pillar;
    Mat4* grid_models;
    PhysWorld phys;
    Vehicle car;
    Character chr;
    i32 play;
    f32 chase_yaw;
    Mesh* cylinder;
    LitUniforms u_car, u_tire, u_char, u_rock, u_terrain;
    Material m_car, m_tire, m_char, m_rock, m_terrain;
    TriMesh* terrain;
    Mesh* terrain_mesh;
    ConvexHull* rocks[ROCK_KINDS];
    Mesh* rock_mesh[ROCK_KINDS];
    Mesh* sphere;
    f32 phys_acc;
    f64 phys_ms;
    u32 phys_steps;
    /* sahne 6: yumuşak cisimler */
    Arena soft_arena;    /* sahne kurulurken sıfırlanır */
    SoftWorld soft;
    SoftBody* flag;
    Mesh* flag_mesh;
    SoftBody* jelly[3];
    Mesh* jelly_mesh[3];
    u32 jelly_count;
    LitUniforms u_flag, u_jelly;
    Material m_flag, m_jelly;
    f64 soft_ms;
    f32 soft_time;       /* simülasyon zamanı (rüzgâr) */
    b32 flat;           /* ölçüm: tüm materyaller shade_flat */
    b32 capture;        /* ölçüm: lit materyaller quad yakalayıcıdan geçer (engine_capture_view) */
    i32 vert_mode;      /* 0: C köşe aşaması (vert_std_c), 1: Matoki std_vert.mtk (K tuşu) */
    Mesh* water;        /* sahne 7: 128×128 bölmeli göl, wave_vert.mtk */
    LitUniforms u_sky;  /* sky.mtk: tint = tepe, fog_color = ufuk */
    b32 sky;            /* gökyüzü kubbesi (Y tuşu) */
    Material m_sky;
    LitUniforms u_water;
    Material m_water;
    /* sahne 7, istatistiksel okyanus (O tuşu ↔ Gerstner): FFT alanları CPU'da, mesh köşeleri her kare yazılır */
    b32 ocean_on;
    Ocean ocean;
    Mesh* ocean_mesh;
    f32* ocean_bx;       /* köşelerin düz konumu (yerel x, z) */
    f32* ocean_bz;
    Material m_ocean;
    f64 ocean_ms;
    /* sahne 8: üst üste bırakılan toplar (eşit / farklı kütle) */
    DemWorld dem;            /* toplar; sahne kurulurken n = 0 */
    f32 drop_start[DROP_COLS * DROP_BALLS];   /* bırakılış merkez yüksekliği */
    f32 drop_peak[DROP_COLS * DROP_BALLS];    /* alttaki top zemine değdikten sonraki en yüksek nokta */
    b32 drop_hit[DROP_COLS];
    b32 drop_small_planet;   /* P: R = 20 m'lik gezegen (g yükseklikle belirgin azalır); kapalı: Dünya */
    f32 drop_time;
    i32 shader_mode;     /* SHADER_* */
    Material m_flat;
    Hud hud;             /* motor içi istatistik paneli (U tuşu; varsayılan kapalı: ölçüm/hash araçları etkilenmez) */
    Arena hud_mem;       /* panel atlasları (g.perm yerleşimini değiştirmesin) */
    b32 hud_on;
    f32 fps_ms;          /* kareler arası süre, üstel ortalama */
    u32 stats[ST_COUNT];
    /* sahne 9: ilk girişte ayrı arenada kurulur (g.perm yerleşimi ve diğer sahnelerin kare hash'leri değişmez) */
    b32 anim_ready;
    Arena anim_mem;
    GltfModel arm;
    Mesh* arm_out[ANIM_ARMS];
    EcsWorld ecs;
    EcsEntity anim_root, tent_ent[TENTACLES];
    Skeleton tent_skel;
    SkinnedMesh tent_skin;
    Mesh* tent_out[TENTACLES];
    LitUniforms u_arm, u_tent;
    Material m_arm, m_tent;
    f64 anim_ms;          /* kaplama (iskelet + köşe) süresi */
    /* ses (M13 faz 4): çarpışma sesi köprüsü; bekleme tablosu kendi küçük arenasında */
    ImpactAudio impact;
    Arena impact_mem;
    b32 mute;             /* J tuşu */
    /* canlı shader editörü (M13 faz 6, F2): ilk açılışta ayrı arenalarda kurulur */
    b32 edit_open, edit_ready;
    Arena edit_mem;          /* editör yapısı + sözdizimi renklendirme */
    Arena edit_code[2];      /* dönüşümlü derleme: biri yürürlükteki program, diğerine yeni deneme */
    u32 edit_cur;            /* yürürlükteki derleme arenası (başta 1 → ilk deneme 0'a) */
    CodeEditor* editor;
    UiContext ui;
    UiInput ui_in;
    u32 edit_ok, edit_fail;
    /* gölge haritası (M13 faz 7, B tuşu; sayfa açar, motor varsayılanı kapalı → ölçüm / hash araçları değişmez) */
    b32 shadow_on, shadow_active, shadow_pass;
    ShadowMap* shadow;
    Arena shadow_mem;
    /* GPU arka ucu (M13 faz 8a): JS WebGPU'yu kurunca engine_gpu_set(1); kare komut akışı g.gcmd'de */
    b32 gpu_on;
    GpuCmd gcmd;
} Demo;

static Demo g;

static b32 editor_setup(void);
static void editor_draw(void);

static void lit_defaults(LitUniforms* u, const Texture* tex, Vec3 tint) {
    u->tex = tex;
    u->to_light = v3_norm(v3(0.4f, 0.8f, 0.35f));
    u->ambient = 0.35f;
    u->light_color = v3(0.85f, 0.82f, 0.75f);
    u->fog_start = 40.0f;
    u->fog_end = 400.0f;
    u->fog_color = v3(0.62f, 0.72f, 0.85f);
    u->tint = tint;
    u->use_mip = FALSE;
    u->wrap = 0.25f;
    u->translucency = 0.0f;
    u->shadow = shadow_none();
    mem_zero(&u->shadow_mat, sizeof(u->shadow_mat));   /* 0 → harita koordinatı (0, 0, 0): her yer aydınlık */
    u->shadow_bias = 0.0f;
    lit_uniforms_prepare(u);
}

/* lit shader uygulaması; ilk dördü bit-bit aynı çıktı (lit.mtk = C shader'ları). Varsayılan SHADER_HQ =
 * shaders/lit_hq.mtk (anizotropik + çift doğrusal süzme, sargılı diffuse, kumaş geçirgenliği). L tuşu gezer. */
enum { SHADER_REF, SHADER_LUT, SHADER_SIMD, SHADER_MATOKI, SHADER_HQ, SHADER_MODE_COUNT };

static void shade_capture(const ShadeQuad* q, const void* uniforms, u32 out[4]);   /* ölçüm, aşağıda */
static ShadeFn g_matoki_full;

static ShadeFn g_matoki_lit;      /* hızlı yol (önsöz önbelleğini okur) */
static PreludeFn g_matoki_pre;
static ShadeFn g_hq_lit;
static PreludeFn g_hq_pre;
static ShadeFn g_hq_sh_lit;    /* lit_hq.mtk gölgeli; g_hq_lit gölgesiz türev (gölge kodu derlenmez, M13 faz 7) */
static PreludeFn g_hq_sh_pre;
/* vertex shader'lar (Matoki vert aşaması) */
static VertexFn g_std_vert, g_wave_vert;
static ShadeFn g_water;                 /* water.mtk (göl fragment) */
static ShadeFn g_sky;                   /* sky.mtk (gökyüzü kubbesi) */
static PreludeFn g_sky_pre;
static PreludeFn g_water_pre;
static PreludeFn g_std_vert_pre, g_wave_vert_pre;

/* Matoki'de shader önsözle birlikte gelir; diğerlerinde önsöz yok */
static PreludeFn lit_prelude_for(i32 mode) {
    if (mode == SHADER_HQ && g.shadow_active && g_hq_sh_lit) return g_hq_sh_pre;
    if (mode == SHADER_HQ && g_hq_lit) return g_hq_pre;
    return mode == SHADER_MATOKI && g_matoki_lit ? g_matoki_pre : NULL;
}

static ShadeFn lit_shader_for(i32 mode) {
    if (mode == SHADER_HQ && g.shadow_active && g_hq_sh_lit) return g_hq_sh_lit;
    if (mode == SHADER_HQ && g_hq_lit) return g_hq_lit;
    if (mode == SHADER_MATOKI && g_matoki_lit) return g_matoki_lit;
#if defined(__wasm_simd128__)
    if (mode == SHADER_SIMD || mode == SHADER_MATOKI || mode == SHADER_HQ) return shade_lit_tex_simd;
#endif
    if (mode == SHADER_LUT) return shade_lit_tex_lut;
    return shade_lit_tex;
}

/* .mtk'yi derle + JIT (aşama: frag / vert); uniform yerleşimi denetimi çağıranda. */
/* lit ailesi shader'ın her uniform'u LitUniforms'taki aynı adlı alanla ad + tip + ofset olarak eşleşmeli (açılış ve canlı
 * derleme aynı denetim). Yalnız ofset bakmak yetmiyordu: aynı ofsette yanlış tip (işaretçiyi f32 okumak) ya da yapının
 * sonundan taşan yeni uniform geçiyordu. Uyuşmayan ilk uniform *bad'e (hata konumu için). tex ve tint zorunlu. */
typedef struct LitField {
    const char* name;
    u32 offset, base, n;
} LitField;

#define LIT_FIELD(f, b, k) { #f, (u32)__builtin_offsetof(LitUniforms, f), b, k }
static const LitField LIT_FIELDS[] = {
    LIT_FIELD(tex, TB_TEX, 1), LIT_FIELD(to_light, TB_FLOAT, 3), LIT_FIELD(ambient, TB_FLOAT, 1),
    LIT_FIELD(light_color, TB_FLOAT, 3), LIT_FIELD(fog_start, TB_FLOAT, 1), LIT_FIELD(fog_color, TB_FLOAT, 3),
    LIT_FIELD(fog_end, TB_FLOAT, 1), LIT_FIELD(eye, TB_FLOAT, 3), LIT_FIELD(use_mip, TB_INT, 1), LIT_FIELD(tint, TB_FLOAT, 3),
    LIT_FIELD(wrap, TB_FLOAT, 1), LIT_FIELD(translucency, TB_FLOAT, 1), LIT_FIELD(shadow, TB_TEX, 1),
    LIT_FIELD(shadow_mat, TB_MAT, 4), LIT_FIELD(shadow_bias, TB_FLOAT, 1),
};

static b32 lit_layout_ok_at(const MatokiProgram* p, const GlobalDecl** bad) {
    b32 has_tex = FALSE, has_tint = FALSE;
    for (const GlobalDecl* g0 = p->module.globals; g0; g0 = g0->next) {
        if (g0->kind != GD_UNIFORM) continue;
        const LitField* f = NULL;
        for (u32 i = 0; i < ARRAY_COUNT(LIT_FIELDS) && !f; i++) {
            if (cstr_len(LIT_FIELDS[i].name) == g0->name_len && mem_equal(LIT_FIELDS[i].name, g0->name, g0->name_len)) f = &LIT_FIELDS[i];
        }
        if (!f || f->offset != g0->offset || f->base != (u32)g0->type.base || f->n != g0->type.n) {
            if (bad) *bad = g0;
            return FALSE;
        }
        has_tex |= f->offset == __builtin_offsetof(LitUniforms, tex);
        has_tint |= f->offset == __builtin_offsetof(LitUniforms, tint);
    }
    if (bad) *bad = NULL;
    return has_tex && has_tint;
}

static b32 lit_layout_ok(const MatokiProgram* p) { return lit_layout_ok_at(p, NULL); }

/* src NULL → gömülü dosya; değilse src (aynı ad günlükte) */
static MatokiProgram* matoki_load_src(Arena* a, const char* file, const char* src, u32 len, u32 stage) {
    const EmbedFile* ef = src ? NULL : embed_find(file);
    char buf[320];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    if (!src && !ef) {
        sb_cstr(&sb, "matoki: gömülü değil: ");
        sb_cstr(&sb, file);
        log_sb(&sb);
        return NULL;
    }
    if (!src) {
        src = (const char*)ef->data;
        len = ef->len;
    }
    MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
    MatokiError e;
    MatokiOptions opt;
    mem_zero(&opt, sizeof(opt));
    opt.stage = stage;
    f64 t0 = sys_now_ms();
    b32 ok = matoki_compile(a, src, len, &opt, p, &e) && matoki_jit(a, p, &e);
    f64 t1 = sys_now_ms();
    if (!ok) {
        sb_cstr(&sb, "matoki: ");
        sb_cstr(&sb, file);
        sb_char(&sb, ':');
        matoki_format_error(&e, &sb);
        log_sb(&sb);
        return NULL;
    }
    b32 layout_ok;
    if (stage == MATOKI_STAGE_VERT) {
        const GlobalDecl* model = matoki_find_uniform(p, "model");
        const GlobalDecl* nm = matoki_find_uniform(p, "normal_mat");
        const GlobalDecl* tm = matoki_find_uniform(p, "time");
        const GlobalDecl* pos = matoki_find_attribute(p, "position");
        layout_ok = model && nm && tm && pos && model->offset == __builtin_offsetof(VertUniforms, model) &&
                    nm->offset == __builtin_offsetof(VertUniforms, normal_mat) && tm->offset == __builtin_offsetof(VertUniforms, time) &&
                    pos->offset == ATTR3D_POS;
    } else {
        layout_ok = lit_layout_ok(p);
    }
    if (!layout_ok) {
        sb_cstr(&sb, "matoki: uniform yerleşimi C yapısıyla (LitUniforms / VertUniforms) uyuşmuyor: ");
        sb_cstr(&sb, file);
        log_sb(&sb);
        return NULL;
    }
    sb_cstr(&sb, "matoki: ");
    sb_cstr(&sb, file);
    sb_cstr(&sb, " derlendi + JIT: ");
    sb_u64(&sb, p->ir_live);
    sb_cstr(&sb, " IR komutu (");
    sb_u64(&sb, p->wasm.hoisted);
    sb_cstr(&sb, " önsözde), ");
    sb_u64(&sb, p->wasm.len);
    sb_cstr(&sb, " bayt wasm, ");
    sb_f64(&sb, t1 - t0, 2);
    sb_cstr(&sb, " ms");
    log_sb(&sb);
    return p;
}

static MatokiProgram* matoki_load(Arena* a, const char* file, u32 stage) { return matoki_load_src(a, file, NULL, 0, stage); }

/* lit_hq.mtk'nin gölgesiz türevi: shadow_lit(wpos) → 1.0 (fonksiyon kullanılmaz, DCE atar). Çarpan zaten tam 1 olduğu
 * için çıktı gölgeli programın gölge kapalı haliyle ve faz 6 öncesiyle bit-bit aynı; gölge hesabı hiç çalışmaz. */
static MatokiProgram* matoki_load_hq_noshadow(Arena* a) {
    const EmbedFile* ef = embed_find("lit_hq.mtk");
    static const char KEY[] = "shadow_lit(wpos)";
    if (!ef) return NULL;
    const char* src = (const char*)ef->data;
    u32 n = ef->len, k = (u32)sizeof(KEY) - 1, at = n;
    u32 hits = 0;
    for (u32 i = 0; i + k <= n; i++) {
        if (mem_equal(src + i, KEY, k)) {
            if (!hits) at = i;
            hits++;
        }
    }
    /* tam bir çağrı (yorumda / ikinci kullanımda geçerse türev yanlış yeri değiştirirdi): değilse gölgeli program kullanılır */
    if (hits != 1) {
        LOG("matoki: lit_hq.mtk'de 'shadow_lit(wpos)' %u kez geçiyor (1 olmalı): gölgesiz türev derlenmedi", hits);
        return NULL;
    }
    char* out = ARENA_ARRAY(a, char, n + 1);
    mem_copy(out, src, at);
    mem_copy(out + at, "1.0", 3);
    mem_copy(out + at + 3, src + at + k, n - at - k);
    return matoki_load_src(a, "lit_hq.mtk (gölgesiz)", out, n - k + 3, MATOKI_STAGE_FRAG);
}

static void matoki_lit_init(Arena* a) {
    MatokiProgram* p = matoki_load(a, "lit.mtk", MATOKI_STAGE_FRAG);
    if (p) {
        g_matoki_lit = p->fn_fast;
        g_matoki_pre = p->prelude;
        g_matoki_full = p->fn;
    }
    MatokiProgram* hq = matoki_load(a, "lit_hq.mtk", MATOKI_STAGE_FRAG);
    MatokiProgram* hq0 = hq ? matoki_load_hq_noshadow(a) : NULL;
    if (hq) {
        g_hq_sh_lit = hq->fn_fast;
        g_hq_sh_pre = hq->prelude;
        g_hq_lit = hq0 ? hq0->fn_fast : hq->fn_fast;   /* türev derlenemezse gölgeli (kapalıyken çarpan 1) */
        g_hq_pre = hq0 ? hq0->prelude : hq->prelude;
    }
    MatokiProgram* wf = matoki_load(a, "water.mtk", MATOKI_STAGE_FRAG);
    if (wf) {
        g_water = wf->fn_fast;
        g_water_pre = wf->prelude;
    }
    MatokiProgram* sk = matoki_load(a, "sky.mtk", MATOKI_STAGE_FRAG);
    if (sk) {
        g_sky = sk->fn_fast;
        g_sky_pre = sk->prelude;
    }
    MatokiProgram* sv = matoki_load(a, "std_vert.mtk", MATOKI_STAGE_VERT);
    if (sv) {
        g_std_vert = (VertexFn)sv->fn_fast;
        g_std_vert_pre = sv->prelude;
    }
    MatokiProgram* wv = matoki_load(a, "wave_vert.mtk", MATOKI_STAGE_VERT);
    if (wv && matoki_find_uniform(wv, "amp")->offset == __builtin_offsetof(VertUniforms, params)) {
        g_wave_vert = (VertexFn)wv->fn_fast;
        g_wave_vert_pre = wv->prelude;
    }
}

static void material(Material* m, const LitUniforms* u) {
    m->shade = shade_lit_tex;
    m->uniforms = u;
    m->cull = CULL_BACK;
}

/* Sahne 3: zemin, 6 katlı piramit, iki kule, birkaç küre */
static void phys_scene_build(void) {
    PhysWorld* w = &g.phys;
    phys_world_clear(w);
    BodyDesc ground = phys_desc_plane(v3(0, 1, 0), 0.0f);
    phys_add_body(w, &ground);
    const u32 levels = 6;
    for (u32 l = 0; l < levels; l++) {
        u32 n = levels - l;
        for (u32 i = 0; i < n; i++) {
            f32 x = ((f32)i - (f32)(n - 1) * 0.5f) * 1.02f;
            BodyDesc b = phys_desc_box(v3(x, 0.5f + (f32)l, -2.0f), v3(0.5f, 0.5f, 0.5f), 1.0f);
            phys_add_body(w, &b);
        }
    }
    for (u32 t = 0; t < 2; t++) {
        for (u32 i = 0; i < 6; i++) {
            BodyDesc b = phys_desc_box(v3(t ? 6.0f : -6.0f, 0.4f + (f32)i * 0.8f, -4.0f), v3(0.6f, 0.4f, 0.6f), 1.5f);
            phys_add_body(w, &b);
        }
    }
    for (u32 i = 0; i < 5; i++) {
        BodyDesc s = phys_desc_sphere(v3(-3.0f + (f32)i * 1.5f, 6.0f + (f32)i, 3.0f), 0.45f, 2.0f);
        phys_add_body(w, &s);
    }
    g.phys_acc = 0.0f;
}

static void phys_shoot(void) {
    if (g.phys.count >= g.phys.cap) return;
    Vec3 f = camera_forward(&g.cam);
    BodyDesc s = phys_desc_sphere(v3_add(g.cam.pos, v3_scale(f, 1.0f)), 0.4f, 4.0f);
    s.vel = v3_scale(f, 28.0f);
    s.restitution = 0.3f;
    phys_add_body(&g.phys, &s);
}

/* G: mermi — küçük, çok hızlı küre, 50 g (CCD + BODY_BULLET: dinamik gövdelerden de geçmez; kumaş yakalar) */
static void phys_shoot_bullet(void) {
    if (g.phys.count >= g.phys.cap) return;
    Vec3 f = camera_forward(&g.cam);
    BodyDesc s = phys_desc_sphere(v3_add(g.cam.pos, v3_scale(f, 1.0f)), 0.12f, 0.05f);
    s.vel = v3_scale(f, 160.0f);
    s.restitution = 0.2f;
    u32 id = phys_add_body(&g.phys, &s);
    g.phys.bodies[id].flags |= BODY_BULLET;
}

/* Sahne 4: sürüş alanı — rampa, merdiven, kutu piramidi, devrilecek koniler, sütunlar */
static void drive_scene_build(void) {
    PhysWorld* w = &g.phys;
    phys_world_clear(w);
    BodyDesc ground = phys_desc_plane(v3(0, 1, 0), 0.0f);
    ground.friction = 1.0f;
    phys_add_body(w, &ground);
    /* rampa (−z yönünde yükselir) */
    BodyDesc ramp = phys_desc_box(v3(0, 0.6f, -45), v3(4, 0.5f, 7), 0.0f);
    ramp.rot = quat_from_axis_angle(v3(1, 0, 0), 0.2f);
    phys_add_body(w, &ramp);
    /* merdiven: 0,25 m basamaklar (karakter için) */
    for (u32 k = 0; k < 6; k++) {
        f32 h = 0.25f * (f32)(k + 1);
        BodyDesc st = phys_desc_box(v3(9, h * 0.5f, -6.0f - (f32)k), v3(1.5f, h * 0.5f, 0.5f), 0.0f);
        phys_add_body(w, &st);
    }
    /* sütunlar */
    for (u32 k = 0; k < 4; k++) {
        BodyDesc p = phys_desc_box(v3(-14.0f + (f32)k * 9.0f, 2.0f, -70.0f), v3(1, 2, 1), 0.0f);
        phys_add_body(w, &p);
    }
    /* kutu piramidi */
    for (u32 l = 0; l < 4; l++) {
        u32 n = 4 - l;
        for (u32 i = 0; i < n; i++) {
            f32 x = -10.0f + ((f32)i - (f32)(n - 1) * 0.5f) * 1.02f;
            BodyDesc b = phys_desc_box(v3(x, 0.5f + (f32)l, -25.0f), v3(0.5f, 0.5f, 0.5f), 30.0f);
            phys_add_body(w, &b);
        }
    }
    /* koni sırası */
    for (u32 k = 0; k < 10; k++) {
        BodyDesc c = phys_desc_box(v3(-3.0f + (f32)(k % 2) * 6.0f, 0.3f, -12.0f - (f32)k * 3.0f), v3(0.25f, 0.3f, 0.25f), 5.0f);
        phys_add_body(w, &c);
    }
    VehicleDesc vd = vehicle_desc_default(v3(0, 1.3f, 0));
    vehicle_create(w, &g.car, &vd);
    CharacterDesc cd = character_desc_default(v3(3.5f, 0.0f, 1.0f));
    character_init(&g.chr, &cd);
    g.phys_acc = 0.0f;
}

static void play_set(i32 mode) {
    g.play = mode;
    if (mode == PLAY_FLY) {
        g.cam.mode = CAM_FLY;
        g.cam.use_look = FALSE;
        return;
    }
    g.cam.mode = CAM_EXTERNAL;
    g.cam.use_look = mode == PLAY_DRIVE;
    if (mode == PLAY_WALK) {
        /* karakteri aracın soluna koy, aracın yönüne baksın */
        const Body* ch = &g.phys.bodies[g.car.body];
        Vec3 f = vehicle_forward(&g.phys, &g.car);
        Vec3 left = v3_norm(v3(f.z, 0.0f, -f.x));
        g.chr.pos = v3_add(v3(ch->pos.x, 0.5f, ch->pos.z), v3_scale(left, 2.8f));
        g.chr.vel = v3(0, 0, 0);
        g.cam.pitch = 0.0f;
        g.cam.yaw = m_atan2f(f.x, -f.z);   /* camera_forward = (sin yaw, ·, −cos yaw) */
    }
}

static void car_reset(void) {
    Body* b = &g.phys.bodies[g.car.body];
    b->pos = v3(0, 1.3f, 0);
    b->rot = quat_identity();
    b->vel = b->ang_vel = v3(0, 0, 0);
    phys_body_update_derived(b);
    g.chase_yaw = 0.0f;
}

/* ---- sahne 5: eklemler ve şekiller ---- */

static f32 terrain_height(f32 x, f32 z) {
    return 1.1f * m_sinf(x * 0.21f) * m_cosf(z * 0.17f) + 0.5f * m_sinf(x * 0.11f + z * 0.13f) + 0.8f;
}

/* yükseklik alanından fizik mesh'i + düzgün normalli çizim mesh'i */
static void terrain_build(Arena* a) {
    const u32 n = 40;
    const f32 cell = 1.5f, half = (f32)n * cell * 0.5f;
    u32 nv = (n + 1) * (n + 1);
    Vec3* v = ARENA_ARRAY(a, Vec3, nv);
    u32* idx = ARENA_ARRAY(a, u32, n * n * 6);
    for (u32 j = 0; j <= n; j++) {
        for (u32 i = 0; i <= n; i++) {
            f32 x = (f32)i * cell - half, z = (f32)j * cell - half;
            v[j * (n + 1) + i] = v3(x, terrain_height(x, z), z);
        }
    }
    u32 k = 0;
    for (u32 j = 0; j < n; j++) {
        for (u32 i = 0; i < n; i++) {
            u32 a0 = j * (n + 1) + i, a1 = a0 + 1, b0 = a0 + n + 1, b1 = b0 + 1;
            idx[k++] = a0; idx[k++] = b0; idx[k++] = b1;
            idx[k++] = a0; idx[k++] = b1; idx[k++] = a1;
        }
    }
    g.terrain = trimesh_create(a, v, nv, idx, n * n * 2);
    Mesh* m = mesh_alloc(a, nv, n * n * 6);
    for (u32 q = 0; q < nv; q++) {
        m->px[q] = v[q].x; m->py[q] = v[q].y; m->pz[q] = v[q].z;
        m->u[q] = v[q].x * 0.25f; m->v[q] = v[q].z * 0.25f;
    }
    for (u32 t = 0; t < n * n * 2; t++) {
        u32 i0 = idx[t * 3], i1 = idx[t * 3 + 1], i2 = idx[t * 3 + 2];
        Vec3 fn = v3_cross(v3_sub(v[i1], v[i0]), v3_sub(v[i2], v[i0]));
        const u32 ids[3] = { i0, i1, i2 };
        for (u32 c = 0; c < 3; c++) { m->nx[ids[c]] += fn.x; m->ny[ids[c]] += fn.y; m->nz[ids[c]] += fn.z; }
        m->indices[t * 3] = i0; m->indices[t * 3 + 1] = i1; m->indices[t * 3 + 2] = i2;
    }
    mesh_compute_bounds(m);
    g.terrain_mesh = m;
}

/* düz gölgeli çizim mesh'i (yüz başına köşe kopyası) */
static Mesh* mesh_from_hull(Arena* a, const ConvexHull* h) {
    u32 tris = 0;
    for (u32 f = 0; f < h->fcount; f++) tris += h->faces[f].count - 2;
    Mesh* m = mesh_alloc(a, h->icount, tris * 3);
    u32 vi = 0, k = 0;
    for (u32 f = 0; f < h->fcount; f++) {
        const HullFace* hf = &h->faces[f];
        u32 base = vi;
        for (u32 q = 0; q < hf->count; q++) {
            Vec3 p = h->verts[h->idx[hf->first + q]];
            m->px[vi] = p.x; m->py[vi] = p.y; m->pz[vi] = p.z;
            m->nx[vi] = hf->n.x; m->ny[vi] = hf->n.y; m->nz[vi] = hf->n.z;
            m->u[vi] = (p.x + p.z) * 0.7f; m->v[vi] = p.y * 0.7f;
            vi++;
        }
        for (u32 q = 1; q + 1 < hf->count; q++) {
            m->indices[k++] = base; m->indices[k++] = base + q; m->indices[k++] = base + q + 1;
        }
    }
    mesh_compute_bounds(m);
    return m;
}

static void rocks_build(Arena* a) {
    Rng r = rng_make(2026);
    for (u32 kind = 0; kind < ROCK_KINDS; kind++) {
        Vec3 pts[14];
        Vec3 s = v3(rng_range(&r, 0.4f, 0.8f), rng_range(&r, 0.3f, 0.6f), rng_range(&r, 0.4f, 0.8f));
        for (u32 i = 0; i < 14; i++) {
            Vec3 d = v3_norm(v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), rng_range(&r, -1, 1)));
            pts[i] = v3_mul(d, v3_scale(s, rng_range(&r, 0.8f, 1.0f)));
        }
        g.rocks[kind] = hull_create(a, pts, 14);
        g.rock_mesh[kind] = g.rocks[kind] ? mesh_from_hull(a, g.rocks[kind]) : NULL;
    }
}

static void joints_scene_build(void) {
    PhysWorld* w = &g.phys;
    phys_world_clear(w);
    BodyDesc td = phys_desc_mesh(v3(0, 0, 0), g.terrain);
    td.friction = 0.8f;
    phys_add_body(w, &td);

    /* zincir: dünyaya asılı 8 kapsül halka (yatay başlar, salınır) */
    u32 prev = PHYS_NO_BODY;
    for (u32 i = 0; i < 8; i++) {
        BodyDesc c = phys_desc_capsule(v3(-3.6f + (f32)i * 0.8f, 9.0f, -10.0f), 0.12f, 0.28f, 2.0f);
        c.rot = quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.5f);
        u32 id = phys_add_body(w, &c);
        JointDesc j = phys_joint_desc(JOINT_BALL, prev, id, v3(-4.0f + (f32)i * 0.8f, 9.0f, -10.0f));
        phys_add_joint(w, &j);
        prev = id;
    }
    /* halat köprü: 10 tahta, uçlarda dünyaya, aralarda menteşe (z ekseni) */
    BodyDesc post = phys_desc_box(v3(-5.6f, 2.5f, 8.0f), v3(0.3f, 2.5f, 1.0f), 0.0f);
    phys_add_body(w, &post);
    post.pos.x = 5.6f;
    phys_add_body(w, &post);
    prev = PHYS_NO_BODY;
    for (u32 i = 0; i < 10; i++) {
        BodyDesc p = phys_desc_box(v3(-4.5f + (f32)i, 4.9f, 8.0f), v3(0.48f, 0.06f, 0.8f), 3.0f);
        u32 id = phys_add_body(w, &p);
        JointDesc j = phys_joint_desc(JOINT_HINGE, prev, id, v3(-5.0f + (f32)i, 4.9f, 8.0f));
        j.axis = v3(0, 0, 1);
        phys_add_joint(w, &j);
        prev = id;
    }
    JointDesc end = phys_joint_desc(JOINT_HINGE, prev, PHYS_NO_BODY, v3(5.0f, 4.9f, 8.0f));
    end.axis = v3(0, 0, 1);
    phys_add_joint(w, &end);
    /* yel değirmeni: motorlu menteşe */
    BodyDesc tower = phys_desc_box(v3(10.0f, 3.5f, -6.45f), v3(0.3f, 3.5f, 0.2f), 0.0f);
    phys_add_body(w, &tower);
    BodyDesc blade = phys_desc_box(v3(10.0f, 6.5f, -6.0f), v3(3.0f, 0.25f, 0.12f), 20.0f);
    u32 bl = phys_add_body(w, &blade);
    JointDesc hm = phys_joint_desc(JOINT_HINGE, PHYS_NO_BODY, bl, v3(10.0f, 6.5f, -6.0f));
    hm.axis = v3(0, 0, 1);
    hm.motor = TRUE;
    hm.motor_speed = 1.2f;
    hm.max_motor_torque = 4000.0f;
    phys_add_joint(w, &hm);
    /* tabela: sınırlı menteşe (x ekseni) */
    BodyDesc beam = phys_desc_box(v3(-10.0f, 6.0f, -4.0f), v3(1.4f, 0.12f, 0.12f), 0.0f);
    phys_add_body(w, &beam);
    BodyDesc sign = phys_desc_box(v3(-10.0f, 5.2f, -4.0f), v3(1.0f, 0.6f, 0.05f), 4.0f);
    u32 sg = phys_add_body(w, &sign);
    JointDesc hs = phys_joint_desc(JOINT_HINGE, PHYS_NO_BODY, sg, v3(-10.0f, 5.85f, -4.0f));
    hs.axis = v3(1, 0, 0);
    hs.limit = TRUE;
    hs.lower = -1.0f;
    hs.upper = 1.0f;
    phys_add_joint(w, &hs);
    /* şekil yağmuru: kapsüller, kayalar, kutular */
    Rng r = rng_make(55);
    for (u32 i = 0; i < 30; i++) {
        Vec3 pos = v3(rng_range(&r, -9, 9), 7.0f + (f32)i * 0.6f, rng_range(&r, -3, 5));
        Quat q = quat_from_axis_angle(v3(rng_range(&r, -1, 1), rng_range(&r, -1, 1), rng_range(&r, -1, 1)), rng_range(&r, -3, 3));
        BodyDesc d;
        u32 kind = i % 3;
        if (kind == 0) d = phys_desc_capsule(pos, rng_range(&r, 0.2f, 0.35f), rng_range(&r, 0.2f, 0.5f), 2.0f);
        else if (kind == 1 && g.rocks[i % ROCK_KINDS]) d = phys_desc_hull(pos, g.rocks[i % ROCK_KINDS], 3.0f);
        else d = phys_desc_box(pos, v3(0.35f, 0.35f, 0.35f), 2.0f);
        d.rot = q;
        phys_add_body(w, &d);
    }
    g.phys_acc = 0.0f;
}

/* ---- sahne 6: yumuşak cisimler (bayrak, jöleler) ---- */

/* yumuşak gövde çizim mesh'i: indeksler sabit; iki yüzlüde arka kopya ters sarım + ters normal */
static Mesh* soft_mesh_alloc(Arena* a, const SoftBody* b, b32 two_sided) {
    u32 sides = two_sided ? 2 : 1;
    Mesh* m = mesh_alloc(a, b->count * sides, b->tri_count * 3 * sides);
    for (u32 t = 0; t < b->tri_count; t++) {
        const u32* tr = &b->tris[t * 3];
        m->indices[t * 3] = tr[0]; m->indices[t * 3 + 1] = tr[1]; m->indices[t * 3 + 2] = tr[2];
        if (two_sided) {
            u32* o = &m->indices[(b->tri_count + t) * 3];
            o[0] = tr[0] + b->count; o[1] = tr[2] + b->count; o[2] = tr[1] + b->count;
        }
    }
    return m;
}

/* konum + köşe normali (yüz normallerinin toplamı); offset: yüzeyi normal boyunca it (jöle zarfı) */
static void soft_mesh_update(Mesh* m, const SoftBody* b, b32 two_sided, f32 offset) {
    u32 n = b->count;
    for (u32 i = 0; i < n; i++) m->nx[i] = m->ny[i] = m->nz[i] = 0.0f;
    for (u32 t = 0; t < b->tri_count; t++) {
        const u32* tr = &b->tris[t * 3];
        Vec3 fn = v3_cross(v3_sub(b->x[tr[1]], b->x[tr[0]]), v3_sub(b->x[tr[2]], b->x[tr[0]]));
        for (u32 c = 0; c < 3; c++) { m->nx[tr[c]] += fn.x; m->ny[tr[c]] += fn.y; m->nz[tr[c]] += fn.z; }
    }
    for (u32 i = 0; i < n; i++) {
        Vec3 nn = v3(m->nx[i], m->ny[i], m->nz[i]);
        f32 l = v3_len(nn);
        nn = l > 1e-12f ? v3_scale(nn, 1.0f / l) : v3(0, 1, 0);
        Vec3 p = v3_add(b->x[i], v3_scale(nn, offset));
        m->px[i] = p.x; m->py[i] = p.y; m->pz[i] = p.z;
        m->nx[i] = nn.x; m->ny[i] = nn.y; m->nz[i] = nn.z;
        if (two_sided) {
            u32 k = i + n;
            m->px[k] = p.x; m->py[k] = p.y; m->pz[k] = p.z;
            m->nx[k] = -nn.x; m->ny[k] = -nn.y; m->nz[k] = -nn.z;
            m->u[k] = m->u[i]; m->v[k] = m->v[i];
        }
    }
    mesh_compute_bounds(m);
}

static void soft_scene_build(void) {
    PhysWorld* w = &g.phys;
    phys_world_clear(w);
    arena_reset(&g.soft_arena, 0);
    Arena* a = &g.soft_arena;
    soft_world_init(&g.soft, a, 8);
    g.soft.substeps = 10;
    BodyDesc ground = phys_desc_plane(v3(0, 1, 0), 0.0f);
    ground.friction = 0.8f;
    phys_add_body(w, &ground);
    /* bayrak direği + 3×2 m bayrak (24×16 parçacık, sol sütun direğe iğneli, uzun menzilli bağlar) */
    BodyDesc pole = phys_desc_box(v3(-6.0f, 3.5f, -4.0f), v3(0.08f, 3.5f, 0.08f), 0.0f);
    phys_add_body(w, &pole);
    const u32 nu = 24, nv = 16;
    g.flag = soft_cloth(a, v3(-5.9f, 6.8f, -4.0f), v3(3.0f, 0, 0), v3(0, -2.0f, 0), nu, nv, 0.9f, 0.0f, 1e-4f, 0.05f);
    for (u32 j = 0; j < nv; j++) soft_pin(g.flag, j * nu);
    soft_add_tethers(a, g.flag);
    soft_world_add(&g.soft, g.flag);
    g.flag_mesh = soft_mesh_alloc(a, g.flag, TRUE);
    for (u32 j = 0; j < nv; j++) {
        for (u32 i = 0; i < nu; i++) {
            g.flag_mesh->u[j * nu + i] = (f32)i / (f32)(nu - 1) * 3.0f;
            g.flag_mesh->v[j * nu + i] = (f32)j / (f32)(nv - 1) * 2.0f;
        }
    }
    /* jöleler: zeminde, dinamik kutunun üstünde, statik bloğun kenarına düşen */
    BodyDesc block = phys_desc_box(v3(3.5f, 0.5f, -1.0f), v3(1.0f, 0.5f, 1.0f), 0.0f);
    phys_add_body(w, &block);
    BodyDesc crate = phys_desc_box(v3(0.0f, 0.4f, -2.0f), v3(0.6f, 0.4f, 0.6f), 6.0f);
    phys_add_body(w, &crate);
    const Vec3 jc[3] = { v3(-2.5f, 0.8f, 0.5f), v3(0.0f, 1.8f, -2.0f), v3(2.7f, 3.5f, -0.6f) };
    const f32 js[3] = { 1.4f, 1.0f, 1.0f };
    const u32 jn[3] = { 4, 3, 3 };
    g.jelly_count = 3;
    for (u32 k = 0; k < 3; k++) {
        g.jelly[k] = soft_jelly(a, jc[k], js[k], jn[k], 4.0f * js[k] * js[k] * js[k], 3e-4f, 0.0f);
        soft_world_add(&g.soft, g.jelly[k]);
        g.jelly_mesh[k] = soft_mesh_alloc(a, g.jelly[k], FALSE);
        for (u32 i = 0; i < g.jelly[k]->count; i++) {
            Vec3 p = g.jelly[k]->x[i];
            g.jelly_mesh[k]->u[i] = (p.x + p.z) * 1.5f;
            g.jelly_mesh[k]->v[i] = p.y * 1.5f;
        }
    }
    g.phys_acc = 0.0f;
    g.soft_time = 0.0f;
}

/* Sahne 8: üst üste, 2 mm aralıkla bırakılan toplar (Galileo topu); aynı yoğunluk → yarıçap = 0,25·∛m.
 * Katı cisim çözücüsü yerine Hertz temaslı DEM (phys/dem.c, D34): çarpışma esnek temaslarla aynı anda yayılır,
 * e = 0,9 (sert kauçuk, E = 50 MPa), çekim g(h) = GM/(R+h)² (Dünya; P ile R = 20 m'lik gezegen).
 * Oranlar (sekme / bırakılış, tests/test_dem.c): sol eşit kütle 0,30 / 0,90 / 1,09 — en üstteki en çok;
 * orta en ağır üstte (1, 2, 4 kg) 0,16 / 0,38 / 1,15 — katapult yok, alttaki hafif sıkışır; sağ en ağır altta
 * (4, 2, 1 kg) 0,21 / 0,63 / 2,76 — enerji üste aktarılır. Model kesin olay çözümüyle doğrulandı (D34). */
static const f32 DROP_MASS[DROP_COLS][DROP_BALLS] = { { 2, 2, 2 }, { 1, 2, 4 }, { 4, 2, 1 } };

static f32 drop_col_x(u32 c) { return c == 0 ? -4.5f : c == 1 ? 1.0f : 4.5f; }   /* bölümler arası boşluk */

static void drop_scene_build(void) {
    phys_world_clear(&g.phys);   /* sahne 8 katı cisim dünyasını kullanmaz */
    DemWorld* w = &g.dem;
    w->n = 0;
    w->planet_radius = g.drop_small_planet ? 20.0 : 6.371e6;
    w->planet_gm = g.drop_small_planet ? 9.81 * 20.0 * 20.0 : 3.986004418e14;
    for (u32 c = 0; c < DROP_COLS; c++) {
        f32 y = 1.5f;   /* alttaki topun yüzeyi; toplar 2 mm aralıklı (yerleştirilmiş, yapışık değil) */
        for (u32 i = 0; i < DROP_BALLS; i++) {
            f32 m = DROP_MASS[c][i], r = 0.25f * m_powf(m, 1.0f / 3.0f);
            dem_add(w, drop_col_x(c), y + r, 0.0, r, m);
            u32 k = c * DROP_BALLS + i;
            g.drop_start[k] = g.drop_peak[k] = y + r;
            y += 2.0f * r + 0.002f;
        }
        g.drop_hit[c] = FALSE;
    }
    g.drop_time = 0.0f;
    g.phys_acc = 0.0f;
}

/* kare sonu: alttaki top zeminden sekince sütunun tepe ölçümü başlar */
static void drop_track(void) {
    for (u32 c = 0; c < DROP_COLS; c++) {
        if (!g.drop_hit[c] && g.dem.b[c * DROP_BALLS].v[1] > 0.0) {
            g.drop_hit[c] = TRUE;
            for (u32 i = 0; i < DROP_BALLS; i++) g.drop_peak[c * DROP_BALLS + i] = 0.0f;
        }
        if (!g.drop_hit[c]) continue;
        for (u32 i = 0; i < DROP_BALLS; i++) {
            u32 k = c * DROP_BALLS + i;
            g.drop_peak[k] = m_maxf(g.drop_peak[k], (f32)g.dem.b[k].x[1]);
        }
    }
}
/* zamanla değişen rüzgâr (belirleyici: yalnızca simülasyon zamanına bağlı) */
static Vec3 wind_at(f32 t) {
    return v3(6.0f + 2.5f * m_sinf(t * 0.7f), 0.0f, 1.5f * m_sinf(t * 0.37f) + 1.0f * m_sinf(t * 1.3f + 1.0f));
}

/* Sahne 9 dokunacı: y ekseni boyunca TENT_LEN boyunda, uca doğru incelen tüp; köşe başına en yakın iki eklem
 * (doğrusal geçiş). Eklem j yerelde y = j·L'de; ters bağlama matrisi bağlama pozunun dünya matrisinin tersi. */
static void tentacle_build(Arena* a) {
    const f32 L = TENT_LEN / (f32)TENT_JOINTS;
    u32 ring_v = TENT_SEGS + 1u, vn = TENT_RINGS * ring_v + 1u;
    Mesh* m = mesh_alloc(a, vn, (TENT_RINGS - 1u) * TENT_SEGS * 6u + TENT_SEGS * 3u);
    u16* jts = ARENA_ARRAY(a, u16, vn * 4u);
    f32* wts = ARENA_ARRAY(a, f32, vn * 4u);
    for (u32 i = 0; i < TENT_RINGS; i++) {
        f32 t = (f32)i / (f32)(TENT_RINGS - 1u), y = t * TENT_LEN;
        f32 r = 0.32f * (1.0f - 0.85f * t);
        f32 sj = y / L;
        u32 j0 = (u32)sj < TENT_JOINTS - 1u ? (u32)sj : TENT_JOINTS - 1u;
        f32 f = j0 < TENT_JOINTS - 1u ? sj - (f32)j0 : 0.0f;
        for (u32 k = 0; k <= TENT_SEGS; k++) {
            u32 v = i * ring_v + k;
            f32 th = (f32)k * (M_TAU_F / (f32)TENT_SEGS), c = m_cosf(th), sn = m_sinf(th);
            m->px[v] = r * c; m->py[v] = y; m->pz[v] = r * sn;
            m->nx[v] = c; m->ny[v] = 0.0f; m->nz[v] = sn;
            m->u[v] = (f32)k / (f32)TENT_SEGS; m->v[v] = t * 6.0f;
            jts[v * 4u] = (u16)j0;
            jts[v * 4u + 1u] = (u16)(j0 + 1u < TENT_JOINTS ? j0 + 1u : j0);
            wts[v * 4u] = 1.0f - f;
            wts[v * 4u + 1u] = f;
        }
    }
    u32 tip = vn - 1u;
    m->px[tip] = 0.0f; m->py[tip] = TENT_LEN; m->pz[tip] = 0.0f;
    m->nx[tip] = 0.0f; m->ny[tip] = 1.0f; m->nz[tip] = 0.0f;
    m->u[tip] = 0.5f; m->v[tip] = 6.0f;
    jts[tip * 4u] = TENT_JOINTS - 1u;
    wts[tip * 4u] = 1.0f;
    u32 n = 0;
    for (u32 i = 0; i + 1u < TENT_RINGS; i++) {
        for (u32 k = 0; k < TENT_SEGS; k++) {
            u32 a0 = i * ring_v + k, b0 = a0 + ring_v;
            u32 q[6] = { a0, b0, a0 + 1u, a0 + 1u, b0, b0 + 1u };
            for (u32 e = 0; e < 6u; e++) m->indices[n++] = q[e];
        }
    }
    for (u32 k = 0; k < TENT_SEGS; k++) {
        u32 a0 = (TENT_RINGS - 1u) * ring_v + k;
        m->indices[n++] = a0;
        m->indices[n++] = tip;
        m->indices[n++] = a0 + 1u;
    }
    mesh_compute_bounds(m);

    Skeleton* sk = &g.tent_skel;
    skeleton_init(sk, a, TENT_JOINTS);
    for (u32 j = 0; j < TENT_JOINTS; j++) {
        sk->parents[j] = (i32)j - 1;
        sk->bind_pose[j] = joint_pose_trs(v3(0.0f, j ? L : 0.0f, 0.0f), quat_identity(), v3(1.0f, 1.0f, 1.0f));
    }
    Mat4 world[TENT_JOINTS];
    skeleton_compute_matrices(sk, NULL, world, NULL);
    for (u32 j = 0; j < TENT_JOINTS; j++) sk->inv_bind[j] = mat4_inverse(world[j]);
    g.tent_skin.bind_mesh = m;
    g.tent_skin.vertex_count = vn;
    g.tent_skin.joints = jts;
    g.tent_skin.weights = wts;
    for (u32 i = 0; i < TENTACLES; i++) g.tent_out[i] = skin_clone_mesh(a, m);
}

/* Sahne 9 kurulumu (bir kez): glTF kolu (gömülü assets/arm_skin.glb) + ECS: kök entity'nin çocukları dokunaçlar */
static b32 anim_scene_build(void) {
    if (g.anim_ready) return TRUE;
    /* önceki deneme başarısızsa aynı arena yeniden kullanılır (her denemede yeni 4 MB sızmasın) */
    if (g.anim_mem.base) arena_reset(&g.anim_mem, 0);
    else if (!arena_init_sys(&g.anim_mem, (usize)4 << 20)) return FALSE;
    Arena scratch = arena_sub(&g.anim_mem, (usize)1 << 20);
    const EmbedFile* f = embed_find("arm_skin.glb");
    if (!f || !gltf_load(f->data, f->len, &g.anim_mem, &scratch, NULL, NULL, &g.arm) || g.arm.draw_count == 0 ||
        !g.arm.draws[0].skinned || g.arm.anim_count < ANIM_ARMS) {
        LOG("sahne 9: arm_skin.glb yüklenemedi (%s)", g.arm.err);
        return FALSE;
    }
    for (u32 k = 0; k < ANIM_ARMS; k++) g.arm_out[k] = skin_clone_mesh(&g.anim_mem, g.arm.draws[0].skinned->bind_mesh);
    tentacle_build(&g.anim_mem);

    ecs_world_init(&g.ecs, &g.anim_mem, 64);
    transform_register(&g.ecs);
    EcsCompId tr = transform_comp_id(&g.ecs);
    g.anim_root = ecs_spawn(&g.ecs);
    Transform t = transform_identity();
    ecs_set(&g.ecs, g.anim_root, tr, &t);
    for (u32 i = 0; i < TENTACLES; i++) {
        f32 a = (f32)i * (M_TAU_F / (f32)TENTACLES);
        Vec3 out = v3(m_sinf(a), 0.0f, m_cosf(a));
        /* kökte halka üzerinde, dışa doğru 0,3 rad eğik (eğim ekseni teğet) */
        Quat tilt = quat_from_axis_angle(v3_cross(v3(0.0f, 1.0f, 0.0f), out), 0.3f);
        Transform ti = transform_trs(v3_scale(out, 1.1f), tilt, v3(1.0f, 1.0f, 1.0f));
        g.tent_ent[i] = ecs_spawn(&g.ecs);
        ecs_set(&g.ecs, g.tent_ent[i], tr, &ti);
        transform_set_parent(&g.ecs, g.tent_ent[i], g.anim_root);
    }

    lit_defaults(&g.u_arm, g.tex_grid, v3(0.95f, 0.75f, 0.35f));
    lit_defaults(&g.u_tent, g.tex_grid, v3(0.9f, 0.35f, 0.55f));
    g.u_tent.wrap = 0.6f;
    g.u_tent.translucency = 0.25f;
    material(&g.m_arm, &g.u_arm);
    material(&g.m_tent, &g.u_tent);
    g.m_arm.cull = CULL_NONE;
    g.m_tent.cull = CULL_NONE;
    g.anim_ready = TRUE;
    return TRUE;
}

/* Sahne 9 karesi: ECS kökü döner, dokunaç eklemlerinde ilerleyen dalga, kollar kendi kliplerini döngüyle oynatır.
 * Kaplama CPU'da (anim/skin, wasm SIMD); çıktı mesh'leri kare sonuna dek sabit kalır (pipe_end'de çizilir). */
static void anim_scene_update(void) {
    if (!g.anim_ready) return;
    f64 t0 = sys_now_ms();
    f32 t = g.time;
    EcsCompId tr = transform_comp_id(&g.ecs);
    Transform* root = (Transform*)ecs_get(&g.ecs, g.anim_root, tr);
    root->rot = quat_from_axis_angle(v3(0.0f, 1.0f, 0.0f), t * 0.25f);
    transform_update(&g.ecs);

    const f32 L = TENT_LEN / (f32)TENT_JOINTS;
    JointPose poses[TENT_JOINTS];
    Mat4 skin_m[TENT_JOINTS];
    for (u32 i = 0; i < TENTACLES; i++) {
        f32 ph = (f32)i * 0.9f;
        for (u32 j = 0; j < TENT_JOINTS; j++) {
            f32 w = 2.2f * t - 0.55f * (f32)j + ph;
            Quat bend = quat_mul(quat_from_axis_angle(v3(0.0f, 0.0f, 1.0f), 0.22f * m_sinf(w)),
                                 quat_from_axis_angle(v3(1.0f, 0.0f, 0.0f), 0.16f * m_cosf(0.7f * w)));
            poses[j] = joint_pose_trs(v3(0.0f, j ? L : 0.0f, 0.0f), bend, v3(1.0f, 1.0f, 1.0f));
        }
        skeleton_compute_matrices(&g.tent_skel, poses, NULL, skin_m);
        skin_mesh(&g.tent_skin, skin_m, TENT_JOINTS, g.tent_out[i]);
    }

    const Skeleton* sk = &g.arm.skins[g.arm.draws[0].skin];
    JointPose ap[SKEL_MAX_JOINTS < 16 ? SKEL_MAX_JOINTS : 16];
    Mat4 am[16];
    if (sk->joint_count <= 16) {
        for (u32 k = 0; k < ANIM_ARMS; k++) {
            const AnimClip* c = &g.arm.animations[k];
            /* klip sonunda 0,5 s bekle → bükülme ve dönüş ayrı görülsün */
            f32 period = c->duration + 0.5f;
            f32 lt = t - period * m_floorf(t / period);
            clip_sample(c, sk, m_minf(lt, c->duration), FALSE, ap);
            skeleton_compute_matrices(sk, ap, NULL, am);
            skin_mesh(g.arm.draws[0].skinned, am, sk->joint_count, g.arm_out[k]);
        }
    }
    g.anim_ms = sys_now_ms() - t0;
}

static void draw(const Mesh* mesh, const Material* mat, Mat4 model);

static void render_scene9(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    if (!g.anim_ready) return;
    EcsCompId tr = transform_comp_id(&g.ecs);
    draw(g.cylinder, &g.m_pole, mat4_mul(mat4_translate(v3(0.0f, 0.15f, 0.0f)),
                                         mat4_mul(mat4_rotate_z(M_HALF_PI_F), mat4_scale(v3(0.3f, 2.6f, 2.6f)))));
    for (u32 i = 0; i < TENTACLES; i++) {
        const Transform* ti = (const Transform*)ecs_get(&g.ecs, g.tent_ent[i], tr);
        draw(g.tent_out[i], &g.m_tent, mat4_mul(mat4_translate(v3(0.0f, 0.3f, 0.0f)), ti->world));
    }
    for (u32 k = 0; k < ANIM_ARMS; k++) {
        Mat4 place = mat4_mul(mat4_translate(v3(-4.0f + 4.0f * (f32)k, 0.0f, -4.5f)), mat4_scale(v3(1.4f, 1.4f, 1.4f)));
        draw(g.arm_out[k], &g.m_arm, mat4_mul(place, g.arm.draws[0].world));
    }
}

static void scene_set(i32 s) {
    g.scene = s;
    impact_reset(&g.impact);
    g.phys.planet_gm = 0.0;   /* gezegen çekimi yalnızca sahne 8'de */
    camera_set_mode(&g.cam, CAM_FLY);
    g.cam.use_look = FALSE;
    if (s == 9) {
        g.cam.pos = v3(0.0f, 3.6f, 10.5f);   /* dokunaçlar önde, glTF kolları arkada */
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.16f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 6.0f;
        phys_world_clear(&g.phys);   /* önceki sahnenin gövdeleri adımlanmasın */
        anim_scene_build();
        return;
    }
    if (s == 8) {
        g.cam.pos = v3(0.0f, 3.4f, 9.0f);    /* üç sütuna önden bakar */
        g.cam.yaw = 0.0f;
        g.cam.pitch = 0.0f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 6.0f;
        drop_scene_build();
        return;
    }
    if (s == 7) {
        g.cam.pos = v3(0.0f, 6.0f, 22.0f);    /* göle bakar */
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.22f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 8.0f;
        return;
    }
    if (s == 6) {
        g.cam.pos = v3(-4.2f, 5.6f, 10.0f);   /* bayrağa bakar: G ile vur */
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.04f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 6.0f;
        soft_scene_build();
        return;
    }
    if (s == 5) {
        g.cam.pos = v3(0, 9, 24);
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.28f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 8.0f;
        joints_scene_build();
        return;
    }
    if (s == 4) {
        g.cam.pos = v3(0, 4, 9);
        g.cam.zfar = 1000.0f;
        g.cam.speed = 8.0f;
        drive_scene_build();
        play_set(PLAY_DRIVE);
        return;
    }
    if (s == 3) {
        g.cam.pos = v3(0, 5, 16);
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.2f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 6.0f;
        phys_scene_build();
        return;
    }
    if (s == 1) {
        g.cam.pos = v3(0, 4, 14);
        g.cam.yaw = 0.0f;
        g.cam.pitch = -0.15f;
        g.cam.zfar = 1000.0f;
        g.cam.speed = 6.0f;
    } else {
        g.cam.pos = v3(0, 1.7f, 0);
        g.cam.yaw = 0.3f;
        g.cam.pitch = -0.08f;
        g.cam.zfar = 3000.0f;
        g.cam.speed = 10.0f;
    }
}

/* en büyük çözünürlük: 1920×1080 (renk + derinlik + 4 MSAA örneği = 40 bayt/piksel → ~83 MB) */
#define FB_MAX_PIXELS (1920u * 1080u)

/* framebuffer'ı (yeniden) ayır; çözünürlük sabit 640×360 değil, sayfanın gerçek piksel boyutu (web/3d.js) */
static b32 fb_alloc(i32 w, i32 h) {
    if (w <= 0 || h <= 0 || (u64)w * (u64)h > FB_MAX_PIXELS) return FALSE;
    arena_reset(&g.fbmem, 0);
    usize n = (usize)w * (usize)h;
    g.fb.w = w;
    g.fb.h = h;
    g.fb.color = ARENA_ARRAY(&g.fbmem, u32, n);
    g.fb.depth = ARENA_ARRAY(&g.fbmem, f32, n);
    /* MSAA 4x örnek tamponları (N tuşu açar/kapatır; varsayılan açık) */
    g.fb.color_ms = ARENA_ARRAY(&g.fbmem, u32, n * RASTER_MSAA);
    g.fb.depth_ms = ARENA_ARRAY(&g.fbmem, f32, n * RASTER_MSAA);
    return TRUE;
}

EXPORT(engine_resize) i32 engine_resize(i32 w, i32 h) {
    if (!g.ready || !fb_alloc(w, h)) return 0;
    g.cam.aspect = (f32)w / (f32)h;
    return 1;
}

EXPORT(engine_init) i32 engine_init(i32 w, i32 h) {
    if (w <= 0 || h <= 0) return 0;
    if (!arena_init_sys(&g.perm, (usize)64 << 20)) return 0;
    if (!arena_init_sys(&g.fbmem, (usize)FB_MAX_PIXELS * 40u + ((usize)1 << 20))) return 0;
    if (!arena_init_sys(&g.frame, (usize)32 << 20)) return 0;
    if (!arena_init_sys(&g.bins, (usize)96 << 20)) return 0;   /* 1080p: karo sayısı 640×360'ın 9 katı */
    if (!arena_init_sys(&g.soft_arena, (usize)8 << 20)) return 0;

    if (!fb_alloc(w, h)) return 0;
    g.fb.samples = RASTER_MSAA;
    raster_init(&g.raster, &g.fb);
    camera_init(&g.cam, (f32)w / (f32)h);

    g.tex_bricks = tex_create(&g.perm, 128, 128, TEX_FLAG_MIPMAP);
    procgen_bricks(g.tex_bricks, 7);
    tex_build_mips(g.tex_bricks);
    g.tex_grid = tex_create(&g.perm, 64, 64, TEX_FLAG_MIPMAP);
    procgen_grid(g.tex_grid, 32, color_pack(235, 235, 235, 255), color_pack(60, 60, 70, 255));
    tex_build_mips(g.tex_grid);
    g.tex_checker = tex_create(&g.perm, 256, 256, TEX_FLAG_MIPMAP);
    procgen_checker(g.tex_checker, 32, color_pack(240, 240, 240, 255), color_pack(25, 25, 30, 255));
    tex_build_mips(g.tex_checker);

    g.tex_white = tex_create(&g.perm, 4, 4, 0);
    for (u32 i = 0; i < 16; i++) g.tex_white->px[i] = color_pack(255, 255, 255, 255);

    g.cube = mesh_cube(&g.perm);
    g.ground = mesh_plane(&g.perm, 80.0f, 80.0f, 0.25f, 8);
    g.big_floor = mesh_plane(&g.perm, 4000.0f, 4000.0f, 0.5f, 1);
    g.water = mesh_plane(&g.perm, 40.0f, 40.0f, 0.15f, 128);   /* 129² köşe, 32 768 üçgen */
    g.ocean_mesh = mesh_plane(&g.perm, 40.0f, 40.0f, 0.15f, 128);
    g.ocean_bx = ARENA_ARRAY(&g.perm, f32, g.ocean_mesh->vertex_count);
    g.ocean_bz = ARENA_ARRAY(&g.perm, f32, g.ocean_mesh->vertex_count);
    for (u32 i = 0; i < g.ocean_mesh->vertex_count; i++) { g.ocean_bx[i] = g.ocean_mesh->px[i]; g.ocean_bz[i] = g.ocean_mesh->pz[i]; }
    {
        /* 40 m yama = göl mesh'i (kenarlar periyodik, dikişsiz); rüzgâr kameraya doğru (+z), hafif çapraz */
        OceanDesc od;
        od.n = 64;
        od.size = 40.0f;
        od.wind_x = 0.35f;
        od.wind_z = 1.0f;
        od.wind_speed = 5.5f;
        od.rms_height = 0.16f;
        od.reverse = 0.0f;     /* rüzgâra karşı dalga yok */
        od.choppy = 0.9f;
        od.seed = 2026;
        ocean_init(&g.ocean, &g.perm, &od);
    }
    g.ocean_on = TRUE;
    lit_defaults(&g.u_bricks, g.tex_bricks, v3(1, 1, 1));
    lit_defaults(&g.u_grid[0], g.tex_grid, v3(1.0f, 0.55f, 0.45f));
    lit_defaults(&g.u_grid[1], g.tex_grid, v3(0.45f, 0.85f, 0.55f));
    lit_defaults(&g.u_grid[2], g.tex_grid, v3(0.5f, 0.6f, 1.0f));
    lit_defaults(&g.u_ground, g.tex_checker, v3(0.75f, 0.8f, 0.7f));
    g.u_ground.fog_start = 8.0f;      /* 80 m'lik zeminin kenarı ufukta sise erisin (sert çizgi olmasın) */
    g.u_ground.fog_end = 48.0f;
    lit_defaults(&g.u_floor, g.tex_checker, v3(1, 1, 1));
    g.u_floor.fog_start = 10.0f;      /* üstel sis: uzak zemin ufukta gök rengine erir (pus) */
    g.u_floor.fog_end = 450.0f;
    lit_defaults(&g.u_pillar, g.tex_bricks, v3(1, 1, 1));
    g.u_pillar.fog_start = 10.0f;
    g.u_pillar.fog_end = 450.0f;
    material(&g.m_bricks, &g.u_bricks);
    for (u32 i = 0; i < 3; i++) material(&g.m_grid[i], &g.u_grid[i]);
    material(&g.m_ground, &g.u_ground);
    material(&g.m_floor, &g.u_floor);
    material(&g.m_pillar, &g.u_pillar);
    lit_defaults(&g.u_pole, g.tex_white, v3(0.55f, 0.56f, 0.6f));   /* mat metal gri */
    material(&g.m_pole, &g.u_pole);
    g.m_flat.shade = shade_flat;
    g.m_flat.uniforms = NULL;
    g.m_flat.cull = CULL_BACK;

    g.grid_models = ARENA_ARRAY(&g.perm, Mat4, GRID_COUNT);
    const f32 spacing = 2.5f;
    for (u32 k = 0; k < GRID_N; k++) {
        for (u32 j = 0; j < GRID_N; j++) {
            for (u32 i = 0; i < GRID_N; i++) {
                Vec3 pos = v3(((f32)i - 4.5f) * spacing, 1.0f + (f32)j * spacing, -20.0f - (f32)k * spacing);
                g.grid_models[(k * GRID_N + j) * GRID_N + i] = mat4_mul(mat4_translate(pos), mat4_scale(v3(1.2f, 1.2f, 1.2f)));
            }
        }
    }

    g.sphere = mesh_sphere(&g.perm, 12, 20);
    g.cylinder = mesh_cylinder(&g.perm, 18);
    terrain_build(&g.perm);
    rocks_build(&g.perm);
    lit_defaults(&g.u_rock, g.tex_checker, v3(0.55f, 0.5f, 0.45f));
    lit_defaults(&g.u_terrain, g.tex_grid, v3(0.45f, 0.75f, 0.35f));
    material(&g.m_rock, &g.u_rock);
    material(&g.m_terrain, &g.u_terrain);
    lit_defaults(&g.u_car, g.tex_grid, v3(0.95f, 0.18f, 0.12f));
    lit_defaults(&g.u_tire, g.tex_checker, v3(0.16f, 0.16f, 0.18f));
    lit_defaults(&g.u_char, g.tex_grid, v3(0.3f, 0.55f, 1.0f));
    material(&g.m_car, &g.u_car);
    material(&g.m_tire, &g.u_tire);
    material(&g.m_char, &g.u_char);
    lit_defaults(&g.u_flag, g.tex_checker, v3(0.95f, 0.22f, 0.18f));
    g.u_flag.wrap = 1.0f;          /* half-Lambert²: kıvrımlar belli, gölge yüzü kara değil */
    g.u_flag.translucency = 0.3f;  /* ters yüz arkadan ışıkla parlar */
    lit_defaults(&g.u_jelly, g.tex_grid, v3(0.35f, 0.95f, 0.45f));
    g.u_jelly.wrap = 0.7f;
    lit_defaults(&g.u_water, g.tex_grid, v3(0.04f, 0.16f, 0.22f));   /* derin su rengi (water.mtk) */
    g.u_water.wrap = 0.5f;
    material(&g.m_water, &g.u_water);
    material(&g.m_ocean, &g.u_water);
    g.m_ocean.cull = CULL_NONE;
    g.m_water.cull = CULL_NONE;          /* dalga tepesinden arka yüz görünebilir */
    g.m_water.vert_params[0] = 0.22f;    /* amp (m) */
    g.m_water.vert_params[1] = 7.0f;     /* en uzun dalga boyu (m) */
    g.m_water.vert_params[2] = 1.0f;     /* hız çarpanı */
    g.m_water.vert_params[3] = 0.6f;     /* sivrilik */
    g.m_water.cull_margin = 1.5f;
    lit_defaults(&g.u_sky, g.tex_grid, v3(0.32f, 0.52f, 0.86f));   /* tepe rengi (water.mtk gök yansımasıyla aynı) */
    material(&g.m_sky, &g.u_sky);
    g.m_sky.cull = CULL_NONE;
    g.u_jelly.translucency = 0.2f;
    material(&g.m_flag, &g.u_flag);
    material(&g.m_jelly, &g.u_jelly);
    phys_world_init(&g.phys, &g.perm, 1024, 8192);
    dem_init(&g.dem, &g.perm, DROP_COLS * DROP_BALLS, 5.0e7, 0.45, 0.9);
    g.phys.allow_sleep = TRUE;

    g.mip = TRUE;
    /* varsayılan Matoki (C ile bit-bit aynı, ölçümde ortalama daha hızlı — tasks/M10_temel.md);
     * derlenemezse C SIMD'ye düşer */
    g.shader_mode = SHADER_SIMD;
    matoki_lit_init(&g.perm);
    if (g_matoki_lit) g.shader_mode = SHADER_MATOKI;
    if (g_hq_lit) g.shader_mode = SHADER_HQ;
    g.vert_mode = g_std_vert != NULL;
    g.sky = TRUE;
    /* ayrı arena: atlas (~200 KB) g.perm'e girince sonraki sahne ayırmalarının yerleşimi kayıyor, ölçümde
     * panel kapalıyken %2,5–4,5 yavaşlık görüldü (tools/bench_ab.mjs); başarısızsa panel çizilmez */
    if (arena_init_sys(&g.hud_mem, (usize)1 << 20)) hud_init(&g.hud, &g.hud_mem, &g.frame);
    {
        static u8 impact_buf[1024 * sizeof(f32) + 64];
        arena_init(&g.impact_mem, impact_buf, sizeof(impact_buf));
        impact_init(&g.impact, &g.impact_mem, 1024);   /* = phys_world_init gövde kapasitesi */
    }
    scene_set(1);
    g.ready = TRUE;
    return 1;
}

static const char* const SHADER_LABEL[SHADER_MODE_COUNT] = { "ref (skaler)", "tablo", "SIMD", "Matoki JIT", "Matoki HQ" };

/* g.stats dolduktan sonra; çözümlenmiş renge yazar */
static void draw_hud(void) {
    static char buf[7][128];
    const u32* s = g.stats;
    f32 fps = g.fps_ms > 0.0f ? 1000.0f / g.fps_ms : 0.0f;
    fmt_format(buf[0], 128, "MATOKI   %.0f fps   kare %.2f ms   %dx%d   %u thread", (f64)fps,
               (f64)s[ST_TOTAL_US] * 0.001, g.fb.w, g.fb.h, jobs_threads());
    fmt_format(buf[1], 128, "sahne %u   %s   MSAA %ux   F2: shader editörü", s[ST_SCENE],
               SHADER_LABEL[g.shader_mode], s[ST_MSAA]);
    fmt_format(buf[2], 128, "üçgen %u / %u   piksel %u   nesne %u (elenen %u)", s[ST_TRI_DRAWN], s[ST_TRI_IN],
               s[ST_PIXELS], s[ST_OBJ_IN], s[ST_OBJ_CULLED]);
    fmt_format(buf[3], 128, "geometri %.2f ms   raster %.2f ms   çözümleme %.2f ms   gölge %s %.2f ms (B)",
               (f64)s[ST_GEO_US] * 0.001, (f64)s[ST_FLUSH_US] * 0.001, (f64)s[ST_RESOLVE_US] * 0.001,
               g.shadow_active ? "açık" : "kapalı", (f64)s[ST_SHADOW_US] * 0.001);
    if (g.scene == 9) {
        fmt_format(buf[4], 128, "kaplama %u dokunaç + %u glTF kol   %.2f ms   (ECS + anim + glTF: MatokiEngine)", TENTACLES,
                   ANIM_ARMS, (f64)s[ST_ANIM_US] * 0.001);
    } else {
        fmt_format(buf[4], 128, "fizik %u gövde   %u temas   %u uyanık   %.2f ms", s[ST_PHYS_BODIES], s[ST_PHYS_CONTACTS],
                   s[ST_PHYS_AWAKE], (f64)s[ST_PHYS_US] * 0.001);
    }
    fmt_format(buf[5], 128, "bellek: kare %u KB   karo %u KB   başarısız ayırma %u", s[ST_FRAME_KB], s[ST_ARENA_KB],
               s[ST_MEM_FAILS]);
    if (!audio_is_running()) {
        fmt_format(buf[6], 128, "U: paneli kapat   ses: bir tuşa basınca başlar");
    } else {
        fmt_format(buf[6], 128, "U: paneli kapat   J: ses %s   çalan %u   vuruş %u   sentez %.2f ms   kesinti %u", g.mute ? "KAPALI" : "açık",
                   s[ST_AUDIO_VOICES], s[ST_IMPACTS], (f64)s[ST_AUDIO_US] * 0.001, s[ST_AUDIO_UNDERRUNS]);
    }
    const char* lines[7] = { buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6] };
    hud_draw_lines(&g.hud, &g.fb, 8, 8, lines, 7);
}

EXPORT(engine_set_hud) void engine_set_hud(i32 on) { g.hud_on = on != 0; }

static void draw(const Mesh* mesh, const Material* mat, Mat4 model) {
    DrawItem d;
    d.mesh = mesh;
    d.mat = g.shadow_pass ? shadow_caster() : g.flat ? &g.m_flat : mat;
    d.model = model;
    pipe_draw(&g.pipe, &d);
}

static void set_frame_uniforms(void) {
    LitUniforms* all[] = { &g.u_bricks, &g.u_grid[0], &g.u_grid[1], &g.u_grid[2], &g.u_ground, &g.u_floor, &g.u_pillar,
                           &g.u_car, &g.u_tire, &g.u_char, &g.u_rock, &g.u_terrain, &g.u_flag, &g.u_jelly, &g.u_water, &g.u_sky, &g.u_pole,
                           &g.u_arm, &g.u_tent };
    for (u32 i = 0; i < ARRAY_COUNT(all); i++) {
        all[i]->eye = g.cam.pos;
        all[i]->use_mip = g.mip;
        if (g.shadow_active) {
            all[i]->shadow = &g.shadow->tex;
            all[i]->shadow_mat = g.shadow->world_to_tex;
            all[i]->shadow_bias = shadow_bias_for(g.shadow, SHADOW_BIAS_M);
        } else {
            all[i]->shadow = shadow_none();
            mem_zero(&all[i]->shadow_mat, sizeof(all[i]->shadow_mat));
            all[i]->shadow_bias = 0.0f;
        }
    }
    ShadeFn fn = g.capture ? shade_capture : lit_shader_for(g.shader_mode);
    PreludeFn pre = g.capture ? NULL : lit_prelude_for(g.shader_mode);
    Material* mats[] = { &g.m_bricks, &g.m_grid[0], &g.m_grid[1], &g.m_grid[2], &g.m_ground, &g.m_floor, &g.m_pillar,
                         &g.m_car, &g.m_tire, &g.m_char, &g.m_rock, &g.m_terrain, &g.m_flag, &g.m_jelly, &g.m_water, &g.m_ocean, &g.m_pole,
                         &g.m_arm, &g.m_tent };
    /* köşe aşaması: Matoki std_vert.mtk ya da C kâhini (K); göl her zaman wave_vert.mtk (C eşi yok) */
    VertexFn vf = g.vert_mode ? g_std_vert : NULL;
    PreludeFn vpre = g.vert_mode ? g_std_vert_pre : NULL;
    for (u32 i = 0; i < ARRAY_COUNT(mats); i++) {
        mats[i]->shade = fn;
        mats[i]->prelude = pre;
        mats[i]->vert = vf;
        mats[i]->vert_prelude = vpre;
    }
    if (g_wave_vert) {
        g.m_water.vert = g_wave_vert;
        g.m_water.vert_prelude = g_wave_vert_pre;
    }
    if (g_water && !g.capture) {
        g.m_water.shade = g_water;
        g.m_water.prelude = g_water_pre;
        g.m_ocean.shade = g_water;   /* köşeler CPU'da yazılı: köşe aşaması std (Matoki / C) */
        g.m_ocean.prelude = g_water_pre;
    }
    g.m_sky.shade = g_sky;
    g.m_sky.prelude = g_sky_pre;
    g.m_sky.vert = vf;
    g.m_sky.vert_prelude = vpre;
}

/* gökyüzü kubbesi: kameraya ortalanmış, uzak düzlemin hemen içinde büyük küre (iç yüz, eleme yok) */
static void draw_sky(void) {
    if (!g_sky || !g.sky || g.flat || g.capture) return;
    f32 r = g.cam.zfar * 0.85f;
    draw(g.sphere, &g.m_sky, mat4_mul(mat4_translate(g.cam.pos), mat4_scale(v3(r, r, r))));
}

static void render_scene1(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    Mat4 spin = mat4_mul(mat4_rotate_y(g.time * 0.9f), mat4_rotate_x(g.time * 0.6f));
    draw(g.cube, &g.m_bricks, mat4_mul(mat4_translate(v3(0, 2.2f, 2.0f)), mat4_mul(spin, mat4_scale(v3(2.5f, 2.5f, 2.5f)))));
    for (u32 i = 0; i < GRID_COUNT; i++) draw(g.cube, &g.m_grid[i % 3], g.grid_models[i]);
}

/* her şekil için çizim (kapsül = silindir + iki küre; hull = önceden üretilmiş düz mesh) */
static void draw_body(const Body* b, u32 i) {
    const Material* box_mat = b->inv_mass > 0.0f ? &g.m_bricks : &g.m_pillar;
    f32 hmin = m_minf(b->half.x, m_minf(b->half.y, b->half.z)), hmax = m_maxf(b->half.x, m_maxf(b->half.y, b->half.z));
    if (b->shape == SHAPE_BOX && hmax > 8.0f * hmin) box_mat = &g.m_pole;   /* direk: tuğla dokusu uzar */
    switch (b->shape) {
    case SHAPE_BOX:
        draw(g.cube, box_mat, mat4_from_pos_rot_scale(b->pos, b->rot, v3_scale(b->half, 2.0f)));
        break;
    case SHAPE_SPHERE:
        draw(g.sphere, &g.m_grid[i % 3], mat4_from_pos_rot_scale(b->pos, b->rot, v3(b->radius, b->radius, b->radius)));
        break;
    case SHAPE_CAPSULE: {
        Quat q = quat_mul(b->rot, quat_from_axis_angle(v3(0, 0, 1), M_PI_F * 0.5f));   /* silindir x → kapsül y */
        draw(g.cylinder, &g.m_grid[2], mat4_from_pos_rot_scale(b->pos, q, v3(2.0f * b->half_height, b->radius, b->radius)));
        Vec3 ax = v3_scale(mat3_col(&b->rot_m, 1), b->half_height);
        Vec3 r = v3(b->radius, b->radius, b->radius);
        draw(g.sphere, &g.m_grid[2], mat4_from_pos_rot_scale(v3_add(b->pos, ax), b->rot, r));
        draw(g.sphere, &g.m_grid[2], mat4_from_pos_rot_scale(v3_sub(b->pos, ax), b->rot, r));
        break;
    }
    case SHAPE_HULL:
        for (u32 k = 0; k < ROCK_KINDS; k++) {
            if (b->hull == g.rocks[k] && g.rock_mesh[k]) draw(g.rock_mesh[k], &g.m_rock, mat4_from_pos_rot_scale(b->pos, b->rot, v3(1, 1, 1)));
        }
        break;
    case SHAPE_MESH:
        if (b->mesh == g.terrain) draw(g.terrain_mesh, &g.m_terrain, mat4_from_pos_rot_scale(b->pos, b->rot, v3(1, 1, 1)));
        break;
    default:
        break;
    }
}

static void render_scene5(void) {
    for (u32 i = 0; i < g.phys.count; i++) draw_body(&g.phys.bodies[i], i);
}

static void render_scene6(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    for (u32 i = 0; i < g.phys.count; i++) draw_body(&g.phys.bodies[i], i);
    draw(g.flag_mesh, &g.m_flag, mat4_identity());
    for (u32 k = 0; k < g.jelly_count; k++) draw(g.jelly_mesh[k], &g.m_jelly, mat4_identity());
}

/* yumuşak cisim mesh'leri (bayrak + jöleler): karede bir kez, gölge ve ana geçişten önce */
static void soft_meshes_update(void) {
    soft_mesh_update(g.flag_mesh, g.flag, TRUE, 0.0f);
    for (u32 k = 0; k < g.jelly_count; k++) soft_mesh_update(g.jelly_mesh[k], g.jelly[k], FALSE, g.jelly[k]->radius);
}

static void render_scene3(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    for (u32 i = 0; i < g.phys.count; i++) {
        const Body* b = &g.phys.bodies[i];
        if (b->shape == SHAPE_BOX) {
            draw(g.cube, &g.m_bricks, mat4_from_pos_rot_scale(b->pos, b->rot, v3_scale(b->half, 2.0f)));
        } else if (b->shape == SHAPE_SPHERE) {
            draw(g.sphere, &g.m_grid[i % 3], mat4_from_pos_rot_scale(b->pos, b->rot, v3(b->radius, b->radius, b->radius)));
        }
    }
}

static void render_scene4(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    for (u32 i = 0; i < g.phys.count; i++) {
        const Body* b = &g.phys.bodies[i];
        if (i == g.car.body || b->shape == SHAPE_PLANE) continue;
        const Material* m = b->inv_mass > 0.0f ? &g.m_bricks : &g.m_pillar;
        if (b->shape == SHAPE_BOX) draw(g.cube, m, mat4_from_pos_rot_scale(b->pos, b->rot, v3_scale(b->half, 2.0f)));
        else draw(g.sphere, &g.m_grid[i % 3], mat4_from_pos_rot_scale(b->pos, b->rot, v3(b->radius, b->radius, b->radius)));
    }
    const Body* ch = &g.phys.bodies[g.car.body];
    draw(g.cube, &g.m_car, mat4_from_pos_rot_scale(ch->pos, ch->rot, v3_scale(ch->half, 2.0f)));
    /* kabin */
    Vec3 cab = v3_add(ch->pos, mat3_mul_v3(&ch->rot_m, v3(0, ch->half.y + 0.28f, 0.25f)));
    draw(g.cube, &g.m_car, mat4_from_pos_rot_scale(cab, ch->rot, v3(1.5f, 0.56f, 2.0f)));
    for (u32 i = 0; i < g.car.wheel_count; i++) draw(g.cylinder, &g.m_tire, vehicle_wheel_matrix(&g.phys, &g.car, i));
    if (g.play != PLAY_WALK) {
        Vec3 c = v3_add(g.chr.pos, v3(0, g.chr.height * 0.5f, 0));
        draw(g.cube, &g.m_char, mat4_from_pos_rot_scale(c, quat_identity(), v3(g.chr.radius * 2.0f, g.chr.height, g.chr.radius * 2.0f)));
    }
}

/* Sahne 7: dalgalı göl (wave_vert.mtk, her kare köşe aşamasında yeniden şekillenir) + içinde sütunlar */
/* okyanus mesh'i: t anındaki FFT alanlarından köşe konumu (yükseklik + yatay λ·D) ve eğim normali */
static void ocean_mesh_update(void) {
    f64 t0 = sys_now_ms();
    ocean_update(&g.ocean, (f64)g.time);
    Mesh* m = g.ocean_mesh;
    for (u32 i = 0; i < m->vertex_count; i++) {
        Vec3 off, n;
        ocean_sample(&g.ocean, g.ocean_bx[i], g.ocean_bz[i], &off, &n);
        m->px[i] = g.ocean_bx[i] + off.x;
        m->py[i] = off.y;
        m->pz[i] = g.ocean_bz[i] + off.z;
        m->nx[i] = n.x; m->ny[i] = n.y; m->nz[i] = n.z;
    }
    mesh_compute_bounds(m);
    g.ocean_ms = sys_now_ms() - t0;
}

static void render_scene7(void) {
    draw(g.big_floor, &g.m_floor, mat4_identity());
    if (g.ocean_on) {
        ocean_mesh_update();
        draw(g.ocean_mesh, &g.m_ocean, mat4_translate(v3(0.0f, 0.9f, -8.0f)));
    } else {
        draw(g.water, &g.m_water, mat4_translate(v3(0.0f, 0.9f, -8.0f)));
    }
    for (i32 k = 0; k < 7; k++) {
        f32 a = (f32)k * (M_TAU_F / 7.0f) + 0.4f;
        Vec3 pos = v3(m_sinf(a) * 11.0f, 2.0f, -8.0f - m_cosf(a) * 11.0f);
        draw(g.cube, &g.m_pillar, mat4_mul(mat4_translate(pos), mat4_scale(v3(1.2f, 4.0f + (f32)(k % 3), 1.2f))));
    }
    /* su üstünde sallanan sandıklar */
    for (i32 k = 0; k < 4; k++) {
        f32 t = g.time * 0.8f + (f32)k * 1.6f;
        Vec3 pos = v3(-6.0f + (f32)k * 4.0f, 1.2f + 0.25f * m_sinf(t * 1.3f), -6.0f + 2.0f * m_cosf((f32)k));
        Mat4 rot = mat4_mul(mat4_rotate_z(0.15f * m_sinf(t)), mat4_rotate_x(0.12f * m_cosf(t * 0.9f)));
        if (g.ocean_on) {   /* okyanusta: yüzey yüksekliğinde yüzer, yüzey normaline yatar */
            Vec3 off, n;
            ocean_sample(&g.ocean, pos.x, pos.z + 8.0f, &off, &n);
            pos.y = 0.9f + off.y + 0.25f;
            rot = mat4_mul(mat4_rotate_z(-n.x), mat4_rotate_x(n.z));
        }
        draw(g.cube, &g.m_bricks, mat4_mul(mat4_translate(pos), mat4_mul(rot, mat4_scale(v3(1.4f, 1.4f, 1.4f)))));
    }
}

/* Sahne 8: toplar kütleye göre renkli (1 kg mavi, 2 kg yeşil, 4 kg kırmızı). Her topun yanında iki çentik:
 * gri = bırakıldığı yükseklik, top rengi = sekmeden sonra ulaştığı en yüksek nokta (aradaki fark = kayıp). */
static void render_scene8(void) {
    draw(g.ground, &g.m_ground, mat4_identity());
    for (u32 c = 0; c < DROP_COLS; c++) {
        f32 x = drop_col_x(c);
        draw(g.cube, &g.m_pole, mat4_mul(mat4_translate(v3(x, 4.5f, -0.9f)), mat4_scale(v3(0.12f, 9.0f, 0.12f))));   /* cetvel */
        for (u32 i = 0; i < DROP_BALLS; i++) {
            u32 k = c * DROP_BALLS + i;
            const DemBall* b = &g.dem.b[k];
            Vec3 pos = v3((f32)b->x[0], (f32)b->x[1], (f32)b->x[2]);
            f32 rad = (f32)b->r;
            f32 m = DROP_MASS[c][i];
            const Material* mat = m >= 4.0f ? &g.m_grid[0] : m >= 2.0f ? &g.m_grid[1] : &g.m_grid[2];
            draw(g.sphere, mat, mat4_from_pos_rot_scale(pos, quat_identity(), v3(rad, rad, rad)));
            f32 side = (f32)i * 0.42f;   /* çentikler cetvelin sağında, toplara göre kademeli */
            draw(g.cube, &g.m_pole, mat4_mul(mat4_translate(v3(x + 0.6f + side, g.drop_start[k], -0.9f)), mat4_scale(v3(0.36f, 0.05f, 0.1f))));
            if (g.drop_hit[c]) draw(g.cube, mat, mat4_mul(mat4_translate(v3(x + 0.6f + side, g.drop_peak[k], -0.9f)), mat4_scale(v3(0.36f, 0.1f, 0.16f))));
        }
    }
}

static void render_scene2(void) {
    draw(g.big_floor, &g.m_floor, mat4_identity());
    for (i32 k = 0; k < 12; k++) {
        f32 a = (f32)k * (M_TAU_F / 12.0f);
        Vec3 pos = v3(m_sinf(a) * 25.0f, 3.0f, -m_cosf(a) * 25.0f);
        draw(g.cube, &g.m_pillar, mat4_mul(mat4_translate(pos), mat4_scale(v3(1.5f, 6.0f, 1.5f))));
    }
}

static void render_scene(void) {
    if (g.scene == 1) render_scene1();
    else if (g.scene == 2) render_scene2();
    else if (g.scene == 7) render_scene7();
    else if (g.scene == 3) render_scene3();
    else if (g.scene == 4) render_scene4();
    else if (g.scene == 6) render_scene6();
    else if (g.scene == 8) render_scene8();
    else if (g.scene == 9) render_scene9();
    else render_scene5();
}

/* Gölge geçişi (M13 faz 7): kameranın önündeki küreye oturan ışık izdüşümünde sahneyi yalnız derinlik çizer
 * (draw → shadow_caster). Okyanus (sahne 7: köşeleri dalga shader'ı oynatır) ve ölçüm kipleri gölgesiz. */
static void shadow_pass(void) {
    g.shadow_active = FALSE;
    g.stats[ST_SHADOW_US] = 0;
    if (!g.shadow_on || g.scene == 7 || g.flat || g.capture) return;
    if (!g.shadow) {
        if (!arena_init_sys(&g.shadow_mem, (usize)SHADOW_SIZE * SHADOW_SIZE * 4u + (64u << 10))) {
            g.shadow_on = FALSE;
            return;
        }
        g.shadow = ARENA_NEW(&g.shadow_mem, ShadowMap);
        shadow_init(g.shadow, &g.shadow_mem, SHADOW_SIZE);
    }
    f64 t0 = sys_now_ms();
    Mat4 view = camera_view(&g.cam);
    Vec3 fwd = v3(-view.m[2], -view.m[6], -view.m[10]);
    Vec3 center = v3_add(g.cam.pos, v3_scale(fwd, SHADOW_RADIUS * 0.6f));
    shadow_fit(g.shadow, g.u_ground.to_light, center, SHADOW_RADIUS, 40.0f);
    arena_reset(&g.frame, 0);
    arena_reset(&g.bins, 0);
    shadow_begin(g.shadow, &g.pipe, &g.frame, &g.bins);
    g.shadow_pass = TRUE;
    render_scene();
    g.shadow_pass = FALSE;
    shadow_end(g.shadow, &g.pipe);
    g.shadow_active = TRUE;
    g.stats[ST_SHADOW_US] = (u32)((sys_now_ms() - t0) * 1000.0);
}

EXPORT(engine_set_shadow) void engine_set_shadow(i32 on) { g.shadow_on = on != 0; }

/* ---- GPU arka ucu (M13 faz 8a, D38): JS (web/gpu.mjs) WebGPU kurulursa açar; yoksa yazılım render kalır ---- */
EXPORT(engine_gpu_set) void engine_gpu_set(i32 on) { g.gpu_on = on != 0; }
/* son karenin komut akışı (gpu/gcmd.h başlığıyla); GPU kipi kapalıysa ya da akış kurulamadıysa 0 */
EXPORT(engine_gpu_cmds) u32* engine_gpu_cmds(void) { return g.gpu_on && g.gcmd.cap ? g.gcmd.words : NULL; }
/* gömülü WGSL (assets/gpu_8a.wgsl) */
EXPORT(engine_gpu_shader) const u8* engine_gpu_shader(void) {
    const EmbedFile* f = embed_find("gpu_8a.wgsl");
    return f ? f->data : NULL;
}
EXPORT(engine_gpu_shader_len) u32 engine_gpu_shader_len(void) {
    const EmbedFile* f = embed_find("gpu_8a.wgsl");
    return f ? f->len : 0;
}

EXPORT(engine_frame) void engine_frame(f32 dt_ms) {
    if (!g.ready) return;
    f64 t_start = sys_now_ms();
    f32 dt = m_clampf(dt_ms * 0.001f, 0.0f, 0.1f);
    g.time += dt;
    g.fps_ms = g.fps_ms > 0.0f ? g.fps_ms + (dt_ms - g.fps_ms) * 0.1f : dt_ms;
    /* shader editörü (F2): açıkken girdi oyuna değil editöre gider; basılı tuş durumu kare ortasında geri yüklenir */
    if (g.in.pressed[KEY_F2] && (g.edit_open || editor_setup())) g.edit_open = !g.edit_open;
    b32 ui_frame = g.edit_open;
    u8 held[256];
    if (ui_frame) {
        /* basılı ya da bu karede basılmış: iki kare arasında bas-bırak (yavaş karede hızlı dokunuş) kaybolmasın */
        for (u32 k = 0; k < 256; k++) g.ui_in.keys[k] = g.in.down[k] | g.in.pressed[k];
        g.ui_in.mouse_x = g.in.mouse_x;
        g.ui_in.mouse_y = g.in.mouse_y;
        g.ui_in.mouse_buttons = g.in.mouse_buttons | g.in.mouse_latch;
        g.ui_in.wheel = g.in.wheel > 0.0f ? 1 : g.in.wheel < 0.0f ? -1 : 0;
        g.ui_in.char_count = MIN(g.in.char_count, UI_MAX_CHARS);
        mem_copy(g.ui_in.chars, g.in.chars, sizeof(u32) * g.ui_in.char_count);
        mem_copy(held, g.in.down, sizeof(held));
        mem_zero(g.in.down, sizeof(g.in.down));
        mem_zero(g.in.pressed, sizeof(g.in.pressed));
        g.in.mouse_dx = g.in.mouse_dy = g.in.wheel = 0.0f;
    }
    /* oyun tuşları editör bloğundan sonra: editörde yazılan 'u' / 'b' paneli / gölgeyi değiştirmesin */
    if (g.in.pressed[KEY_U]) g.hud_on = !g.hud_on;
    if (g.in.pressed[KEY_B]) g.shadow_on = !g.shadow_on;
    if (g.in.pressed[KEY_J]) {   /* ses aç/kapa: master 0 ↔ synth varsayılanı */
        static f32 master_on = -1.0f;
        Synth* sy = audio_synth();
        if (master_on < 0.0f) master_on = sy->master;
        g.mute = !g.mute;
        if (g.mute) synth_stop_all(sy);
        sy->master = g.mute ? 0.0f : master_on;
    }

    if (g.in.pressed[KEY_0 + 1]) scene_set(1);
    if (g.in.pressed[KEY_0 + 2]) scene_set(2);
    if (g.in.pressed[KEY_0 + 3]) scene_set(3);
    if (g.in.pressed[KEY_0 + 4]) scene_set(4);
    if (g.in.pressed[KEY_0 + 5]) scene_set(5);
    if (g.in.pressed[KEY_0 + 6]) scene_set(6);
    if (g.in.pressed[KEY_0 + 7]) scene_set(7);
    if (g.in.pressed[KEY_0 + 8]) scene_set(8);
    if (g.in.pressed[KEY_0 + 9]) scene_set(9);
    if (g.scene == 8 && g.in.pressed[KEY_T]) drop_scene_build();
    if (g.scene == 8 && g.in.pressed[KEY_P]) { g.drop_small_planet = !g.drop_small_planet; drop_scene_build(); }
    if (g.in.pressed[KEY_K] && g_std_vert) g.vert_mode = !g.vert_mode;
    if (g.in.pressed[KEY_Y]) g.sky = !g.sky;
    if (g.in.pressed[KEY_O]) g.ocean_on = !g.ocean_on;
    if (g.scene == 5 && g.in.pressed[KEY_T]) joints_scene_build();
    if (g.scene == 6 && g.in.pressed[KEY_T]) soft_scene_build();
    const u8* key = g.in.down;
    b32 driving = g.scene == 4 && g.play == PLAY_DRIVE;
    b32 walking = g.scene == 4 && g.play == PLAY_WALK;
    Vec3 wish = v3(0, 0, 0);
    b32 jump = FALSE;
    if (g.scene == 4) {
        if (g.in.pressed[KEY_V]) {
            play_set((g.play + 1) % PLAY_COUNT);
            driving = g.play == PLAY_DRIVE;
            walking = g.play == PLAY_WALK;
        }
        if (g.in.pressed[KEY_T]) car_reset();
        /* araç girişleri (sürmüyorsa park freni) */
        Vehicle* car = &g.car;
        if (driving) {
            f32 st = (key[KEY_A] ? 1.0f : 0.0f) - (key[KEY_D] ? 1.0f : 0.0f);
            car->steer += (st - car->steer) * m_minf(1.0f, dt * 6.0f);
            car->throttle = 0.0f;
            car->brake = 0.0f;
            if (key[KEY_W]) car->throttle = 1.0f;
            if (key[KEY_S]) {
                if (car->speed > 1.0f) car->brake = 1.0f;
                else car->throttle = -0.6f;
            }
            car->handbrake = key[KEY_SPACE] != 0;
            g.chase_yaw += g.in.mouse_dx * 0.004f;
            g.chase_yaw *= 1.0f - m_minf(1.0f, dt * 0.8f);   /* yavaşça arkaya dön */
        } else {
            car->throttle = 0.0f;
            car->steer = 0.0f;
            car->brake = 0.3f;
            car->handbrake = FALSE;
        }
        if (walking) {
            Vec3 f = camera_forward(&g.cam), r = camera_right(&g.cam);
            Vec3 flat = v3_norm(v3(f.x, 0.0f, f.z));
            f32 fw = (key[KEY_W] ? 1.0f : 0.0f) - (key[KEY_S] ? 1.0f : 0.0f);
            f32 rt = (key[KEY_D] ? 1.0f : 0.0f) - (key[KEY_A] ? 1.0f : 0.0f);
            wish = v3_add(v3_scale(flat, fw), v3_scale(v3(r.x, 0.0f, r.z), rt));
            f32 l = v3_len(wish);
            if (l > 1.0f) wish = v3_scale(wish, 1.0f / l);
            jump = g.in.pressed[KEY_SPACE] != 0;
        }
    }
    if (g.scene >= 3) {
        if (g.in.pressed[KEY_F]) phys_shoot();
        if (g.in.pressed[KEY_G]) phys_shoot_bullet();
        if (g.scene == 3 && g.in.pressed[KEY_T]) phys_scene_build();
        /* sabit adım; en fazla 4 adım/kare (yavaş karede "ölüm sarmalı"nı önler) */
        g.phys_acc += dt;
        g.phys_steps = 0;
        g.phys_ms = 0.0;
        g.soft_ms = 0.0;
        while (g.phys_acc >= PHYS_DT && g.phys_steps < 4) {
            if (g.scene == 4) {
                vehicle_update(&g.phys, &g.car, PHYS_DT);
                if (walking) character_update(&g.phys, &g.chr, wish, jump && g.phys_steps == 0, PHYS_DT);
            }
            b32 snd = audio_is_running() && !g.mute;
            if (snd) impact_advance(&g.impact, PHYS_DT);
            if (g.scene == 8) {
                static f32 vb[DROP_COLS * DROP_BALLS * 3];
                for (u32 i = 0; snd && i < g.dem.n && i < DROP_COLS * DROP_BALLS; i++) {
                    for (u32 j = 0; j < 3; j++) vb[i * 3 + j] = (f32)g.dem.b[i].v[j];
                }
                f64 t0 = sys_now_ms();
                dem_step(&g.dem, PHYS_DT);
                g.phys_ms += sys_now_ms() - t0;
                if (snd && g.dem.n <= DROP_COLS * DROP_BALLS) impact_from_dem(&g.impact, audio_synth(), &g.dem, vb, PHYS_DT, 4);
                drop_track();
                g.drop_time += PHYS_DT;
            } else {
                phys_step(&g.phys, PHYS_DT);
                g.phys_ms += g.phys.stats.step_ms;
                if (snd) impact_from_phys(&g.impact, audio_synth(), &g.phys, 4);
            }
            if (g.scene == 6) {
                g.soft_time += PHYS_DT;
                g.soft.wind = wind_at(g.soft_time);
                soft_world_step(&g.soft, &g.phys, PHYS_DT);
                g.soft_ms += g.soft.step_ms;
            }
            g.phys_acc -= PHYS_DT;
            g.phys_steps++;
        }
        if (g.phys_acc > PHYS_DT) g.phys_acc = 0.0f;
        if (g.scene == 8 && g.drop_time > DROP_RESET) drop_scene_build();   /* kendiliğinden yeniden bırak */
    }
    if (g.in.pressed[KEY_M]) g.mip = !g.mip;
    if (g.in.pressed[KEY_N]) g.fb.samples = g.fb.samples == RASTER_MSAA ? 1u : RASTER_MSAA;
    if (g.in.pressed[KEY_L]) g.shader_mode = (g.shader_mode + 1) % SHADER_MODE_COUNT;
    if (g.in.pressed[KEY_R]) raster_set_mode(&g.raster, (RasterMode)((g.raster.mode + 1) % RASTER_MODE_COUNT));
    if (g.in.pressed[KEY_C] && g.cam.mode != CAM_EXTERNAL) camera_set_mode(&g.cam, g.cam.mode == CAM_FLY ? CAM_ORBIT : CAM_FLY);
    if (!driving) camera_update(&g.cam, &g.in, dt);
    if (driving) {
        /* takip kamerası: aracın arkası (fareyle döndürülebilir), yumuşak izleme */
        const Body* ch = &g.phys.bodies[g.car.body];
        Vec3 f = vehicle_forward(&g.phys, &g.car);
        f = v3_norm(v3(f.x, 0.0f, f.z));
        f32 c = m_cosf(g.chase_yaw), s = m_sinf(g.chase_yaw);
        Vec3 dir = v3(f.x * c - f.z * s, 0.0f, f.x * s + f.z * c);
        Vec3 want = v3_add(v3_sub(ch->pos, v3_scale(dir, 7.5f)), v3(0, 2.8f, 0));
        g.cam.pos = v3_lerp(g.cam.pos, want, m_minf(1.0f, dt * 5.0f));
        g.cam.look = v3_add(ch->pos, v3(0, 1.0f, 0));
    } else if (walking) {
        g.cam.pos = character_eye(&g.chr);
    }
    if (ui_frame) mem_copy(g.in.down, held, sizeof(held));
    g.in.mouse_dx = g.in.mouse_dy = g.in.wheel = 0.0f;
    g.in.char_count = 0;
    g.in.mouse_latch = 0;
    mem_zero(g.in.pressed, sizeof(g.in.pressed));

    /* CPU'da üretilen geometri karede bir kez, iki geçişten (gölge + ana) önce: gölge aynı pozdan */
    if (g.scene == 9) anim_scene_update();
    if (g.scene == 6) soft_meshes_update();
    if (!g.gpu_on) shadow_pass();
    else g.shadow_active = FALSE;   /* GPU gölgesi faz 8c */
    f64 t0 = sys_now_ms();
    if (!g.gpu_on) fb_clear(&g.fb, color_pack(158, 184, 217, 255), 1.0f);
    f64 t_clear = sys_now_ms();
    arena_reset(&g.frame, 0);
    arena_reset(&g.bins, 0);
    raster_reset_stats(&g.raster);
    raster_set_depth(&g.raster, TRUE, TRUE, DEPTH_LESS);
    raster_begin(&g.raster, &g.bins);
    set_frame_uniforms();
    /* GPU kipi: komut akışı pipe_begin'den önce (pipe_end kare arenasını pipe_begin noktasına geri sarar) */
    b32 gpu = g.gpu_on && gcmd_begin(&g.gcmd, &g.frame, PIPE_MAX_DRAWS, (u32)g.fb.w, (u32)g.fb.h,
                                      color_pack(158, 184, 217, 255), 1.0f, g.flat ? GPU_MODE_FLAT : GPU_MODE_NORMAL);
    pipe_begin(&g.pipe, &g.raster, &g.frame, camera_view(&g.cam), camera_proj(&g.cam), g.cam.pos);
    pipe_set_time(&g.pipe, g.time);
    if (gpu) pipe_set_gpu(&g.pipe, &g.gcmd);
    draw_sky();
    render_scene();
    f64 t_geo = sys_now_ms();
    pipe_end(&g.pipe);
    if (gpu) gcmd_end(&g.gcmd);
    f64 t1 = sys_now_ms();
    if (!gpu) fb_resolve(&g.fb);   /* MSAA: örnekler → renk */
    f64 t_res = sys_now_ms();

    const PipeStats* ps = &g.pipe.stats;
    const RasterStats* rs = &g.raster.stats;
    g.stats[ST_SCENE] = (u32)g.scene;
    g.stats[ST_MIP] = (u32)g.mip;
    g.stats[ST_CAM_MODE] = (u32)g.cam.mode;
    g.stats[ST_OBJ_IN] = ps->objects_in;
    g.stats[ST_OBJ_CULLED] = ps->objects_culled;
    g.stats[ST_TRI_IN] = ps->tris_in;
    g.stats[ST_TRI_REJECTED] = ps->tris_rejected;
    g.stats[ST_TRI_NEAR] = ps->tris_near_clipped;
    g.stats[ST_TRI_GUARD] = ps->tris_guard_clipped;
    g.stats[ST_TRI_EMITTED] = ps->tris_emitted;
    g.stats[ST_TRI_BACKFACE] = rs->tris_culled;
    g.stats[ST_TRI_DEGEN] = rs->tris_degenerate + rs->tris_empty;
    g.stats[ST_TRI_DRAWN] = rs->tris_drawn;
    g.stats[ST_QUADS] = rs->quads_shaded;
    g.stats[ST_PIXELS] = rs->pixels_written;
    g.stats[ST_FRAME_US] = (u32)((t1 - t0) * 1000.0);
    g.stats[ST_CLEAR_US] = (u32)((t_clear - t0) * 1000.0);
    /* geometri = çizim kaydı + köşe aşaması + üçgen kurulumu; boyama = raster (pipe_end içindeki flush'lar) */
    g.stats[ST_GEO_US] = (u32)(((t_geo - t_clear) + g.pipe.vert_ms + g.pipe.setup_ms) * 1000.0);
    g.stats[ST_FLUSH_US] = (u32)(g.pipe.raster_ms * 1000.0);
    g.stats[ST_VERT_US] = (u32)(g.pipe.vert_ms * 1000.0);
    g.stats[ST_SETUP_US] = (u32)(g.pipe.setup_ms * 1000.0);
    g.stats[ST_DRAWS_DROPPED] = ps->draws_dropped;
    g.stats[ST_TRIS_DROPPED] = rs->tris_dropped;
    g.stats[ST_FRAME_KB] = (u32)(g.frame.peak >> 10);
    g.stats[ST_MEM_FAILS] = g.frame.fails + g.bins.fails;
    g.stats[ST_TOTAL_US] = (u32)((t_res - t_start) * 1000.0);
    g.stats[ST_PRELUDES] = rs->preludes;
    g.stats[ST_MSAA] = fb_msaa(&g.fb) ? RASTER_MSAA : 1u;
    g.stats[ST_RESOLVE_US] = (u32)((t_res - t1) * 1000.0);
    g.stats[ST_VERT_MODE] = (u32)g.vert_mode;
    g.stats[ST_VERTS] = ps->verts_transformed;
    g.stats[ST_BATCHES] = g.pipe.batches;
    g.stats[ST_ARENA_KB] = (u32)(g.bins.peak >> 10);
    g.stats[ST_RASTER_MODE] = (u32)g.raster.mode;
    g.stats[ST_BIN_ITEMS] = rs->bin_items;
    g.stats[ST_BIN_FULL] = rs->bin_full;
    g.stats[ST_SHADER_MODE] = (u32)g.shader_mode;
    g.stats[ST_PHYS_BODIES] = g.phys.count;
    g.stats[ST_PHYS_CONTACTS] = g.phys.stats.contacts;
    g.stats[ST_PHYS_US] = (u32)(g.phys_ms * 1000.0);
    g.stats[ST_ANIM_US] = g.scene == 9 ? (u32)(g.anim_ms * 1000.0) : 0u;
    g.stats[ST_PHYS_STEPS] = g.phys_steps;
    g.stats[ST_PHYS_AWAKE] = g.phys.stats.awake;
    g.stats[ST_PLAY_MODE] = (u32)g.play;
    g.stats[ST_CAR_KMH] = (u32)(i32)(g.car.speed * 3.6f * 100.0f);
    g.stats[ST_CAR_WHEELS] = g.car.wheels_on_ground;
    g.stats[ST_CHAR_GROUNDED] = (u32)g.chr.grounded;
    u32 particles = 0;
    for (u32 k = 0; g.scene == 6 && k < g.soft.count; k++) particles += g.soft.bodies[k]->count;
    g.stats[ST_SOFT_PARTICLES] = particles;
    g.stats[ST_SOFT_US] = (u32)(g.soft_ms * 1000.0);
    g.stats[ST_SOFT_CONTACTS] = g.soft.rigid_contacts + g.soft.soft_contacts;
    g.stats[ST_CAM_X] = (u32)(i32)(g.cam.pos.x * 100.0f);
    g.stats[ST_CAM_Y] = (u32)(i32)(g.cam.pos.y * 100.0f);
    g.stats[ST_CAM_Z] = (u32)(i32)(g.cam.pos.z * 100.0f);
    g.stats[ST_AUDIO_VOICES] = audio_is_running() ? synth_active_voices(audio_synth()) : 0u;
    g.stats[ST_IMPACTS] = g.impact.played;
    g.stats[ST_AUDIO_UNDERRUNS] = audio_is_running() ? __atomic_load_n(&audio_ring()->underruns, __ATOMIC_RELAXED) : 0u;
    g.stats[ST_HUD_US] = 0;
    if (g.hud_on && !g.gpu_on) {   /* GPU kipinde panel / editör bindirmesi faz 8c (yazılım tamponu gösterilmiyor) */
        f64 th = sys_now_ms();
        draw_hud();
        g.stats[ST_HUD_US] = (u32)((sys_now_ms() - th) * 1000.0);
    }
    if (g.edit_open && g.edit_ready && !g.gpu_on) editor_draw();
    /* ses: halka tamponu 60 ms gecikmeye kadar doldur (AudioWorklet okur; ses başlamadıysa iş yok) */
    g.stats[ST_AUDIO_US] = 0;
    if (audio_is_running()) {
        f64 ta = sys_now_ms();
        audio_pump(AUDIO_LATENCY_S);
        g.stats[ST_AUDIO_US] = (u32)((sys_now_ms() - ta) * 1000.0);
    }
}

EXPORT(engine_framebuffer) u32* engine_framebuffer(void) { return g.fb.color; }
/* ölçüm / GPU denetimi: tek örnekli derinlik tamponu (MSAA kapalıyken son karenin derinliği) */
EXPORT(engine_depthbuffer) f32* engine_depthbuffer(void) { return g.fb.depth; }
EXPORT(engine_stats) u32* engine_stats(void) { return g.stats; }
EXPORT(engine_stats_count) u32 engine_stats_count(void) { return ST_COUNT; }

EXPORT(engine_key) void engine_key(i32 code, i32 down) {
    if (code < 0 || code > 255) return;
    if (down && !g.in.down[code]) g.in.pressed[code] = 1;
    g.in.down[code] = down ? 1 : 0;
}

EXPORT(engine_mouse) void engine_mouse(f32 dx, f32 dy) {
    g.in.mouse_dx += dx;
    g.in.mouse_dy += dy;
}

EXPORT(engine_wheel) void engine_wheel(f32 dy) { g.in.wheel += dy; }

EXPORT(engine_release_keys) void engine_release_keys(void) { mem_zero(g.in.down, sizeof(g.in.down)); }

/* ---- ölçüm (tools/bench.mjs) ---- */

typedef struct BenchView {
    i32 scene;
    f32 x, y, z, yaw, pitch;
} BenchView;

static const BenchView BENCH_VIEWS[] = {
    { 1, 0.0f, 4.0f, 14.0f, 0.0f, -0.15f },     /* 1: başlangıç: küp, zemin, ızgara */
    { 1, 0.0f, 8.3f, -29.1f, 0.0f, 0.2f },      /* 2: ızgaranın içinde, yakın kırpma */
    { 1, 34.0f, 30.0f, 22.0f, -0.57f, -0.30f }, /* 3: uzaktan: çok sayıda küçük üçgen */
    { 2, 0.0f, 1.7f, 0.0f, 0.3f, -0.08f },      /* 4: ufka uzanan mipmap zemini */
    { 6, -3.2f, 5.2f, 1.2f, -0.25f, 0.05f },    /* 5: bayrak yakından (kumaş ışığı) */
    { 7, 0.0f, 6.0f, 22.0f, 0.0f, -0.22f },     /* 6: dalgalı göl (wave_vert.mtk, 33 bin üçgen) */
    { 8, 0.0f, 3.4f, 9.0f, 0.0f, 0.0f },       /* 7: üst üste toplar (--frames ile fizik ilerler) */
    { 3, 0.0f, 5.0f, 16.0f, 0.0f, -0.2f },     /* 8: fizik yığınları (--frames ile devrilir) */
    { 4, 0.0f, 4.0f, 9.0f, 0.0f, -0.1f },      /* 9: araba sahnesi (takip kamerası) */
    { 5, 0.0f, 9.0f, 24.0f, 0.0f, -0.28f },    /* 10: eklemler + şekiller */
};

EXPORT(engine_bench_view_count) i32 engine_bench_view_count(void) { return (i32)ARRAY_COUNT(BENCH_VIEWS); }

/* Görünümü sabitler; ardından engine_frame(0) her seferinde aynı kareyi üretir. */
EXPORT(engine_bench_view) void engine_bench_view(i32 view) {
    if (view < 1 || view > (i32)ARRAY_COUNT(BENCH_VIEWS)) return;
    const BenchView* b = &BENCH_VIEWS[view - 1];
    scene_set(b->scene);
    g.cam.pos = v3(b->x, b->y, b->z);
    g.cam.yaw = b->yaw;
    g.cam.pitch = b->pitch;
    g.time = 1.0f;
    g.mip = TRUE;
    mem_zero(&g.in, sizeof(g.in));
}

EXPORT(engine_set_flat) void engine_set_flat(i32 on) { g.flat = on != 0; }
EXPORT(engine_set_mip) void engine_set_mip(i32 on) { g.mip = on != 0; }
EXPORT(engine_matoki_ready) i32 engine_matoki_ready(void) { return g_matoki_lit != NULL; }

EXPORT(engine_shader_mode_count) i32 engine_shader_mode_count(void) { return SHADER_MODE_COUNT; }
EXPORT(engine_set_shader_mode) void engine_set_shader_mode(i32 mode) {
    if (mode >= 0 && mode < SHADER_MODE_COUNT) g.shader_mode = mode;
}

/* ---- shader mikro-ölçümü (tools/bench.mjs --shade) ----
 * Raster'dan bağımsız: sabit, gerçekçi quad'lar (tutarlı uv → mip 0..5, rastgele normal/konum)
 * tuğla dokusu + mip açık. Dönüş: quad başına ns; engine_shade_bench_hash çıktıların FNV'si. */
#define SHADE_BENCH_QUADS 1024
static ShadeQuad g_bench_quads[SHADE_BENCH_QUADS];
static u32 g_bench_hash;

EXPORT(engine_shade_bench) f64 engine_shade_bench(i32 mode, i32 iters) {
    if (!g.ready || mode < 0 || mode >= SHADER_MODE_COUNT) return -1.0;
    Rng rng = rng_make(0x5AADE);
    for (u32 i = 0; i < SHADE_BENCH_QUADS; i++) {
        ShadeQuad* q = &g_bench_quads[i];
        mem_zero(q, sizeof(*q));
        q->x = (i32)(rng_u32(&rng) % 640) & ~1;
        q->y = (i32)(rng_u32(&rng) % 360) & ~1;
        q->mask = (i % 5 == 0) ? 0x7 : 0xF;
        f32 s = m_bits_f32((u32)(127 - 8 + (i32)(rng_u32(&rng) % 6)) << 23);
        f32 u0 = rng_range(&rng, -3, 3), v0 = rng_range(&rng, -3, 3);
        Vec3 p = v3(rng_range(&rng, -60, 60), rng_range(&rng, 0, 20), rng_range(&rng, -60, 60));
        Vec3 n = v3(rng_range(&rng, -1, 1), rng_range(&rng, -1, 1), rng_range(&rng, -1, 1));
        for (u32 l = 0; l < 4; l++) {
            f32 lx = (f32)(l & 1), ly = (f32)(l >> 1);
            q->z[l] = 0.5f;
            q->var[VAR3D_WPOS][l] = p.x + lx * 0.05f;
            q->var[VAR3D_WPOS + 1][l] = p.y + ly * 0.05f;
            q->var[VAR3D_WPOS + 2][l] = p.z;
            q->var[VAR3D_NRM][l] = n.x;
            q->var[VAR3D_NRM + 1][l] = n.y + lx * 0.01f;
            q->var[VAR3D_NRM + 2][l] = n.z;
            q->var[VAR3D_UV][l] = u0 + lx * s;
            q->var[VAR3D_UV + 1][l] = v0 + ly * s;
        }
    }
    LitUniforms u = g.u_bricks;
    u.use_mip = TRUE;
    u.eye = v3(0, 5, 0);
    ShadeFn fn = lit_shader_for(mode);
    PreludeFn pre = lit_prelude_for(mode);
    if (pre) pre(&u);   /* tek çizim: önsöz bir kez (süreye dahil değil; raster'da çizim başına) */
    u32 out[4];
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < SHADE_BENCH_QUADS; i++) {
        fn(&g_bench_quads[i], &u, out);
        for (u32 l = 0; l < 4; l++) {
            if (g_bench_quads[i].mask & (1u << l)) h = (h ^ out[l]) * 0x01000193u;
        }
    }
    g_bench_hash = h;
    f64 t0 = sys_now_ms();
    for (i32 it = 0; it < iters; it++) {
        for (u32 i = 0; i < SHADE_BENCH_QUADS; i++) fn(&g_bench_quads[i], &u, out);
    }
    f64 t1 = sys_now_ms();
    return (t1 - t0) * 1e6 / ((f64)iters * SHADE_BENCH_QUADS);
}

EXPORT(engine_shade_bench_hash) u32 engine_shade_bench_hash(void) { return g_bench_hash; }

/* Gerçek kareden quad yakalama: engine_capture_view(v) bir bench görünümünü çizer ve her 5. quad'ı
 * (uniform bloğuyla) kaydeder; engine_replay_bench(mode, iters) aynı quad'ları, uniform değiştikçe
 * önsözü çağırarak (raster gibi) gölgeler. Dönüş: quad başına ns. */
#define CAPTURE_MAX 8192
static ShadeQuad* g_cap_q;        /* ilk yakalamada perm arenasından (yalnızca ölçüm) */
static const void** g_cap_u;
static u32 g_cap_n, g_cap_seen;

static void shade_capture(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    if (g_cap_seen++ % 5 == 0 && g_cap_n < CAPTURE_MAX) {
        g_cap_q[g_cap_n] = *q;
        g_cap_u[g_cap_n] = uniforms;
        g_cap_n++;
    }
    shade_lit_tex_lut(q, uniforms, out);
}

EXPORT(engine_capture_view) u32 engine_capture_view(i32 view) {
    if (!g_cap_q) {
        g_cap_q = ARENA_ARRAY(&g.perm, ShadeQuad, CAPTURE_MAX);
        g_cap_u = ARENA_ARRAY(&g.perm, const void*, CAPTURE_MAX);
    }
    engine_bench_view(view);
    i32 saved = g.shader_mode;
    g_cap_n = g_cap_seen = 0;
    g.capture = TRUE;
    engine_frame(0);
    g.capture = FALSE;
    g.shader_mode = saved;
    return g_cap_n;
}

static u32 g_replay_preludes;

/* mode == SHADER_MODE_COUNT: Matoki tam gövde (önsöz yok) */
EXPORT(engine_replay_bench) f64 engine_replay_bench(i32 mode, i32 iters) {
    if (!g_cap_n || mode < 0 || mode > SHADER_MODE_COUNT) return -1.0;
    if (mode == SHADER_MODE_COUNT && !g_matoki_full) return -1.0;
    ShadeFn fn = mode == SHADER_MODE_COUNT ? g_matoki_full : lit_shader_for(mode);
    PreludeFn pre = mode == SHADER_MODE_COUNT ? NULL : lit_prelude_for(mode);
    g_replay_preludes = 0;
    u32 out[4];
    u32 h = 0x811c9dc5u;
    f64 t0 = sys_now_ms();
    for (i32 it = 0; it < iters; it++) {
        const void* last = NULL;
        for (u32 i = 0; i < g_cap_n; i++) {
            if (pre && g_cap_u[i] != last) {
                pre(g_cap_u[i]);
                last = g_cap_u[i];
                if (it == 0) g_replay_preludes++;
            }
            fn(&g_cap_q[i], g_cap_u[i], out);
            if (it == 0) {
                for (u32 l = 0; l < 4; l++) {
                    if (g_cap_q[i].mask & (1u << l)) h = (h ^ out[l]) * 0x01000193u;
                }
            }
        }
    }
    f64 t1 = sys_now_ms();
    g_bench_hash = h;
    return (t1 - t0) * 1e6 / ((f64)iters * g_cap_n);
}

EXPORT(engine_replay_preludes) u32 engine_replay_preludes(void) { return g_replay_preludes; }

/* ölçüm/teşhis: lit shader'ı verilen kaynakla yeniden derle (uniform yerleşimi aynı olmalı).
 * engine_scratch(n) kaynağın yazılacağı belleği verir (sığmazsa 0). Başarı → 1. Başarısız derleme kalıcı
 * arenayı geri sarar (sık düzenlemede bellek sızmaz); başarılı programın belleği JIT kodu kullandığı için kalır. */
static char* g_src_buf;
static u32 g_src_cap;
EXPORT(engine_scratch) char* engine_scratch(u32 n) {
    g_src_buf = n < 0xFFFFFFFFu ? (char*)arena_try(&g.perm, (usize)n + 1, 1) : NULL;
    g_src_cap = g_src_buf ? n : 0;
    return g_src_buf;
}

/* ölçüm: kullanılacak en fazla thread (0 = hepsi, 1 = tek) */
/* ölçüm: MSAA 4x aç / kapa */
EXPORT(engine_set_msaa) void engine_set_msaa(i32 on) { g.fb.samples = on ? RASTER_MSAA : 1u; }

/* ölçüm: köşe aşaması 0 = C kâhini, 1 = Matoki std_vert.mtk */
EXPORT(engine_set_vert_mode) void engine_set_vert_mode(i32 m) { g.vert_mode = m && g_std_vert; }

/* ölçüm: gökyüzü kubbesi aç / kapa */
EXPORT(engine_set_sky) void engine_set_sky(i32 on) { g.sky = on != 0; }

EXPORT(engine_set_threads) void engine_set_threads(i32 n) { jobs_set_limit(n > 0 ? (u32)n : 0); }

EXPORT(engine_set_codegen_scalar) void engine_set_codegen_scalar(i32 on) { codegen_set_scalar(on != 0); }

/* lit.mtk yerine geçecek kaynağı a'da derler; uniform yerleşimi uymazsa reddeder (yanlış ofsetten okuma olmasın).
 * Başarıda Matoki lit yolu yeni programa geçer. Çağrı kare dışında / raster boşken (önceki program artık çalışmıyor). */
static b32 lit_recompile(Arena* a, const char* src, u32 len, MatokiError* err, u32* wasm_bytes) {
    MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
    mem_zero(err, sizeof(*err));
    if (!matoki_compile(a, src, len, NULL, p, err) || !matoki_jit(a, p, err)) return FALSE;
    const GlobalDecl* bad = NULL;
    if (!lit_layout_ok_at(p, &bad)) {
        if (bad) {
            matoki_error(err, bad->line, bad->col, "uniform LitUniforms'taki adı / tipi / sırasıyla uyuşmuyor:", bad->name, bad->name_len);
        } else {
            matoki_error(err, 1, 1, "uniform 'tex' ve 'tint' gerekli (LitUniforms sırası)", NULL, 0);
        }
        return FALSE;
    }
    g_matoki_lit = p->fn_fast;
    g_matoki_pre = p->prelude;
    g_matoki_full = p->fn;
    if (wasm_bytes) *wasm_bytes = p->wasm.len;
    return TRUE;
}

EXPORT(engine_matoki_recompile) i32 engine_matoki_recompile(u32 len) {
    if (!g_src_buf || len > g_src_cap) return 0;
    usize mark = arena_mark(&g.perm);
    MatokiError e;
    if (!lit_recompile(&g.perm, g_src_buf, len, &e, NULL)) {
        arena_reset(&g.perm, mark);
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, "matoki yeniden derleme: ");
        matoki_format_error(&e, &sb);
        log_sb(&sb);
        return 0;
    }
    return 1;
}

/* ---- canlı shader editörü (M13 faz 6): F2 açar / kapatır, Ctrl+Enter lit.mtk'yi derler ---- */
#define EDIT_CODE_BYTES ((usize)8 << 20)   /* derleme başına (IR ≤ 65 536 komut + wasm + önsöz önbelleği) */

static b32 editor_setup(void) {
    if (g.edit_ready) return TRUE;
    const EmbedFile* f = embed_find("lit.mtk");
    if (!f) return FALSE;
    /* başarısız denemeden kalan arenalar yeniden kullanılır (her F2'de yeniden ayırıp sızdırmasın) */
    if (!g.edit_mem.base && !arena_init_sys(&g.edit_mem, (usize)1 << 20)) return FALSE;
    for (u32 k = 0; k < 2; k++) {
        if (!g.edit_code[k].base && !arena_init_sys(&g.edit_code[k], EDIT_CODE_BYTES)) return FALSE;
    }
    arena_reset(&g.edit_mem, 0);
    g.editor = ARENA_NEW(&g.edit_mem, CodeEditor);
    editor_init(g.editor, &g.edit_mem, (const char*)f->data, f->len);
    ui_init(&g.ui, &g.fb);
    g.edit_cur = 1;
    g.edit_ready = TRUE;
    return TRUE;
}

/* derleme isteği: yürürlükte olmayan arenaya; başarıda o arena yürürlüğe girer, eskisi sonraki denemede silinir */
static void editor_compile(void) {
    u32 next = g.edit_cur ^ 1u;
    arena_reset(&g.edit_code[next], 0);
    MatokiError e;
    u32 bytes = 0;
    f64 t0 = sys_now_ms();
    b32 ok = lit_recompile(&g.edit_code[next], g.editor->buf, g.editor->len, &e, &bytes);
    f64 ms = sys_now_ms() - t0;
    if (ok) {
        g.edit_cur = next;
        g.shader_mode = SHADER_MATOKI;   /* sonucu hemen göster */
        g.edit_ok++;
    } else {
        g.edit_fail++;
    }
    editor_set_diag(g.editor, ok ? NULL : &e, ms, bytes);
}

/* çözümlenmiş renge, panelin sağına (HUD sol üstte) */
static void editor_draw(void) {
    g.ui.fb = &g.fb;
    i32 w = MIN(g.fb.w - 16, MAX(320, g.fb.w * 11 / 20));
    i32 h = g.fb.h - 16;
    ui_begin(&g.ui, &g.ui_in);
    b32 compile = editor_update_and_draw(g.editor, &g.ui, &g.ui_in, g.fps_ms * 0.001f, g.fb.w - w - 8, 8, w, h);
    ui_end(&g.ui);
    if (compile) editor_compile();
}

EXPORT(engine_pointer) void engine_pointer(i32 x, i32 y, i32 buttons) {
    g.in.mouse_x = x;
    g.in.mouse_y = y;
    g.in.mouse_latch |= (u32)buttons & ~g.in.mouse_buttons;
    g.in.mouse_buttons = (u32)buttons;
}

EXPORT(engine_char) void engine_char(u32 cp) {
    if (g.in.char_count < ARRAY_COUNT(g.in.chars)) g.in.chars[g.in.char_count++] = cp;
}

/* JS: 1 → fareyi kilitleme, imleç konumu + karakter gönder */
EXPORT(engine_ui_active) i32 engine_ui_active(void) { return g.edit_open; }

/* teşhis: yakalanan quad'larda varying sınıfları; kind 0 = denormal, 1 = inf/NaN, 2 = |x| > 1e6 */
EXPORT(engine_capture_census) u32 engine_capture_census(i32 kind) {
    u32 n = 0;
    for (u32 i = 0; i < g_cap_n; i++) {
        b32 hit = FALSE;
        for (u32 v = 0; v < VAR3D_COUNT && !hit; v++) {
            for (u32 l = 0; l < 4; l++) {
                u32 b = m_f32_bits(g_cap_q[i].var[v][l]);
                u32 e = (b >> 23) & 0xFF;
                f32 x = g_cap_q[i].var[v][l];
                if ((kind == 0 && e == 0 && (b & 0x7FFFFF)) || (kind == 1 && e == 255) || (kind == 2 && e != 255 && m_absf(x) > 1e6f)) hit = TRUE;
            }
        }
        n += hit;
    }
    return n;
}

EXPORT(engine_raster_mode_count) i32 engine_raster_mode_count(void) { return RASTER_MODE_COUNT; }
EXPORT(engine_set_raster_mode) void engine_set_raster_mode(i32 mode) { raster_set_mode(&g.raster, (RasterMode)mode); }
