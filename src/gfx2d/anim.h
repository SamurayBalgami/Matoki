/* gfx2d/anim.h — Sprite animasyonu: kare listesi (atlas indeksleri) + fps + döngü bayrağı.
 * AnimPlayer zamanı biriktirir; kare seçimi deterministiktir (t * fps tabanı).
 * Klip değişince zaman sıfırlanır, aynı klip yeniden çalınırsa devam eder.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/anim.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef GFX2D_ANIM_H
#define GFX2D_ANIM_H

#include "core/types.h"

typedef struct {
    const u16* frames;   /* atlas indeksleri */
    u32        count;
    f32        fps;
    b32        loop;
} AnimClip;

typedef struct {
    const AnimClip* clip;
    f32             t;   /* saniye */
} AnimPlayer;

void anim_play(AnimPlayer* p, const AnimClip* clip);   /* farklı klipse baştan başlar */
void anim_restart(AnimPlayer* p, const AnimClip* clip);
void anim_update(AnimPlayer* p, f32 dt);
u32  anim_frame_index(const AnimPlayer* p);            /* 0..count-1 */
u16  anim_frame(const AnimPlayer* p);                  /* atlas indeksi */
b32  anim_finished(const AnimPlayer* p);               /* döngüsüz klip son karede */

#endif
