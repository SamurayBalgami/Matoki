/* gfx3d/camera.h — perspektif kamera: FPS uçuş (WASD + fare) ve orbit modları.
 * yaw = 0 → -Z'ye bakar; fare sağa → yaw artar. Giriş, platformdan gelen ham
 * tuş durumları (JS keyCode) ve fare deltalarıdır; mantık tamamen burada. */
#ifndef GFX3D_CAMERA_H
#define GFX3D_CAMERA_H
#include "core/math.h"

#define KEY_SHIFT 16
#define KEY_CTRL  17
#define KEY_SPACE 32
#define KEY_LEFT  37
#define KEY_UP    38
#define KEY_RIGHT 39
#define KEY_DOWN  40
#define KEY_0     48
#define KEY_A     65
#define KEY_C     67
#define KEY_D     68
#define KEY_E     69
#define KEY_F     70
#define KEY_L     76
#define KEY_M     77
#define KEY_P     80
#define KEY_Q     81
#define KEY_R     82
#define KEY_S     83
#define KEY_W     87
#define KEY_F1    112

typedef struct InputState {
    u8  down[256];
    u8  pressed[256];   /* bu karede basıldı (kenar) */
    f32 mouse_dx, mouse_dy;
    f32 wheel;
} InputState;

typedef enum CamMode {
    CAM_FLY,
    CAM_ORBIT,
    CAM_EXTERNAL   /* konumu oyun kodu belirler; fare yalnızca bakış (veya hedefe bak) */
} CamMode;

typedef struct Camera {
    CamMode mode;
    Vec3 pos;
    f32  yaw, pitch;
    f32  fov_y, znear, zfar, aspect;
    f32  speed;        /* m/s */
    f32  sensitivity;  /* radyan / piksel */
    Vec3 target;       /* orbit merkezi */
    f32  distance;     /* orbit uzaklığı */
    b32  use_look;     /* CAM_EXTERNAL: look_at hedefine bak (yaw/pitch yerine) */
    Vec3 look;
} Camera;

void camera_init(Camera* c, f32 aspect);
Vec3 camera_forward(const Camera* c);
Vec3 camera_right(const Camera* c);
void camera_update(Camera* c, const InputState* in, f32 dt);
Mat4 camera_view(const Camera* c);
Mat4 camera_proj(const Camera* c);
void camera_set_mode(Camera* c, CamMode mode);

#endif
