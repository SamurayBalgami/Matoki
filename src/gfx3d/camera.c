/* gfx3d/camera.c — kamera hareketi ve matrisleri.
 * Uçuş: WASD yatay, Space/E yukarı, Q/Ctrl aşağı, Shift ×4 hız, tekerlek hız ayarı.
 * Orbit: fare hedef etrafında döndürür, tekerlek yakınlaştırır, WASD hedefi kaydırır. */
#include "gfx3d/camera.h"

void camera_init(Camera* c, f32 aspect) {
    c->mode = CAM_FLY;
    c->pos = v3(0, 2, 8);
    c->yaw = 0.0f;
    c->pitch = 0.0f;
    c->fov_y = 70.0f * (M_PI_F / 180.0f);
    c->znear = 0.1f;
    c->zfar = 1000.0f;
    c->aspect = aspect;
    c->speed = 6.0f;
    c->sensitivity = 0.0025f;
    c->target = v3(0, 0, 0);
    c->distance = 10.0f;
}

Vec3 camera_forward(const Camera* c) {
    f32 cp = m_cosf(c->pitch);
    return v3(m_sinf(c->yaw) * cp, m_sinf(c->pitch), -m_cosf(c->yaw) * cp);
}

Vec3 camera_right(const Camera* c) { return v3(m_cosf(c->yaw), 0.0f, m_sinf(c->yaw)); }

void camera_set_mode(Camera* c, CamMode mode) {
    if (mode == c->mode) return;
    if (mode == CAM_ORBIT) {
        c->target = v3_add(c->pos, v3_scale(camera_forward(c), c->distance));
    }
    c->mode = mode;
}

void camera_update(Camera* c, const InputState* in, f32 dt) {
    c->yaw += in->mouse_dx * c->sensitivity;
    c->pitch -= in->mouse_dy * c->sensitivity;
    if (in->down[KEY_LEFT]) c->yaw -= 1.5f * dt;
    if (in->down[KEY_RIGHT]) c->yaw += 1.5f * dt;
    if (in->down[KEY_UP]) c->pitch += 1.5f * dt;
    if (in->down[KEY_DOWN]) c->pitch -= 1.5f * dt;
    const f32 lim = 1.55f;
    c->pitch = m_clampf(c->pitch, -lim, lim);
    if (c->yaw > M_PI_F) c->yaw -= M_TAU_F;
    if (c->yaw < -M_PI_F) c->yaw += M_TAU_F;

    if (c->mode == CAM_EXTERNAL) return;   /* yalnızca bakış */
    Vec3 fwd = camera_forward(c);
    Vec3 right = camera_right(c);
    Vec3 flat = v3_norm(v3(fwd.x, 0.0f, fwd.z));
    f32 spd = c->speed * (in->down[KEY_SHIFT] ? 4.0f : 1.0f) * dt;

    if (c->mode == CAM_FLY) {
        if (in->wheel != 0.0f) {
            c->speed *= in->wheel < 0 ? 1.25f : 0.8f;
            c->speed = m_clampf(c->speed, 0.5f, 500.0f);
        }
        Vec3 mv = v3(0, 0, 0);
        if (in->down[KEY_W]) mv = v3_add(mv, fwd);
        if (in->down[KEY_S]) mv = v3_sub(mv, fwd);
        if (in->down[KEY_D]) mv = v3_add(mv, right);
        if (in->down[KEY_A]) mv = v3_sub(mv, right);
        if (in->down[KEY_SPACE] || in->down[KEY_E]) mv.y += 1.0f;
        if (in->down[KEY_Q] || in->down[KEY_CTRL]) mv.y -= 1.0f;
        c->pos = v3_add(c->pos, v3_scale(mv, spd));
    } else {
        if (in->wheel != 0.0f) {
            c->distance *= in->wheel > 0 ? 1.15f : 0.87f;
            c->distance = m_clampf(c->distance, 0.2f, 2000.0f);
        }
        Vec3 mv = v3(0, 0, 0);
        if (in->down[KEY_W]) mv = v3_add(mv, flat);
        if (in->down[KEY_S]) mv = v3_sub(mv, flat);
        if (in->down[KEY_D]) mv = v3_add(mv, right);
        if (in->down[KEY_A]) mv = v3_sub(mv, right);
        if (in->down[KEY_SPACE] || in->down[KEY_E]) mv.y += 1.0f;
        if (in->down[KEY_Q] || in->down[KEY_CTRL]) mv.y -= 1.0f;
        c->target = v3_add(c->target, v3_scale(mv, spd));
        c->pos = v3_sub(c->target, v3_scale(fwd, c->distance));
    }
}

Mat4 camera_view(const Camera* c) {
    if (c->mode == CAM_EXTERNAL && c->use_look) return mat4_look_at(c->pos, c->look, v3(0, 1, 0));
    return mat4_look_at(c->pos, v3_add(c->pos, camera_forward(c)), v3(0, 1, 0));
}

Mat4 camera_proj(const Camera* c) { return mat4_perspective(c->fov_y, c->aspect, c->znear, c->zfar); }
