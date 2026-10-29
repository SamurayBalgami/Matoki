/* anim/skeleton.c — İskelet ilklendirme, döngü denetimi, quaternion SLERP/NLERP ve dünya matrisi hesabı.
 * Özyinelemesiz 3-durumlu yürüyüş ile derin veya döngüsel eklem zincirlerinde yığın taşması ve
 * sonsuz döngü oluşması engellenir; bozuk/NaN girdiler birim dönüşüme temizlenir.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/skeleton.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "anim/skeleton.h"

static b32 vec3_finite(Vec3 v) {
    return anim_f32_finite(v.x) && anim_f32_finite(v.y) && anim_f32_finite(v.z);
}

static b32 quat_finite(Quat q) {
    return anim_f32_finite(q.x) && anim_f32_finite(q.y) &&
           anim_f32_finite(q.z) && anim_f32_finite(q.w);
}

static b32 mat4_finite(const Mat4* m) {
    for (u32 i = 0; i < 16; i++) {
        if (!anim_f32_finite(m->m[i])) return FALSE;
    }
    return TRUE;
}

static Quat quat_safe_norm(Quat q) {
    if (!quat_finite(q)) return quat_identity();
    f32 len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!anim_f32_finite(len2) || len2 <= 1e-20f) return quat_identity();
    f32 inv = 1.0f / m_sqrtf(len2);
    return quat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

JointPose joint_pose_identity(void) {
    JointPose p;
    p.translation = v3(0.0f, 0.0f, 0.0f);
    p.rotation    = quat_identity();
    p.scale       = v3(1.0f, 1.0f, 1.0f);
    return p;
}

JointPose joint_pose_trs(Vec3 t, Quat r, Vec3 s) {
    JointPose p;
    p.translation = vec3_finite(t) ? t : v3(0.0f, 0.0f, 0.0f);
    p.rotation    = quat_safe_norm(r);
    p.scale       = vec3_finite(s) ? s : v3(1.0f, 1.0f, 1.0f);
    return p;
}

Mat4 joint_pose_mat4(const JointPose* p) {
    if (!p) return mat4_identity();
    Vec3 t = vec3_finite(p->translation) ? p->translation : v3(0.0f, 0.0f, 0.0f);
    Vec3 s = vec3_finite(p->scale)       ? p->scale       : v3(1.0f, 1.0f, 1.0f);
    Quat q = quat_safe_norm(p->rotation);

    f32 x = q.x, y = q.y, z = q.z, w = q.w;
    f32 xx = x * x, yy = y * y, zz = z * z;
    f32 xy = x * y, xz = x * z, yz = y * z;
    f32 wx = w * x, wy = w * y, wz = w * z;

    Mat4 r;
    r.m[0]  = (1.0f - 2.0f * (yy + zz)) * s.x;
    r.m[1]  = (2.0f * (xy + wz)) * s.x;
    r.m[2]  = (2.0f * (xz - wy)) * s.x;
    r.m[3]  = 0.0f;

    r.m[4]  = (2.0f * (xy - wz)) * s.y;
    r.m[5]  = (1.0f - 2.0f * (xx + zz)) * s.y;
    r.m[6]  = (2.0f * (yz + wx)) * s.y;
    r.m[7]  = 0.0f;

    r.m[8]  = (2.0f * (xz + wy)) * s.z;
    r.m[9]  = (2.0f * (yz - wx)) * s.z;
    r.m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;
    r.m[11] = 0.0f;

    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    r.m[15] = 1.0f;
    return r;
}

JointPose joint_pose_from_mat4(Mat4 m) {
    if (!mat4_finite(&m)) return joint_pose_identity();
    Vec3 t = v3(m.m[12], m.m[13], m.m[14]);
    Vec3 c0 = v3(m.m[0], m.m[1], m.m[2]);
    Vec3 c1 = v3(m.m[4], m.m[5], m.m[6]);
    Vec3 c2 = v3(m.m[8], m.m[9], m.m[10]);
    f32 sx = v3_len(c0);
    f32 sy = v3_len(c1);
    f32 sz = v3_len(c2);
    Vec3 r0 = sx > 1e-6f ? v3_scale(c0, 1.0f / sx) : v3(1.0f, 0.0f, 0.0f);
    Vec3 r1 = sy > 1e-6f ? v3_scale(c1, 1.0f / sy) : v3(0.0f, 1.0f, 0.0f);
    Vec3 r2 = sz > 1e-6f ? v3_scale(c2, 1.0f / sz) : v3(0.0f, 0.0f, 1.0f);

    Quat q;
    f32 tr = r0.x + r1.y + r2.z;
    if (tr > 0.0f) {
        f32 s = m_sqrtf(tr + 1.0f) * 2.0f;
        q = quat((r1.z - r2.y) / s, (r2.x - r0.z) / s, (r0.y - r1.x) / s, 0.25f * s);
    } else if (r0.x > r1.y && r0.x > r2.z) {
        f32 s = m_sqrtf(m_maxf(0.0f, 1.0f + r0.x - r1.y - r2.z)) * 2.0f;
        q = s > 1e-6f ? quat(0.25f * s, (r0.y + r1.x) / s, (r2.x + r0.z) / s, (r1.z - r2.y) / s)
                      : quat_identity();
    } else if (r1.y > r2.z) {
        f32 s = m_sqrtf(m_maxf(0.0f, 1.0f + r1.y - r0.x - r2.z)) * 2.0f;
        q = s > 1e-6f ? quat((r0.y + r1.x) / s, 0.25f * s, (r1.z + r2.y) / s, (r2.x - r0.z) / s)
                      : quat_identity();
    } else {
        f32 s = m_sqrtf(m_maxf(0.0f, 1.0f + r2.z - r0.x - r1.y)) * 2.0f;
        q = s > 1e-6f ? quat((r2.x + r0.z) / s, (r1.z + r2.y) / s, 0.25f * s, (r0.y - r1.x) / s)
                      : quat_identity();
    }
    return joint_pose_trs(t, q, v3(sx, sy, sz));
}

Quat quat_nlerp(Quat a, Quat b, f32 t) {
    a = quat_safe_norm(a);
    b = quat_safe_norm(b);
    if (!anim_f32_finite(t)) t = 0.0f;
    t = m_clampf(t, 0.0f, 1.0f);
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b = quat(-b.x, -b.y, -b.z, -b.w);
    }
    Quat r = quat(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    );
    return quat_safe_norm(r);
}

Quat quat_slerp(Quat a, Quat b, f32 t) {
    a = quat_safe_norm(a);
    b = quat_safe_norm(b);
    if (!anim_f32_finite(t)) t = 0.0f;
    t = m_clampf(t, 0.0f, 1.0f);
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        b = quat(-b.x, -b.y, -b.z, -b.w);
        dot = -dot;
    }
    if (dot > 0.9995f) {
        Quat r = quat(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t
        );
        return quat_safe_norm(r);
    }
    dot = m_minf(dot, 1.0f);
    f32 sin_theta_0 = m_sqrtf(m_maxf(0.0f, 1.0f - dot * dot));
    if (sin_theta_0 <= 1e-6f) return a;
    f32 theta_0 = m_atan2f(sin_theta_0, dot);
    f32 s0 = m_sinf((1.0f - t) * theta_0) / sin_theta_0;
    f32 s1 = m_sinf(t * theta_0) / sin_theta_0;
    Quat r = quat(
        a.x * s0 + b.x * s1,
        a.y * s0 + b.y * s1,
        a.z * s0 + b.z * s1,
        a.w * s0 + b.w * s1
    );
    return quat_safe_norm(r);
}

b32 skeleton_init(Skeleton* skel, Arena* a, u32 joint_count) {
    if (!skel || !a || joint_count == 0u || joint_count > SKEL_MAX_JOINTS) return FALSE;
    __builtin_memset(skel, 0, sizeof(*skel));
    skel->joint_count    = joint_count;
    skel->parents        = ARENA_ARRAY(a, i32, joint_count);
    skel->inv_bind       = ARENA_ARRAY(a, Mat4, joint_count);
    skel->bind_pose      = ARENA_ARRAY(a, JointPose, joint_count);
    skel->node_indices   = ARENA_ARRAY(a, i32, joint_count);
    skel->skeleton_node  = -1;
    skel->root_transform = mat4_identity();
    for (u32 i = 0; i < joint_count; i++) {
        skel->parents[i]      = -1;
        skel->inv_bind[i]     = mat4_identity();
        skel->bind_pose[i]    = joint_pose_identity();
        skel->node_indices[i] = -1;
    }
    return TRUE;
}

b32 skeleton_validate(const Skeleton* skel) {
    if (!skel || skel->joint_count == 0u || skel->joint_count > SKEL_MAX_JOINTS) return FALSE;
    if (!skel->parents || !skel->inv_bind || !skel->bind_pose) return FALSE;
    if (!mat4_finite(&skel->root_transform)) return FALSE;

    u32 n = skel->joint_count;
    for (u32 i = 0; i < n; i++) {
        i32 p = skel->parents[i];
        if (p != -1) {
            if (p < 0 || (u32)p >= n || (u32)p == i) return FALSE;
        }
        /* Ata zincirinde döngü denetimi */
        i32 cur = p;
        u32 steps = 0;
        while (cur != -1) {
            if (cur < 0 || (u32)cur >= n) return FALSE;
            if ((u32)cur == i) return FALSE;
            if (++steps > n) return FALSE;
            cur = skel->parents[cur];
        }
        if (!mat4_finite(&skel->inv_bind[i])) return FALSE;
        if (!vec3_finite(skel->bind_pose[i].translation)) return FALSE;
        if (!quat_finite(skel->bind_pose[i].rotation)) return FALSE;
        if (!vec3_finite(skel->bind_pose[i].scale)) return FALSE;
    }
    return TRUE;
}

u32 skeleton_sanitize(Skeleton* skel) {
    if (!skel || skel->joint_count == 0u || skel->joint_count > SKEL_MAX_JOINTS) return 1u;
    u32 fixes = 0;
    u32 n = skel->joint_count;

    if (!mat4_finite(&skel->root_transform)) {
        skel->root_transform = mat4_identity();
        fixes++;
    }
    for (u32 i = 0; i < n; i++) {
        i32 p = skel->parents[i];
        if (p != -1 && (p < 0 || (u32)p >= n || (u32)p == i)) {
            skel->parents[i] = -1;
            fixes++;
        }
    }
    /* Döngü temizliği */
    u8 state[SKEL_MAX_JOINTS];
    __builtin_memset(state, 0, sizeof(state));
    for (u32 i = 0; i < n; i++) {
        if (state[i] == 2u) continue;
        u32 cur = i;
        while (state[cur] == 0u) {
            state[cur] = 1u;
            i32 p = skel->parents[cur];
            if (p == -1) break;
            if (state[p] == 1u) {
                /* Döngü tespit edildi: bu bağlantıyı kopar */
                skel->parents[cur] = -1;
                fixes++;
                break;
            }
            cur = (u32)p;
        }
        cur = i;
        while (state[cur] == 1u) {
            state[cur] = 2u;
            i32 p = skel->parents[cur];
            if (p == -1) break;
            cur = (u32)p;
        }
    }

    for (u32 i = 0; i < n; i++) {
        if (!mat4_finite(&skel->inv_bind[i])) {
            skel->inv_bind[i] = mat4_identity();
            fixes++;
        }
        JointPose* bp = &skel->bind_pose[i];
        if (!vec3_finite(bp->translation) || !quat_finite(bp->rotation) || !vec3_finite(bp->scale)) {
            *bp = joint_pose_trs(bp->translation, bp->rotation, bp->scale);
            fixes++;
        } else {
            f32 l2 = bp->rotation.x * bp->rotation.x + bp->rotation.y * bp->rotation.y +
                     bp->rotation.z * bp->rotation.z + bp->rotation.w * bp->rotation.w;
            if (l2 <= 1e-20f) {
                bp->rotation = quat_identity();
                fixes++;
            }
        }
    }
    return fixes;
}

b32 skeleton_compute_matrices(const Skeleton* skel, const JointPose* local_poses,
                              Mat4* out_world, Mat4* out_skin) {
    if (!skel || skel->joint_count == 0u || skel->joint_count > SKEL_MAX_JOINTS) return FALSE;
    if (!skel->parents || !skel->inv_bind || !skel->bind_pose) return FALSE;

    u32 n = skel->joint_count;
    Mat4 world_buf[SKEL_MAX_JOINTS];
    Mat4* world = out_world ? out_world : world_buf;
    u8  state[SKEL_MAX_JOINTS];
    u32 stack[SKEL_MAX_JOINTS];
    __builtin_memset(state, 0, sizeof(state));

    Mat4 root_tf = mat4_finite(&skel->root_transform) ? skel->root_transform : mat4_identity();
    const JointPose* poses = local_poses ? local_poses : skel->bind_pose;

    for (u32 i = 0; i < n; i++) {
        if (state[i] == 2u) continue;
        u32 sp = 0;
        i32 cur = (i32)i;
        while (cur >= 0 && (u32)cur < n && state[cur] == 0u) {
            state[cur] = 1u;
            stack[sp++] = (u32)cur;
            cur = skel->parents[cur];
        }
        Mat4 parent_world = (cur >= 0 && (u32)cur < n && state[cur] == 2u) ? world[cur] : root_tf;
        while (sp > 0u) {
            u32 j = stack[--sp];
            Mat4 loc = joint_pose_mat4(&poses[j]);
            world[j] = mat4_mul(parent_world, loc);
            state[j] = 2u;
            parent_world = world[j];
        }
    }

    if (out_skin) {
        for (u32 i = 0; i < n; i++) {
            Mat4 ibm = mat4_finite(&skel->inv_bind[i]) ? skel->inv_bind[i] : mat4_identity();
            out_skin[i] = mat4_mul(world[i], ibm);
        }
    }
    return TRUE;
}
