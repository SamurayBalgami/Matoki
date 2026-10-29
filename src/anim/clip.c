/* anim/clip.c — Anahtar kare arama, STEP/LINEAR/CUBICSPLINE değerlendirme ve poz karıştırma.
 * glTF 2.0 Ek C kübik Hermite spline formülünü uygular; zaman sınır dışında kırpılır,
 * 0 anahtar kare veya NaN içeren bozuk girdiler çökmeden reddedilir/temizlenir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/clip.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "anim/clip.h"

static void sanitize_floats(const f32* src, u32 comps, f32* dst) {
    for (u32 c = 0; c < comps; c++) {
        f32 v = src[c];
        dst[c] = anim_f32_finite(v) ? v : 0.0f;
    }
}

static void eval_keyframe_value(const AnimSampler* smp, u32 k, f32* out_val) {
    u32 comps = smp->comps;
    if (smp->interp == ANIM_INTERP_CUBICSPLINE) {
        /* CUBICSPLINE yerleşimi: [in_tangent, value, out_tangent] -> value = 3*k + 1 */
        sanitize_floats(smp->values + ((usize)k * 3u + 1u) * comps, comps, out_val);
    } else {
        sanitize_floats(smp->values + (usize)k * comps, comps, out_val);
    }
}

b32 clip_eval_sampler(const AnimSampler* smp, f32 time, f32* out_val) {
    if (!smp || !out_val || smp->count == 0u || !smp->times || !smp->values) return FALSE;
    if (smp->comps != 3u && smp->comps != 4u) return FALSE;

    u32 n = smp->count;
    u32 comps = smp->comps;
    if (!anim_f32_finite(time)) time = 0.0f;

    f32 t0 = smp->times[0];
    if (!anim_f32_finite(t0)) return FALSE;
    if (n == 1u || time <= t0) {
        eval_keyframe_value(smp, 0u, out_val);
        return TRUE;
    }

    f32 tn = smp->times[n - 1u];
    if (!anim_f32_finite(tn)) return FALSE;
    if (time >= tn) {
        eval_keyframe_value(smp, n - 1u, out_val);
        return TRUE;
    }

    /* İkili arama ile smp->times[k] <= time <= smp->times[k + 1] aralığını bul */
    u32 lo = 0u, hi = n - 1u;
    while (lo + 1u < hi) {
        u32 mid = lo + ((hi - lo) >> 1u);
        f32 tm = smp->times[mid];
        if (!anim_f32_finite(tm)) return FALSE;
        if (tm <= time) lo = mid;
        else hi = mid;
    }

    u32 k = lo;
    f32 tk0 = smp->times[k];
    f32 tk1 = smp->times[k + 1u];
    if (!anim_f32_finite(tk0) || !anim_f32_finite(tk1)) return FALSE;

    if (smp->interp == ANIM_INTERP_STEP) {
        eval_keyframe_value(smp, k, out_val);
        return TRUE;
    }

    f32 dt = tk1 - tk0;
    f32 u  = (dt > 1e-9f) ? m_clampf((time - tk0) / dt, 0.0f, 1.0f) : 0.0f;

    if (smp->interp == ANIM_INTERP_CUBICSPLINE) {
        f32 v0[4], b0[4], a1[4], v1[4];
        sanitize_floats(smp->values + ((usize)k * 3u + 1u) * comps, comps, v0);
        sanitize_floats(smp->values + ((usize)k * 3u + 2u) * comps, comps, b0);
        sanitize_floats(smp->values + ((usize)(k + 1u) * 3u + 0u) * comps, comps, a1);
        sanitize_floats(smp->values + ((usize)(k + 1u) * 3u + 1u) * comps, comps, v1);

        f32 u2  = u * u;
        f32 u3  = u2 * u;
        f32 h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        f32 h10 = u3 - 2.0f * u2 + u;
        f32 h01 = -2.0f * u3 + 3.0f * u2;
        f32 h11 = u3 - u2;
        f32 safe_dt = (dt > 0.0f) ? dt : 0.0f;

        for (u32 c = 0; c < comps; c++) {
            out_val[c] = h00 * v0[c] + h10 * safe_dt * b0[c] +
                         h01 * v1[c] + h11 * safe_dt * a1[c];
        }
        return TRUE;
    }

    /* ANIM_INTERP_LINEAR */
    f32 v0[4], v1[4];
    sanitize_floats(smp->values + (usize)k * comps, comps, v0);
    sanitize_floats(smp->values + (usize)(k + 1u) * comps, comps, v1);
    if (comps == 4u) {
        Quat q = quat_slerp(quat(v0[0], v0[1], v0[2], v0[3]),
                            quat(v1[0], v1[1], v1[2], v1[3]), u);
        out_val[0] = q.x; out_val[1] = q.y; out_val[2] = q.z; out_val[3] = q.w;
    } else {
        for (u32 c = 0; c < 3u; c++) {
            out_val[c] = v0[c] + (v1[c] - v0[c]) * u;
        }
    }
    return TRUE;
}

b32 clip_validate(const AnimClip* clip, u32 joint_count) {
    if (!clip || joint_count == 0u || joint_count > SKEL_MAX_JOINTS) return FALSE;
    if (clip->sampler_count == 0u || clip->sampler_count > ANIM_MAX_SAMPLERS || !clip->samplers) return FALSE;
    if (clip->channel_count == 0u || clip->channel_count > ANIM_MAX_CHANNELS || !clip->channels) return FALSE;
    if (!anim_f32_finite(clip->duration) || clip->duration < 0.0f) return FALSE;

    for (u32 i = 0; i < clip->sampler_count; i++) {
        const AnimSampler* s = &clip->samplers[i];
        if (s->count == 0u || !s->times || !s->values) return FALSE;
        if (s->comps != 3u && s->comps != 4u) return FALSE;
        if (s->interp != ANIM_INTERP_LINEAR && s->interp != ANIM_INTERP_STEP &&
            s->interp != ANIM_INTERP_CUBICSPLINE) return FALSE;

        for (u32 k = 0; k < s->count; k++) {
            if (!anim_f32_finite(s->times[k])) return FALSE;
            if (k > 0u && s->times[k] < s->times[k - 1u]) return FALSE;
        }
        usize val_count = (usize)s->count * (usize)s->comps *
                          (s->interp == ANIM_INTERP_CUBICSPLINE ? 3u : 1u);
        for (usize v = 0; v < val_count; v++) {
            if (!anim_f32_finite(s->values[v])) return FALSE;
        }
    }

    for (u32 i = 0; i < clip->channel_count; i++) {
        const AnimChannel* ch = &clip->channels[i];
        if (ch->sampler >= clip->sampler_count) return FALSE;
        if (ch->joint >= joint_count) return FALSE;
        u32 want_comps = (ch->path == ANIM_PATH_ROTATION) ? 4u : 3u;
        if (clip->samplers[ch->sampler].comps != want_comps) return FALSE;
    }
    return TRUE;
}

static f32 wrap_clip_time(f32 time, f32 duration, b32 loop) {
    if (!anim_f32_finite(time)) return 0.0f;
    if (!anim_f32_finite(duration) || duration <= 0.0f) return 0.0f;
    if (!loop) return m_clampf(time, 0.0f, duration);
    /* Tam Turda duration sınırında kalması için: time > 0 ve tam katıysa koru veya sar */
    f32 q = time / duration;
    if (!anim_f32_finite(q) || m_absf(q) >= 8388608.0f) return 0.0f;
    f32 w = time - m_floorf(q) * duration;
    return m_clampf(w, 0.0f, duration);
}

b32 clip_sample(const AnimClip* clip, const Skeleton* skel, f32 time, b32 loop,
                JointPose* out_poses) {
    if (!skel || !out_poses || skel->joint_count == 0u || skel->joint_count > SKEL_MAX_JOINTS) {
        return FALSE;
    }
    u32 jc = skel->joint_count;
    for (u32 i = 0; i < jc; i++) {
        out_poses[i] = skel->bind_pose ? skel->bind_pose[i] : joint_pose_identity();
    }
    if (!clip || clip->channel_count == 0u || !clip->channels || !clip->samplers) {
        return FALSE;
    }

    f32 t = loop ? wrap_clip_time(time, clip->duration, TRUE)
                 : (anim_f32_finite(time) ? time : 0.0f);

    b32 any_ok = FALSE;
    u32 ch_limit = MIN(clip->channel_count, ANIM_MAX_CHANNELS);
    for (u32 i = 0; i < ch_limit; i++) {
        const AnimChannel* ch = &clip->channels[i];
        if (ch->joint >= jc || ch->sampler >= clip->sampler_count) continue;
        const AnimSampler* smp = &clip->samplers[ch->sampler];
        u32 want_comps = (ch->path == ANIM_PATH_ROTATION) ? 4u : 3u;
        if (smp->comps != want_comps) continue;

        f32 val[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (!clip_eval_sampler(smp, t, val)) continue;
        any_ok = TRUE;

        JointPose* dst = &out_poses[ch->joint];
        if (ch->path == ANIM_PATH_TRANSLATION) {
            dst->translation = v3(val[0], val[1], val[2]);
        } else if (ch->path == ANIM_PATH_ROTATION) {
            dst->rotation = quat_normalize(quat(val[0], val[1], val[2], val[3]));
        } else if (ch->path == ANIM_PATH_SCALE) {
            dst->scale = v3(val[0], val[1], val[2]);
        }
    }
    return any_ok;
}

void clip_blend_poses(const JointPose* pose_a, const JointPose* pose_b, u32 joint_count,
                      f32 weight, b32 use_slerp, JointPose* out_poses) {
    if (!pose_a || !pose_b || !out_poses || joint_count == 0u) return;
    u32 n = MIN(joint_count, SKEL_MAX_JOINTS);
    if (!anim_f32_finite(weight)) weight = 0.0f;
    f32 w = m_clampf(weight, 0.0f, 1.0f);

    for (u32 i = 0; i < n; i++) {
        JointPose a = joint_pose_trs(pose_a[i].translation, pose_a[i].rotation, pose_a[i].scale);
        JointPose b = joint_pose_trs(pose_b[i].translation, pose_b[i].rotation, pose_b[i].scale);
        JointPose r;
        r.translation = v3(
            m_lerpf(a.translation.x, b.translation.x, w),
            m_lerpf(a.translation.y, b.translation.y, w),
            m_lerpf(a.translation.z, b.translation.z, w)
        );
        r.rotation = use_slerp ? quat_slerp(a.rotation, b.rotation, w)
                               : quat_nlerp(a.rotation, b.rotation, w);
        r.scale = v3(
            m_lerpf(a.scale.x, b.scale.x, w),
            m_lerpf(a.scale.y, b.scale.y, w),
            m_lerpf(a.scale.z, b.scale.z, w)
        );
        out_poses[i] = r;
    }
}

b32 clip_sample_blended(const AnimClip* clip_a, f32 time_a, b32 loop_a,
                        const AnimClip* clip_b, f32 time_b, b32 loop_b,
                        f32 weight, b32 use_slerp, const Skeleton* skel,
                        JointPose* out_poses) {
    if (!skel || !out_poses || skel->joint_count == 0u || skel->joint_count > SKEL_MAX_JOINTS) {
        return FALSE;
    }
    JointPose pa[SKEL_MAX_JOINTS];
    JointPose pb[SKEL_MAX_JOINTS];
    b32 ok_a = clip_sample(clip_a, skel, time_a, loop_a, pa);
    b32 ok_b = clip_sample(clip_b, skel, time_b, loop_b, pb);
    clip_blend_poses(pa, pb, skel->joint_count, weight, use_slerp, out_poses);
    return ok_a || ok_b;
}
