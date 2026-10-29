/* anim/clip.h — Animasyon klibi (AnimClip), anahtar kare örnekleyici (STEP, LINEAR, CUBICSPLINE)
 * ve iki poz/animasyon arasında ağırlıklı karıştırma (SLERP / NLERP).
 * glTF 2.0 animasyon kanallarını (translation, rotation, scale) doğrudan temsil eder.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/clip.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef ANIM_CLIP_H
#define ANIM_CLIP_H

#include "anim/skeleton.h"

#define ANIM_MAX_SAMPLERS 512u
#define ANIM_MAX_CHANNELS 512u

typedef enum {
    ANIM_INTERP_LINEAR      = 0,
    ANIM_INTERP_STEP        = 1,
    ANIM_INTERP_CUBICSPLINE = 2
} AnimInterp;

typedef enum {
    ANIM_PATH_TRANSLATION = 0,
    ANIM_PATH_ROTATION    = 1,
    ANIM_PATH_SCALE       = 2
} AnimPath;

typedef struct {
    u32        count;   /* anahtar kare sayısı (>= 1 geçerli) */
    u32        comps;   /* bileşen sayısı: 3 (Vec3) veya 4 (Quat) */
    AnimInterp interp;  /* LINEAR, STEP, CUBICSPLINE */
    const f32* times;   /* [count] saniye cinsinden zaman damgaları */
    const f32* values;  /* LINEAR/STEP: [count * comps], CUBICSPLINE: [count * 3 * comps] */
} AnimSampler;

typedef struct {
    u32      sampler;   /* AnimClip.samplers indeksi */
    u32      joint;     /* Skeleton eklem indeksi (0..joint_count-1) */
    i32      node;      /* glTF node indeksi (bilgi amaçlı, yoksa -1) */
    AnimPath path;      /* TRANSLATION, ROTATION, SCALE */
} AnimChannel;

typedef struct {
    char         name[64];
    f32          duration;      /* en büyük anahtar kare zamanı (saniye) */
    AnimSampler* samplers;
    u32          sampler_count;
    AnimChannel* channels;
    u32          channel_count;
} AnimClip;

/* Tek bir örnekleyiciyi verilen zamanda değerlendirir (out_val en az sampler->comps elemanlı).
 * Geçersiz veya 0 anahtar kareli örnekleyicide FALSE döner. */
b32 clip_eval_sampler(const AnimSampler* smp, f32 time, f32* out_val);

/* Klibin tüm örnekleyici ve kanallarını iskelet sınırlarına ve NaN/0-kare hatalarına karşı denetler */
b32 clip_validate(const AnimClip* clip, u32 joint_count);

/* Klibi verilen zamanda örnekler: önce skel->bind_pose kopyalanır, sonra kanallar uygulanır.
 * loop == TRUE ise zaman [0, duration] aralığına sarılır; FALSE ise uçlarda kırpılır. */
b32 clip_sample(const AnimClip* clip, const Skeleton* skel, f32 time, b32 loop,
                JointPose* out_poses);

/* İki poz dizisini weight (0..1) ağırlığıyla karıştırır (use_slerp: TRUE=SLERP, FALSE=NLERP) */
void clip_blend_poses(const JointPose* pose_a, const JointPose* pose_b, u32 joint_count,
                      f32 weight, b32 use_slerp, JointPose* out_poses);

/* İki klibi kendi zamanlarında örnekleyip ağırlıklı olarak tek poz dizisinde birleştirir */
b32 clip_sample_blended(const AnimClip* clip_a, f32 time_a, b32 loop_a,
                        const AnimClip* clip_b, f32 time_b, b32 loop_b,
                        f32 weight, b32 use_slerp, const Skeleton* skel,
                        JointPose* out_poses);

#endif
