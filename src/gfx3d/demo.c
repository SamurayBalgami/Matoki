/* gfx3d/demo.c — 3D demo: wasm export'ları ve sahneler.
 * Sahne 1: dönen tuğla küp + zemin + 1000 küplük ızgara (kırpma/derinlik testi).
 * Sahne 2: ufka uzanan dama tahtası zemin; M tuşu mipmap'i açar/kapatır (moiré).
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
#include "core/rng.h"

#define ROCK_KINDS 4

#define PHYS_DT (1.0f / 60.0f)
#define KEY_G 71
#define KEY_T 84
#define KEY_V 86
#define KEY_N 78
#define KEY_K 75

/* sahne 4 oyun modu */
enum { PLAY_DRIVE, PLAY_WALK, PLAY_FLY, PLAY_COUNT };
#include "gfx3d/camera.h"
#include "gfx3d/pipeline.h"
#include "img/image.h"
#include "img/procgen.h"

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
    ST_COUNT
};

typedef struct Demo {
    b32 ready;
    Arena perm, frame, bins;
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
    LitUniforms u_water;
    Material m_water;
    i32 shader_mode;     /* SHADER_* */
    Material m_flat;
    u32 stats[ST_COUNT];
} Demo;

static Demo g;

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
/* vertex shader'lar (Matoki vert aşaması) */
static VertexFn g_std_vert, g_wave_vert;
static ShadeFn g_water;                 /* water.mtk (göl fragment) */
static PreludeFn g_water_pre;
static PreludeFn g_std_vert_pre, g_wave_vert_pre;

/* Matoki'de shader önsözle birlikte gelir; diğerlerinde önsöz yok */
static PreludeFn lit_prelude_for(i32 mode) {
    if (mode == SHADER_HQ && g_hq_lit) return g_hq_pre;
    return mode == SHADER_MATOKI && g_matoki_lit ? g_matoki_pre : NULL;
}

static ShadeFn lit_shader_for(i32 mode) {
    if (mode == SHADER_HQ && g_hq_lit) return g_hq_lit;
    if (mode == SHADER_MATOKI && g_matoki_lit) return g_matoki_lit;
#if defined(__wasm_simd128__)
    if (mode == SHADER_SIMD || mode == SHADER_MATOKI || mode == SHADER_HQ) return shade_lit_tex_simd;
#endif
    if (mode == SHADER_LUT) return shade_lit_tex_lut;
    return shade_lit_tex;
}

/* .mtk'yi derle + JIT (aşama: frag / vert); uniform yerleşimi denetimi çağıranda. */
static MatokiProgram* matoki_load(Arena* a, const char* file, u32 stage) {
    const EmbedFile* ef = embed_find(file);
    char buf[320];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    if (!ef) {
        sb_cstr(&sb, "matoki: gömülü değil: ");
        sb_cstr(&sb, file);
        log_sb(&sb);
        return NULL;
    }
    MatokiProgram* p = ARENA_NEW(a, MatokiProgram);
    MatokiError e;
    MatokiOptions opt;
    mem_zero(&opt, sizeof(opt));
    opt.stage = stage;
    f64 t0 = sys_now_ms();
    b32 ok = matoki_compile(a, (const char*)ef->data, ef->len, &opt, p, &e) && matoki_jit(a, p, &e);
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
        const GlobalDecl* tex = matoki_find_uniform(p, "tex");
        const GlobalDecl* tint = matoki_find_uniform(p, "tint");
        const GlobalDecl* tr = matoki_find_uniform(p, "translucency");
        layout_ok = tex && tint && tex->offset == __builtin_offsetof(LitUniforms, tex) && tint->offset == __builtin_offsetof(LitUniforms, tint) &&
                    (!tr || tr->offset == __builtin_offsetof(LitUniforms, translucency));
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

static void matoki_lit_init(Arena* a) {
    MatokiProgram* p = matoki_load(a, "lit.mtk", MATOKI_STAGE_FRAG);
    if (p) {
        g_matoki_lit = p->fn_fast;
        g_matoki_pre = p->prelude;
        g_matoki_full = p->fn;
    }
    MatokiProgram* hq = matoki_load(a, "lit_hq.mtk", MATOKI_STAGE_FRAG);
    if (hq) {
        g_hq_lit = hq->fn_fast;
        g_hq_pre = hq->prelude;
    }
    MatokiProgram* wf = matoki_load(a, "water.mtk", MATOKI_STAGE_FRAG);
    if (wf) {
        g_water = wf->fn_fast;
        g_water_pre = wf->prelude;
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

/* zamanla değişen rüzgâr (belirleyici: yalnızca simülasyon zamanına bağlı) */
static Vec3 wind_at(f32 t) {
    return v3(6.0f + 2.5f * m_sinf(t * 0.7f), 0.0f, 1.5f * m_sinf(t * 0.37f) + 1.0f * m_sinf(t * 1.3f + 1.0f));
}

static void scene_set(i32 s) {
    g.scene = s;
    camera_set_mode(&g.cam, CAM_FLY);
    g.cam.use_look = FALSE;
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

EXPORT(engine_init) i32 engine_init(i32 w, i32 h) {
    if (w <= 0 || h <= 0) return 0;
    if (!arena_init_sys(&g.perm, (usize)112 << 20)) return 0;   /* MSAA örnek tamponları 720p'de ~30 MB */
    if (!arena_init_sys(&g.frame, (usize)32 << 20)) return 0;
    if (!arena_init_sys(&g.bins, (usize)48 << 20)) return 0;
    if (!arena_init_sys(&g.soft_arena, (usize)8 << 20)) return 0;

    g.fb.w = w;
    g.fb.h = h;
    g.fb.color = ARENA_ARRAY(&g.perm, u32, (usize)w * (usize)h);
    g.fb.depth = ARENA_ARRAY(&g.perm, f32, (usize)w * (usize)h);
    /* MSAA 4x örnek tamponları (N tuşu açar/kapatır; varsayılan açık) */
    g.fb.color_ms = ARENA_ARRAY(&g.perm, u32, (usize)w * (usize)h * RASTER_MSAA);
    g.fb.depth_ms = ARENA_ARRAY(&g.perm, f32, (usize)w * (usize)h * RASTER_MSAA);
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

    g.cube = mesh_cube(&g.perm);
    g.ground = mesh_plane(&g.perm, 80.0f, 80.0f, 0.25f, 8);
    g.big_floor = mesh_plane(&g.perm, 4000.0f, 4000.0f, 0.5f, 1);
    g.water = mesh_plane(&g.perm, 40.0f, 40.0f, 0.15f, 128);   /* 129² köşe, 32 768 üçgen */

    lit_defaults(&g.u_bricks, g.tex_bricks, v3(1, 1, 1));
    lit_defaults(&g.u_grid[0], g.tex_grid, v3(1.0f, 0.55f, 0.45f));
    lit_defaults(&g.u_grid[1], g.tex_grid, v3(0.45f, 0.85f, 0.55f));
    lit_defaults(&g.u_grid[2], g.tex_grid, v3(0.5f, 0.6f, 1.0f));
    lit_defaults(&g.u_ground, g.tex_checker, v3(0.75f, 0.8f, 0.7f));
    lit_defaults(&g.u_floor, g.tex_checker, v3(1, 1, 1));
    g.u_floor.fog_start = 300.0f;
    g.u_floor.fog_end = 1800.0f;
    lit_defaults(&g.u_pillar, g.tex_bricks, v3(1, 1, 1));
    g.u_pillar.fog_start = 300.0f;
    g.u_pillar.fog_end = 1800.0f;
    material(&g.m_bricks, &g.u_bricks);
    for (u32 i = 0; i < 3; i++) material(&g.m_grid[i], &g.u_grid[i]);
    material(&g.m_ground, &g.u_ground);
    material(&g.m_floor, &g.u_floor);
    material(&g.m_pillar, &g.u_pillar);
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
    g.m_water.cull = CULL_NONE;          /* dalga tepesinden arka yüz görünebilir */
    g.m_water.vert_params[0] = 0.22f;    /* amp (m) */
    g.m_water.vert_params[1] = 7.0f;     /* en uzun dalga boyu (m) */
    g.m_water.vert_params[2] = 1.0f;     /* hız çarpanı */
    g.m_water.vert_params[3] = 0.6f;     /* sivrilik */
    g.m_water.cull_margin = 1.5f;
    g.u_jelly.translucency = 0.2f;
    material(&g.m_flag, &g.u_flag);
    material(&g.m_jelly, &g.u_jelly);
    phys_world_init(&g.phys, &g.perm, 1024, 8192);
    g.phys.allow_sleep = TRUE;

    g.mip = TRUE;
    /* varsayılan Matoki (C ile bit-bit aynı, ölçümde ortalama daha hızlı — tasks/M10_temel.md);
     * derlenemezse C SIMD'ye düşer */
    g.shader_mode = SHADER_SIMD;
    matoki_lit_init(&g.perm);
    if (g_matoki_lit) g.shader_mode = SHADER_MATOKI;
    if (g_hq_lit) g.shader_mode = SHADER_HQ;
    g.vert_mode = g_std_vert != NULL;
    scene_set(1);
    g.ready = TRUE;
    return 1;
}

static void draw(const Mesh* mesh, const Material* mat, Mat4 model) {
    DrawItem d;
    d.mesh = mesh;
    d.mat = g.flat ? &g.m_flat : mat;
    d.model = model;
    pipe_draw(&g.pipe, &d);
}

static void set_frame_uniforms(void) {
    LitUniforms* all[] = { &g.u_bricks, &g.u_grid[0], &g.u_grid[1], &g.u_grid[2], &g.u_ground, &g.u_floor, &g.u_pillar,
                           &g.u_car, &g.u_tire, &g.u_char, &g.u_rock, &g.u_terrain, &g.u_flag, &g.u_jelly, &g.u_water };
    for (u32 i = 0; i < ARRAY_COUNT(all); i++) {
        all[i]->eye = g.cam.pos;
        all[i]->use_mip = g.mip;
    }
    ShadeFn fn = g.capture ? shade_capture : lit_shader_for(g.shader_mode);
    PreludeFn pre = g.capture ? NULL : lit_prelude_for(g.shader_mode);
    Material* mats[] = { &g.m_bricks, &g.m_grid[0], &g.m_grid[1], &g.m_grid[2], &g.m_ground, &g.m_floor, &g.m_pillar,
                         &g.m_car, &g.m_tire, &g.m_char, &g.m_rock, &g.m_terrain, &g.m_flag, &g.m_jelly, &g.m_water };
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
    }
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
    soft_mesh_update(g.flag_mesh, g.flag, TRUE, 0.0f);
    draw(g.flag_mesh, &g.m_flag, mat4_identity());
    for (u32 k = 0; k < g.jelly_count; k++) {
        soft_mesh_update(g.jelly_mesh[k], g.jelly[k], FALSE, g.jelly[k]->radius);
        draw(g.jelly_mesh[k], &g.m_jelly, mat4_identity());
    }
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
static void render_scene7(void) {
    draw(g.big_floor, &g.m_floor, mat4_identity());
    draw(g.water, &g.m_water, mat4_translate(v3(0.0f, 0.9f, -8.0f)));
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
        draw(g.cube, &g.m_bricks, mat4_mul(mat4_translate(pos), mat4_mul(rot, mat4_scale(v3(1.4f, 1.4f, 1.4f)))));
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

EXPORT(engine_frame) void engine_frame(f32 dt_ms) {
    if (!g.ready) return;
    f64 t_start = sys_now_ms();
    f32 dt = m_clampf(dt_ms * 0.001f, 0.0f, 0.1f);
    g.time += dt;

    if (g.in.pressed[KEY_0 + 1]) scene_set(1);
    if (g.in.pressed[KEY_0 + 2]) scene_set(2);
    if (g.in.pressed[KEY_0 + 3]) scene_set(3);
    if (g.in.pressed[KEY_0 + 4]) scene_set(4);
    if (g.in.pressed[KEY_0 + 5]) scene_set(5);
    if (g.in.pressed[KEY_0 + 6]) scene_set(6);
    if (g.in.pressed[KEY_0 + 7]) scene_set(7);
    if (g.in.pressed[KEY_K] && g_std_vert) g.vert_mode = !g.vert_mode;
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
            phys_step(&g.phys, PHYS_DT);
            g.phys_ms += g.phys.stats.step_ms;
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
    g.in.mouse_dx = g.in.mouse_dy = g.in.wheel = 0.0f;
    mem_zero(g.in.pressed, sizeof(g.in.pressed));

    f64 t0 = sys_now_ms();
    fb_clear(&g.fb, color_pack(158, 184, 217, 255), 1.0f);
    f64 t_clear = sys_now_ms();
    arena_reset(&g.frame, 0);
    arena_reset(&g.bins, 0);
    raster_reset_stats(&g.raster);
    raster_set_depth(&g.raster, TRUE, TRUE, DEPTH_LESS);
    raster_begin(&g.raster, &g.bins);
    set_frame_uniforms();
    pipe_begin(&g.pipe, &g.raster, &g.frame, camera_view(&g.cam), camera_proj(&g.cam), g.cam.pos);
    pipe_set_time(&g.pipe, g.time);
    if (g.scene == 1) render_scene1();
    else if (g.scene == 2) render_scene2();
    else if (g.scene == 7) render_scene7();
    else if (g.scene == 3) render_scene3();
    else if (g.scene == 4) render_scene4();
    else if (g.scene == 6) render_scene6();
    else render_scene5();
    f64 t_geo = sys_now_ms();
    pipe_end(&g.pipe);
    f64 t1 = sys_now_ms();
    fb_resolve(&g.fb);   /* MSAA: örnekler → renk */
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
}

EXPORT(engine_framebuffer) u32* engine_framebuffer(void) { return g.fb.color; }
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
 * engine_scratch(n) kaynağın yazılacağı belleği verir. Başarı → 1. */
static char* g_src_buf;
EXPORT(engine_scratch) char* engine_scratch(u32 n) {
    g_src_buf = (char*)arena_alloc(&g.perm, n + 1, 1);
    return g_src_buf;
}

/* ölçüm: kullanılacak en fazla thread (0 = hepsi, 1 = tek) */
/* ölçüm: MSAA 4x aç / kapa */
EXPORT(engine_set_msaa) void engine_set_msaa(i32 on) { g.fb.samples = on ? RASTER_MSAA : 1u; }

/* ölçüm: köşe aşaması 0 = C kâhini, 1 = Matoki std_vert.mtk */
EXPORT(engine_set_vert_mode) void engine_set_vert_mode(i32 m) { g.vert_mode = m && g_std_vert; }

EXPORT(engine_set_threads) void engine_set_threads(i32 n) { jobs_set_limit(n > 0 ? (u32)n : 0); }

EXPORT(engine_set_codegen_scalar) void engine_set_codegen_scalar(i32 on) { codegen_set_scalar(on != 0); }

EXPORT(engine_matoki_recompile) i32 engine_matoki_recompile(u32 len) {
    MatokiProgram* p = ARENA_NEW(&g.perm, MatokiProgram);
    MatokiError e;
    if (!matoki_compile(&g.perm, g_src_buf, len, NULL, p, &e) || !matoki_jit(&g.perm, p, &e)) {
        char buf[300];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, "matoki yeniden derleme: ");
        matoki_format_error(&e, &sb);
        log_sb(&sb);
        return 0;
    }
    g_matoki_lit = p->fn_fast;
    g_matoki_pre = p->prelude;
    g_matoki_full = p->fn;
    return 1;
}

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
