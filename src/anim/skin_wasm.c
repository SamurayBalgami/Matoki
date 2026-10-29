/* anim/skin_wasm.c — skin_vertices'in wasm SIMD128 ve Relaxed SIMD uygulaması.
 * Mat4 sütunlarını f32x4 vektörleri olarak ağırlıklandırır; normal SIMD derlemesinde toplama
 * ağacı sırası skaler sürüm (skin.c) ile aynıdır (bit-bit aynı), relaxed SIMD derlemesinde
 * (-mrelaxed-simd) simd_madd ile f32x4.relaxed_madd (FMA) kullanır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/anim/skin_wasm.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "anim/skin.h"
#include "core/simd.h"

static b32 mat4_is_finite_wasm(const Mat4* m) {
    for (u32 i = 0; i < 16; i++) {
        if (!anim_f32_finite(m->m[i])) return FALSE;
    }
    return TRUE;
}

static inline v128_t blend_col(const f32* m0, const f32* m1, const f32* m2, const f32* m3,
                               u32 off, v128_t vw0, v128_t vw1, v128_t vw2, v128_t vw3) {
    v128_t a = simd_madd(wasm_v128_load(m0 + off), vw0,
                         wasm_f32x4_mul(wasm_v128_load(m1 + off), vw1));
    v128_t b = simd_madd(wasm_v128_load(m2 + off), vw2,
                         wasm_f32x4_mul(wasm_v128_load(m3 + off), vw3));
    return wasm_f32x4_add(a, b);
}

void skin_vertices(const Mat4* skin_mats, u32 joint_count,
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
            mats[j] = mat4_is_finite_wasm(&skin_mats[j]) ? skin_mats[j] : mat4_identity();
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

        v128_t vw0 = wasm_f32x4_splat(w0);
        v128_t vw1 = wasm_f32x4_splat(w1);
        v128_t vw2 = wasm_f32x4_splat(w2);
        v128_t vw3 = wasm_f32x4_splat(w3);

        v128_t c0 = blend_col(m0, m1, m2, m3, 0u,  vw0, vw1, vw2, vw3);
        v128_t c1 = blend_col(m0, m1, m2, m3, 4u,  vw0, vw1, vw2, vw3);
        v128_t c2 = blend_col(m0, m1, m2, m3, 8u,  vw0, vw1, vw2, vw3);
        v128_t c3 = blend_col(m0, m1, m2, m3, 12u, vw0, vw1, vw2, vw3);

        f32 px = in_px[i], py = in_py[i], pz = in_pz[i];
        if (!(anim_f32_finite(px) && anim_f32_finite(py) && anim_f32_finite(pz))) {
            px = 0.0f; py = 0.0f; pz = 0.0f;
        }
        v128_t vp = wasm_f32x4_add(
            simd_madd(c0, wasm_f32x4_splat(px), wasm_f32x4_mul(c1, wasm_f32x4_splat(py))),
            simd_madd(c2, wasm_f32x4_splat(pz), c3));

        out_px[i] = wasm_f32x4_extract_lane(vp, 0);
        out_py[i] = wasm_f32x4_extract_lane(vp, 1);
        out_pz[i] = wasm_f32x4_extract_lane(vp, 2);

        if (has_nrm) {
            f32 nx = in_nx[i], ny = in_ny[i], nz = in_nz[i];
            if (!(anim_f32_finite(nx) && anim_f32_finite(ny) && anim_f32_finite(nz))) {
                nx = 0.0f; ny = 1.0f; nz = 0.0f;
            }
            v128_t vn = simd_madd(c2, wasm_f32x4_splat(nz),
                simd_madd(c0, wasm_f32x4_splat(nx), wasm_f32x4_mul(c1, wasm_f32x4_splat(ny))));

            f32 rnx = wasm_f32x4_extract_lane(vn, 0);
            f32 rny = wasm_f32x4_extract_lane(vn, 1);
            f32 rnz = wasm_f32x4_extract_lane(vn, 2);
            Vec3 n = v3_norm(v3(rnx, rny, rnz));
            out_nx[i] = n.x;
            out_ny[i] = n.y;
            out_nz[i] = n.z;
        }
    }
}
