/* anim/skin.c — CPU Linear Blend Skinning (LBS) doğrulama, ağırlık temizleme ve skaler çekirdek.
 * Köşe başına 4 eklem matrisini ağırlıklarla birleştirip konum ve normalleri dönüştürür;
 * toplama ağacı sırası wasm SIMD sürümü (skin_wasm.c) ile birebir aynıdır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/skin.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "anim/skin.h"

static b32 mat4_is_finite(const Mat4* m) {
    for (u32 i = 0; i < 16; i++) {
        if (!anim_f32_finite(m->m[i])) return FALSE;
    }
    return TRUE;
}

b32 skin_validate_weights(const u16* joints, const f32* weights, u32 vertex_count, u32 joint_count) {
    if (!joints || !weights || vertex_count == 0u || vertex_count > SKIN_MAX_VERTICES) return FALSE;
    if (joint_count == 0u || joint_count > SKEL_MAX_JOINTS) return FALSE;

    for (u32 i = 0; i < vertex_count; i++) {
        const u16* j = joints + (usize)i * 4u;
        const f32* w = weights + (usize)i * 4u;
        f32 sum = 0.0f;
        for (u32 k = 0; k < 4u; k++) {
            if (j[k] >= joint_count) return FALSE;
            if (!anim_f32_finite(w[k]) || w[k] < 0.0f) return FALSE;
            sum += w[k];
        }
        if (!anim_f32_finite(sum) || sum <= 1e-6f) return FALSE;
    }
    return TRUE;
}

u32 skin_sanitize_weights(u16* joints, f32* weights, u32 vertex_count, u32 joint_count) {
    if (!joints || !weights || vertex_count == 0u || vertex_count > SKIN_MAX_VERTICES) return 1u;
    u32 jc = (joint_count > 0u && joint_count <= SKEL_MAX_JOINTS) ? joint_count : 1u;
    u32 fixed_verts = 0u;

    for (u32 i = 0; i < vertex_count; i++) {
        u16* j = joints + (usize)i * 4u;
        f32* w = weights + (usize)i * 4u;
        b32 bad = FALSE;
        for (u32 k = 0; k < 4u; k++) {
            if (j[k] >= jc) {
                j[k] = 0u;
                w[k] = 0.0f;
                bad = TRUE;
            }
            if (!anim_f32_finite(w[k]) || w[k] < 0.0f) {
                w[k] = 0.0f;
                bad = TRUE;
            }
        }
        f32 sum = (w[0] + w[1]) + (w[2] + w[3]);
        if (!anim_f32_finite(sum) || sum <= 1e-8f) {
            j[0] = 0u; j[1] = 0u; j[2] = 0u; j[3] = 0u;
            w[0] = 1.0f; w[1] = 0.0f; w[2] = 0.0f; w[3] = 0.0f;
            bad = TRUE;
        } else {
            f32 inv = 1.0f / sum;
            w[0] *= inv; w[1] *= inv; w[2] *= inv; w[3] *= inv;
        }
        if (bad) fixed_verts++;
    }
    return fixed_verts;
}

Mesh* skin_clone_mesh(Arena* a, const Mesh* bind_mesh) {
    if (!a || !bind_mesh || bind_mesh->vertex_count == 0u ||
        bind_mesh->vertex_count > SKIN_MAX_VERTICES) return NULL;
    u32 vn = bind_mesh->vertex_count;
    u32 in = bind_mesh->index_count;
    Mesh* m = mesh_alloc(a, vn, in);
    if (bind_mesh->color) m->color = ARENA_ARRAY(a, u32, vn);
    for (u32 i = 0; i < vn; i++) {
        m->px[i] = bind_mesh->px[i];
        m->py[i] = bind_mesh->py[i];
        m->pz[i] = bind_mesh->pz[i];
        m->nx[i] = bind_mesh->nx[i];
        m->ny[i] = bind_mesh->ny[i];
        m->nz[i] = bind_mesh->nz[i];
        m->u[i]  = bind_mesh->u[i];
        m->v[i]  = bind_mesh->v[i];
        if (m->color && bind_mesh->color) m->color[i] = bind_mesh->color[i];
    }
    for (u32 i = 0; i < in; i++) {
        m->indices[i] = bind_mesh->indices[i];
    }
    m->aabb_min = bind_mesh->aabb_min;
    m->aabb_max = bind_mesh->aabb_max;
    m->sphere_center = bind_mesh->sphere_center;
    m->sphere_radius = bind_mesh->sphere_radius;
    return m;
}

void skin_vertices_scalar(const Mat4* skin_mats, u32 joint_count,
                          const f32* in_px, const f32* in_py, const f32* in_pz,
                          const f32* in_nx, const f32* in_ny, const f32* in_nz,
                          const u16* joints, const f32* weights, u32 vertex_count,
                          f32* out_px, f32* out_py, f32* out_pz,
                          f32* out_nx, f32* out_ny, f32* out_nz) {
    if (!skin_mats || !in_px || !in_py || !in_pz || !joints || !weights ||
        !out_px || !out_py || !out_pz || vertex_count == 0u) return;

    u32 jc = (joint_count > 0u && joint_count <= SKEL_MAX_JOINTS) ? joint_count : 1u;
    Mat4 mats[SKEL_MAX_JOINTS];
    if (joint_count > 0u) {
        u32 limit = MIN(joint_count, SKEL_MAX_JOINTS);
        for (u32 j = 0; j < limit; j++) {
            mats[j] = mat4_is_finite(&skin_mats[j]) ? skin_mats[j] : mat4_identity();
        }
    } else {
        mats[0] = mat4_identity();
    }

    b32 has_nrm = (in_nx && in_ny && in_nz && out_nx && out_ny && out_nz);

    for (u32 i = 0; i < vertex_count; i++) {
        const u16* j = joints + (usize)i * 4u;
        const f32* w = weights + (usize)i * 4u;

        u16 j0 = j[0], j1 = j[1], j2 = j[2], j3 = j[3];
        f32 w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];

        if ((j0 >= jc) | (j1 >= jc) | (j2 >= jc) | (j3 >= jc)) {
            if (j0 >= jc) { j0 = 0u; w0 = 0.0f; }
            if (j1 >= jc) { j1 = 0u; w1 = 0.0f; }
            if (j2 >= jc) { j2 = 0u; w2 = 0.0f; }
            if (j3 >= jc) { j3 = 0u; w3 = 0.0f; }
        }

        f32 wsum = (w0 + w1) + (w2 + w3);
        if (!(w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f && w3 >= 0.0f &&
              wsum > 1e-8f && m_absf(wsum - 1.0f) <= 1e-4f)) {
            if (!anim_f32_finite(w0) || w0 < 0.0f) w0 = 0.0f;
            if (!anim_f32_finite(w1) || w1 < 0.0f) w1 = 0.0f;
            if (!anim_f32_finite(w2) || w2 < 0.0f) w2 = 0.0f;
            if (!anim_f32_finite(w3) || w3 < 0.0f) w3 = 0.0f;
            wsum = (w0 + w1) + (w2 + w3);
            if (!anim_f32_finite(wsum) || wsum <= 1e-8f) {
                j0 = 0u; w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; w3 = 0.0f;
            } else {
                f32 inv = 1.0f / wsum;
                w0 *= inv; w1 *= inv; w2 *= inv; w3 *= inv;
            }
        }

        const f32* m0 = mats[j0].m;
        const f32* m1 = mats[j1].m;
        const f32* m2 = mats[j2].m;
        const f32* m3 = mats[j3].m;

        f32 c0x = (m0[0] * w0 + m1[0] * w1) + (m2[0] * w2 + m3[0] * w3);
        f32 c0y = (m0[1] * w0 + m1[1] * w1) + (m2[1] * w2 + m3[1] * w3);
        f32 c0z = (m0[2] * w0 + m1[2] * w1) + (m2[2] * w2 + m3[2] * w3);

        f32 c1x = (m0[4] * w0 + m1[4] * w1) + (m2[4] * w2 + m3[4] * w3);
        f32 c1y = (m0[5] * w0 + m1[5] * w1) + (m2[5] * w2 + m3[5] * w3);
        f32 c1z = (m0[6] * w0 + m1[6] * w1) + (m2[6] * w2 + m3[6] * w3);

        f32 c2x = (m0[8]  * w0 + m1[8]  * w1) + (m2[8]  * w2 + m3[8]  * w3);
        f32 c2y = (m0[9]  * w0 + m1[9]  * w1) + (m2[9]  * w2 + m3[9]  * w3);
        f32 c2z = (m0[10] * w0 + m1[10] * w1) + (m2[10] * w2 + m3[10] * w3);

        f32 c3x = (m0[12] * w0 + m1[12] * w1) + (m2[12] * w2 + m3[12] * w3);
        f32 c3y = (m0[13] * w0 + m1[13] * w1) + (m2[13] * w2 + m3[13] * w3);
        f32 c3z = (m0[14] * w0 + m1[14] * w1) + (m2[14] * w2 + m3[14] * w3);

        f32 px = in_px[i], py = in_py[i], pz = in_pz[i];
        if (!(anim_f32_finite(px) && anim_f32_finite(py) && anim_f32_finite(pz))) {
            px = 0.0f; py = 0.0f; pz = 0.0f;
        }
        out_px[i] = (c0x * px + c1x * py) + (c2x * pz + c3x);
        out_py[i] = (c0y * px + c1y * py) + (c2y * pz + c3y);
        out_pz[i] = (c0z * px + c1z * py) + (c2z * pz + c3z);

        if (has_nrm) {
            f32 nx = in_nx[i], ny = in_ny[i], nz = in_nz[i];
            if (!(anim_f32_finite(nx) && anim_f32_finite(ny) && anim_f32_finite(nz))) {
                nx = 0.0f; ny = 1.0f; nz = 0.0f;
            }
            f32 rnx = (c0x * nx + c1x * ny) + c2x * nz;
            f32 rny = (c0y * nx + c1y * ny) + c2y * nz;
            f32 rnz = (c0z * nx + c1z * ny) + c2z * nz;
            Vec3 n = v3_norm(v3(rnx, rny, rnz));
            out_nx[i] = n.x;
            out_ny[i] = n.y;
            out_nz[i] = n.z;
        }
    }
}

#if !defined(__wasm_simd128__)
void skin_vertices(const Mat4* skin_mats, u32 joint_count,
                   const f32* in_px, const f32* in_py, const f32* in_pz,
                   const f32* in_nx, const f32* in_ny, const f32* in_nz,
                   const u16* joints, const f32* weights, u32 vertex_count,
                   f32* out_px, f32* out_py, f32* out_pz,
                   f32* out_nx, f32* out_ny, f32* out_nz) {
    skin_vertices_scalar(skin_mats, joint_count,
                         in_px, in_py, in_pz, in_nx, in_ny, in_nz,
                         joints, weights, vertex_count,
                         out_px, out_py, out_pz, out_nx, out_ny, out_nz);
}
#endif

b32 skin_mesh(const SkinnedMesh* sm, const Mat4* skin_mats, u32 joint_count, Mesh* out_mesh) {
    if (!sm || !sm->bind_mesh || !sm->joints || !sm->weights || !skin_mats || !out_mesh) {
        return FALSE;
    }
    const Mesh* bm = sm->bind_mesh;
    if (sm->vertex_count == 0u || sm->vertex_count != bm->vertex_count ||
        out_mesh->vertex_count != bm->vertex_count) {
        return FALSE;
    }
    if (joint_count == 0u || joint_count > SKEL_MAX_JOINTS) return FALSE;

    skin_vertices(skin_mats, joint_count,
                  bm->px, bm->py, bm->pz,
                  bm->nx, bm->ny, bm->nz,
                  sm->joints, sm->weights, sm->vertex_count,
                  out_mesh->px, out_mesh->py, out_mesh->pz,
                  out_mesh->nx, out_mesh->ny, out_mesh->nz);
    mesh_compute_bounds(out_mesh);
    return TRUE;
}
