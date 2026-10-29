/* gfx2d/camera2d.h — 2D kamera: dünya → ekran dönüşümü, ölü bölge (deadzone) ile takip,
 * kare hızından bağımsız üstel yumuşatma, dünya sınırlarına kırpma ve piksel hizalama (pixel-art).
 * pos = görüş alanının dünya uzayındaki merkezi; zoom = ekran pikseli / dünya birimi.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/gfx2d/camera2d.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef GFX2D_CAMERA2D_H
#define GFX2D_CAMERA2D_H

#include "core/types.h"
#include "core/math.h"
#include "gfx2d/batch2d.h"

typedef struct {
    Vec2 pos;          /* görüş merkezi (dünya) */
    f32  zoom;
    Vec2 view;         /* ekran boyutu (piksel) */
    Vec2 offset;       /* ekran üzerindeki görüş alanı sol üstü (piksel), ör. üst çubuk için */
    Rect bounds;       /* dünya sınırları (w <= 0 ise kırpma yok) */
    Vec2 deadzone;     /* merkez etrafında hedefin serbest dolaşabildiği yarı-genişlik/yükseklik (dünya) */
    f32  follow_rate;  /* yumuşatma: saniyede kalan mesafenin 2^-rate'i kalır (0 = anında) */
} Camera2D;

void cam2d_init(Camera2D* c, f32 view_w, f32 view_h);
/* Hedefe doğru ölü bölge + üstel yumuşatma ile ilerler, sonra sınırlara kırpar */
void cam2d_follow(Camera2D* c, Vec2 target, f32 dt);
void cam2d_clamp(Camera2D* c);
/* Dünya → ekran. pixel_snap: öteleme tam piksele yuvarlanır (pixel-art'ta titreşim/aralık olmaz) */
Mat3 cam2d_matrix(const Camera2D* c, b32 pixel_snap);
/* Paralaks katman: factor 0 = sabit, 1 = dünya ile aynı */
Mat3 cam2d_matrix_parallax(const Camera2D* c, f32 factor, b32 pixel_snap);
Rect cam2d_visible_rect(const Camera2D* c);
Vec2 cam2d_screen_to_world(const Camera2D* c, Vec2 screen);

#endif
