/* gfx3d/shadow.c — gölge haritası kademeleri: kurulum, ışık izdüşümü oturtma ve yalnız derinlik geçişi.
 * Ortografik: x, y ∈ [l, r] × [b, t] → [-1, 1], görüş z ∈ [-n, -f] → [0, 1] (D2). Doku uzayı: u = (x + 1) / 2,
 * v = (1 − y) / 2 (satır 0 üstte, framebuffer gibi); derinlik aynen. Merkez texel ızgarasına yuvarlanır. */
#include "gfx3d/shadow.h"
#include "core/mem.h"

void shadow_init(ShadowMap* s, Arena* perm, u32 size) {
    mem_zero(s, sizeof(*s));
    s->size = size;
    f32* depth = ARENA_ARRAY(perm, f32, (usize)size * size * SHADOW_CASCADES);
    for (u32 k = 0; k < SHADOW_CASCADES; k++) {
        ShadowCascade* c = &s->c[k];
        c->fb.w = (i32)size;
        c->fb.h = (i32)size;
        c->fb.samples = 1;
        c->fb.depth = depth + (usize)k * size * size;
        c->fb.color = NULL;   /* yalnız derinlik çizimleri: renk yazılmaz */
        raster_init(&c->raster, &c->fb);
        c->view = c->proj = c->world_to_tex = mat4_identity();
        c->depth_range = 1.0f;
    }
    s->tex.px = (u32*)depth;
    s->tex.w = size;
    s->tex.h = size * SHADOW_CASCADES;
    s->tex.w_mask = size - 1u;
    s->tex.h_mask = s->tex.h - 1u;
    s->tex.log2_w = (u32)__builtin_ctz(size);
    s->tex.flags = TEX_FLAG_CLAMP;
    s->tex.levels = 1;
    tex_fill_level_offsets(&s->tex);
}

void shadow_fit(ShadowMap* s, u32 k, Vec3 sun_dir, Vec3 center, f32 radius, f32 caster_extra) {
    ShadowCascade* sc = &s->c[k];
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
    sc->view = rot;
    sc->proj = o;
    sc->texel_world = texel;
    sc->depth_range = f - n;
    Mat4 bias = mat4_identity();
    bias.m[0] = 0.5f;
    bias.m[12] = 0.5f;
    bias.m[5] = -0.5f;
    bias.m[13] = 0.5f;
    sc->world_to_tex = mat4_mul(bias, mat4_mul(o, rot));
}

void shadow_begin(ShadowMap* s, u32 k, Pipeline* p, Arena* frame, Arena* bins) {
    ShadowCascade* c = &s->c[k];
    usize n = (usize)s->size * s->size;
    for (usize i = 0; i < n; i++) c->fb.depth[i] = 1.0f;
    raster_begin(&c->raster, bins);
    raster_set_depth(&c->raster, TRUE, TRUE, DEPTH_LESS);
    raster_set_blend(&c->raster, BLEND_NONE);
    pipe_begin(p, &c->raster, frame, c->view, c->proj, v3(0.0f, 0.0f, 0.0f));
}

const Material* shadow_caster(void) {
    static const Material m = { .shade = NULL, .cull = CULL_FRONT };
    return &m;
}

void shadow_end(Pipeline* p) { pipe_end(p); }

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
