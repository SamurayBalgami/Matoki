/* gfx2d/camera2d.c — Camera2D takip, kırpma ve matris hesapları.
 * Yumuşatma katsayısı 1 - 2^(-rate*dt): aynı sürede kare hızından bağımsız aynı yol alınır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/camera2d.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "gfx2d/camera2d.h"

void cam2d_init(Camera2D* c, f32 view_w, f32 view_h) {
    c->pos         = v2(view_w * 0.5f, view_h * 0.5f);
    c->zoom        = 1.0f;
    c->view        = v2(view_w, view_h);
    c->offset      = v2(0.0f, 0.0f);
    c->bounds      = rect(0.0f, 0.0f, 0.0f, 0.0f);
    c->deadzone    = v2(0.0f, 0.0f);
    c->follow_rate = 0.0f;
}

static f32 deadzone_target(f32 cur, f32 target, f32 dz) {
    if (target > cur + dz) return target - dz;
    if (target < cur - dz) return target + dz;
    return cur;
}

void cam2d_follow(Camera2D* c, Vec2 target, f32 dt) {
    Vec2 goal = v2(deadzone_target(c->pos.x, target.x, c->deadzone.x),
                     deadzone_target(c->pos.y, target.y, c->deadzone.y));
    f32 k = (c->follow_rate > 0.0f) ? (1.0f - m_exp2f(-c->follow_rate * dt)) : 1.0f;
    c->pos = v2_add(c->pos, v2_scale(v2_sub(goal, c->pos), k));
    cam2d_clamp(c);
}

void cam2d_clamp(Camera2D* c) {
    if (c->bounds.w <= 0.0f || c->bounds.h <= 0.0f) return;
    f32 hw = c->view.x * 0.5f / c->zoom;
    f32 hh = c->view.y * 0.5f / c->zoom;
    f32 x0 = c->bounds.x + hw, x1 = c->bounds.x + c->bounds.w - hw;
    f32 y0 = c->bounds.y + hh, y1 = c->bounds.y + c->bounds.h - hh;
    c->pos.x = (x0 > x1) ? (c->bounds.x + c->bounds.w * 0.5f) : m_clampf(c->pos.x, x0, x1);
    c->pos.y = (y0 > y1) ? (c->bounds.y + c->bounds.h * 0.5f) : m_clampf(c->pos.y, y0, y1);
}

Mat3 cam2d_matrix_parallax(const Camera2D* c, f32 factor, b32 pixel_snap) {
    /* ekran = (dünya - pos*factor) * zoom + view/2 + offset */
    f32 tx = -c->pos.x * factor * c->zoom + c->view.x * 0.5f + c->offset.x;
    f32 ty = -c->pos.y * factor * c->zoom + c->view.y * 0.5f + c->offset.y;
    if (pixel_snap) {
        tx = m_floorf(tx + 0.5f);
        ty = m_floorf(ty + 0.5f);
    }
    return mat3_2d(tx, ty, 0.0f, c->zoom, c->zoom);
}

Mat3 cam2d_matrix(const Camera2D* c, b32 pixel_snap) {
    return cam2d_matrix_parallax(c, 1.0f, pixel_snap);
}

Rect cam2d_visible_rect(const Camera2D* c) {
    f32 w = c->view.x / c->zoom, h = c->view.y / c->zoom;
    return rect(c->pos.x - w * 0.5f, c->pos.y - h * 0.5f, w, h);
}

Vec2 cam2d_screen_to_world(const Camera2D* c, Vec2 s) {
    return v2((s.x - c->offset.x - c->view.x * 0.5f) / c->zoom + c->pos.x,
                (s.y - c->offset.y - c->view.y * 0.5f) / c->zoom + c->pos.y);
}
