/* gfx3d/shadow.c — gölge haritası kurulumu, ışık izdüşümü oturtma ve yalnız derinlik geçişi.
 * Ortografik: x, y ∈ [l, r] × [b, t] → [-1, 1], görüş z ∈ [-n, -f] → [0, 1] (D2). Doku uzayı: u = (x + 1) / 2,
 * v = (1 − y) / 2 (satır 0 üstte, framebuffer gibi); derinlik aynen. Merkez texel ızgarasına yuvarlanır. */
#include "gfx3d/shadow.h"
#include "core/mem.h"

void shadow_init(ShadowMap* s, Arena* perm, u32 size) {
    mem_zero(s, sizeof(*s));
    s->size = size;
    s->fb.w = (i32)size;
    s->fb.h = (i32)size;
    s->fb.samples = 1;
    s->fb.depth = ARENA_ARRAY(perm, f32, (usize)size * size);
    s->fb.color = NULL;   /* yalnız derinlik çizimleri: renk yazılmaz */
    s->tex.px = (u32*)s->fb.depth;
    s->tex.w = s->tex.h = size;
    s->tex.w_mask = s->tex.h_mask = size - 1u;
    s->tex.log2_w = (u32)__builtin_ctz(size);
    s->tex.flags = TEX_FLAG_CLAMP;
    s->tex.levels = 1;
    tex_fill_level_offsets(&s->tex);
    raster_init(&s->raster, &s->fb);
    s->view = s->proj = s->world_to_tex = mat4_identity();
    s->depth_range = 1.0f;
}

void shadow_fit(ShadowMap* s, Vec3 sun_dir, Vec3 center, f32 radius, f32 caster_extra) {
    Vec3 up = m_absf(sun_dir.y) > 0.99f ? v3(0.0f, 0.0f, 1.0f) : v3(0.0f, 1.0f, 0.0f);
    /* yalnız dönme: öteleme izdüşüme konur → texel hizalama dünyaya sabit ızgarada */
    Mat4 rot = mat4_look_at(v3(0.0f, 0.0f, 0.0f), v3_scale(sun_dir, -1.0f), up);
    Vec4 c = mat4_mul_v4(&rot, v4(center.x, center.y, center.z, 1.0f));
    f32 texel = (2.0f * radius) / (f32)s->size;
    f32 cx = m_floorf(c.x / texel) * texel, cy = m_floorf(c.y / texel) * texel;
    f32 n = -c.z - radius - caster_extra, f = -c.z + radius;
    Mat4 o = mat4_identity();
    o.m[0] = 1.0f / radius;
    o.m[5] = 1.0f / radius;
    o.m[10] = -1.0f / (f - n);
    o.m[12] = -cx / radius;
    o.m[13] = -cy / radius;
    o.m[14] = -n / (f - n);
    s->view = rot;
    s->proj = o;
    s->texel_world = texel;
    s->depth_range = f - n;
    Mat4 bias = mat4_identity();
    bias.m[0] = 0.5f;
    bias.m[12] = 0.5f;
    bias.m[5] = -0.5f;
    bias.m[13] = 0.5f;
    s->world_to_tex = mat4_mul(bias, mat4_mul(o, rot));
}

void shadow_begin(ShadowMap* s, Pipeline* p, Arena* frame, Arena* bins) {
    usize n = (usize)s->size * s->size;
    for (usize i = 0; i < n; i++) s->fb.depth[i] = 1.0f;
    raster_begin(&s->raster, bins);
    raster_set_depth(&s->raster, TRUE, TRUE, DEPTH_LESS);
    raster_set_blend(&s->raster, BLEND_NONE);
    pipe_begin(p, &s->raster, frame, s->view, s->proj, v3(0.0f, 0.0f, 0.0f));
}

const Material* shadow_caster(void) {
    static const Material m = { .shade = NULL, .cull = CULL_FRONT };
    return &m;
}

void shadow_end(ShadowMap* s, Pipeline* p) {
    (void)s;
    pipe_end(p);
}

const Texture* shadow_none(void) {
    static u32 px[1];
    static Texture t;
    if (!t.px) {
        f32 two = 2.0f;
        mem_copy(&px[0], &two, 4);
        t.w = t.h = 1;
        t.flags = TEX_FLAG_CLAMP;
        t.levels = 1;
        tex_fill_level_offsets(&t);
        t.px = px;
    }
    return &t;
}
