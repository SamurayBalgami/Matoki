/* gfx2d/anim.c — AnimPlayer zaman ilerletme ve kare seçimi.
 * Döngülü kliplerde t, taşma birikmesin diye klip süresine göre sarılır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/anim.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "gfx2d/anim.h"
#include "core/math.h"

void anim_restart(AnimPlayer* p, const AnimClip* clip) {
    p->clip = clip;
    p->t    = 0.0f;
}

void anim_play(AnimPlayer* p, const AnimClip* clip) {
    if (p->clip != clip) anim_restart(p, clip);
}

void anim_update(AnimPlayer* p, f32 dt) {
    if (p->clip == NULL || p->clip->fps <= 0.0f) return;
    p->t += dt;
    f32 len = (f32)p->clip->count / p->clip->fps;
    if (p->clip->loop && len > 0.0f && p->t >= len) {
        p->t -= len * m_floorf(p->t / len);
    }
}

u32 anim_frame_index(const AnimPlayer* p) {
    if (p->clip == NULL || p->clip->count == 0) return 0;
    u32 i = (u32)(p->t * p->clip->fps);
    if (p->clip->loop) return i % p->clip->count;
    return (i < p->clip->count) ? i : p->clip->count - 1u;
}

u16 anim_frame(const AnimPlayer* p) {
    if (p->clip == NULL || p->clip->count == 0) return 0;
    return p->clip->frames[anim_frame_index(p)];
}

b32 anim_finished(const AnimPlayer* p) {
    if (p->clip == NULL || p->clip->loop) return FALSE;
    return (u32)(p->t * p->clip->fps) >= p->clip->count;
}
