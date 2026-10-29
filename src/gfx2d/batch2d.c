/* gfx2d/batch2d.c — 2D sprite yığınlayıcı uygulaması ve varsayılan sprite shader'ı.
 * Köşeler ekran pikselinde, z = 0, inv_w = 1 (perspektif yok); derinlik testi çağıranın raster durumundadır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/batch2d.c), bizim raster API'sine uyarlandı (D37). */
#include "gfx2d/batch2d.h"
#include "raster/ref_shaders.h"
#include "core/fmt.h"

static b32 mat3_is_identity(const Mat3* m) {
    static const f32 id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    for (u32 i = 0; i < 9; i++) {
        if (m->m[i] != id[i]) return FALSE;
    }
    return TRUE;
}

void shade_sprite(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const SpriteUniforms* u = (const SpriteUniforms*)uniforms;
    for (u32 l = 0; l < 4; l++) {
        u32 c = ref_sample_level(u->tex, q->var[0][l], q->var[1][l], 0);
        f32 ch[4];
        for (u32 k = 0; k < 4; k++) ch[k] = (f32)((c >> (k * 8)) & 0xFFu) * u->tint[k];
        if (u->cutout && ch[3] < 127.5f) {
            ((ShadeQuad*)q)->kill |= 1u << l;   /* sözleşme: kill raster'da yazılabilir */
            continue;
        }
        u32 p[4];
        for (u32 k = 0; k < 4; k++) p[k] = (u32)m_f2i_sat(m_clampf(ch[k], 0.0f, 255.0f) + 0.5f);
        out[l] = color_pack(p[0], p[1], p[2], p[3]);
    }
}

void b2d_begin(Batch2D* b, Raster* r, Arena* frame, Mat3 camera) {
    ASSERT(b && r && frame);
    b->raster = r;
    b->frame = frame;
    b->camera = camera;
    b->cam_is_identity = mat3_is_identity(&camera);
    b->shade_fn = shade_sprite;
    b->blend = B2D_OPAQUE;
    b->cur_tex = NULL;
    b->cur_tint = v4(1.0f, 1.0f, 1.0f, 1.0f);
    b->state_dirty = TRUE;
    b->sprites = 0;
}

void b2d_set_blend(Batch2D* b, B2dBlend blend) {
    if (b->blend != blend) {
        b->blend = blend;
        b->state_dirty = TRUE;
    }
}

void b2d_set_shader(Batch2D* b, ShadeFn shade_fn) {
    if (b->shade_fn != shade_fn) {
        b->shade_fn = shade_fn;
        b->state_dirty = TRUE;
    }
}

void b2d_sprite(Batch2D* b, const Texture* tex, Rect src_rect, Vec2 dst_pos, Vec2 size, f32 rot, Vec4 tint) {
    ASSERT(b && tex);
    if (b->cur_tex != tex) {
        b->cur_tex = tex;
        b->state_dirty = TRUE;
    }
    if (b->cur_tint.x != tint.x || b->cur_tint.y != tint.y || b->cur_tint.z != tint.z || b->cur_tint.w != tint.w) {
        b->cur_tint = tint;
        b->state_dirty = TRUE;
    }
    if (b->state_dirty) {
        SpriteUniforms* u = ARENA_TRY_ARRAY(b->frame, SpriteUniforms, 1);
        if (!u) return;   /* kare belleği dolu: sprite atlanır (arena fails sayar) */
        u->tex = b->cur_tex;
        u->tint[0] = tint.x;
        u->tint[1] = tint.y;
        u->tint[2] = tint.z;
        u->tint[3] = tint.w;
        u->cutout = b->blend == B2D_CUTOUT;
        raster_set_shader(b->raster, b->shade_fn, u, 2);
        b->state_dirty = FALSE;
    }

    f32 hx = size.x * 0.5f, hy = size.y * 0.5f;
    f32 c = 1.0f, s = 0.0f;
    if (rot != 0.0f) {
        c = m_cosf(rot);
        s = m_sinf(rot);
    }
    f32 rx_x = hx * c, rx_y = hx * s;
    f32 ry_x = -hy * s, ry_y = hy * c;
    Vec2 p[4] = {
        v2(dst_pos.x - rx_x - ry_x, dst_pos.y - rx_y - ry_y),
        v2(dst_pos.x + rx_x - ry_x, dst_pos.y + rx_y - ry_y),
        v2(dst_pos.x + rx_x + ry_x, dst_pos.y + rx_y + ry_y),
        v2(dst_pos.x - rx_x + ry_x, dst_pos.y - rx_y + ry_y),
    };
    if (!b->cam_is_identity) {
        for (u32 i = 0; i < 4; i++) p[i] = mat3_mul_point(&b->camera, p[i]);
    }

    f32 u0, v0, u1, v1;
    if (src_rect.w > 1.0f || src_rect.h > 1.0f) {   /* piksel dikdörtgeni */
        f32 iw = 1.0f / (f32)tex->w, ih = 1.0f / (f32)tex->h;
        u0 = src_rect.x * iw;
        v0 = src_rect.y * ih;
        u1 = (src_rect.x + src_rect.w) * iw;
        v1 = (src_rect.y + src_rect.h) * ih;
    } else {
        u0 = src_rect.x;
        v0 = src_rect.y;
        u1 = src_rect.x + src_rect.w;
        v1 = src_rect.y + src_rect.h;
    }
    const f32 uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    RasterVert rv[4];
    for (u32 i = 0; i < 4; i++) {
        rv[i] = (RasterVert){ 0 };
        rv[i].x = p[i].x;
        rv[i].y = p[i].y;
        rv[i].inv_w = 1.0f;
        rv[i].var[0] = uv[i][0];
        rv[i].var[1] = uv[i][1];
    }
    raster_triangle(b->raster, &rv[0], &rv[1], &rv[2]);
    raster_triangle(b->raster, &rv[0], &rv[2], &rv[3]);
    b->sprites++;
}

void b2d_end(Batch2D* b) {
    ASSERT(b);
    raster_flush(b->raster);
    b->cur_tex = NULL;
    b->state_dirty = TRUE;
}
