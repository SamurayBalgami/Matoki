/* raster/ref_shaders_simd_wasm.c — shade_lit_tex'in wasm SIMD eşi (quad'ın 4 lane'i = bir f32x4).
 * Her işlem skaler lit_shade_lanes ile aynı sırada; m_maxf/m_clampf'in NaN davranışı
 * pmax/bitselect ile birebir taklit edilir → çıktı bit-bit aynı (FMA yok).
 * Texel okumaları skaler (wasm'da gather yok); adres hesabı vektörde. */
#include <wasm_simd128.h>
#include "raster/ref_shaders.h"

/* m_maxf(a, b) = a > b ? a : b  ≡  pmax(b, a) (pmax(x, y) = x < y ? y : x) */
static inline v128_t vmax(v128_t a, v128_t b) { return wasm_f32x4_pmax(b, a); }

/* m_clampf(x, lo, hi) = x < lo ? lo : (x > hi ? hi : x) */
static inline v128_t vclamp(v128_t x, v128_t lo, v128_t hi) {
    v128_t t = wasm_v128_bitselect(hi, x, wasm_f32x4_gt(x, hi));
    return wasm_v128_bitselect(lo, t, wasm_f32x4_lt(x, lo));
}

/* round-to-byte: m_f2i_sat(clamp(c, 0, 1) * 255 + 0.5) */
static inline v128_t to_byte(v128_t c, v128_t zero, v128_t one) {
    v128_t s = wasm_f32x4_add(wasm_f32x4_mul(vclamp(c, zero, one), wasm_f32x4_splat(255.0f)), wasm_f32x4_splat(0.5f));
    return wasm_i32x4_trunc_sat_f32x4(s);
}

void shade_lit_tex_simd(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const LitUniforms* u = (const LitUniforms*)uniforms;
    const v128_t zero = wasm_f32x4_splat(0.0f);
    const v128_t one = wasm_f32x4_splat(1.0f);

    /* ---- örnekleme ---- */
    const v128_t uu = wasm_v128_load(q->var[VAR3D_UV]);
    const v128_t vv = wasm_v128_load(q->var[VAR3D_UV + 1]);
    const TexLevel* lv = &u->lv[lit_mip_level(u, q->var[VAR3D_UV], q->var[VAR3D_UV + 1])];
    v128_t ix = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_floor(wasm_f32x4_mul(uu, wasm_f32x4_splat(lv->fw))));
    v128_t iy = wasm_i32x4_trunc_sat_f32x4(wasm_f32x4_floor(wasm_f32x4_mul(vv, wasm_f32x4_splat(lv->fh))));
    if (u->tex->flags & TEX_FLAG_CLAMP) {
        ix = wasm_i32x4_min(wasm_i32x4_max(ix, wasm_i32x4_splat(0)), wasm_i32x4_splat((i32)lv->w - 1));
        iy = wasm_i32x4_min(wasm_i32x4_max(iy, wasm_i32x4_splat(0)), wasm_i32x4_splat((i32)lv->h - 1));
    } else {
        ix = wasm_v128_and(ix, wasm_i32x4_splat((i32)(lv->w - 1)));
        iy = wasm_v128_and(iy, wasm_i32x4_splat((i32)(lv->h - 1)));
    }
    const v128_t idx = wasm_i32x4_add(wasm_i32x4_mul(iy, wasm_i32x4_splat((i32)lv->w)), ix);
    const u32* px = lv->px;
    const v128_t texel = wasm_i32x4_make((i32)px[(u32)wasm_i32x4_extract_lane(idx, 0)],
                                         (i32)px[(u32)wasm_i32x4_extract_lane(idx, 1)],
                                         (i32)px[(u32)wasm_i32x4_extract_lane(idx, 2)],
                                         (i32)px[(u32)wasm_i32x4_extract_lane(idx, 3)]);

    /* ---- normal: v3_norm ---- */
    v128_t nx = wasm_v128_load(q->var[VAR3D_NRM]);
    v128_t ny = wasm_v128_load(q->var[VAR3D_NRM + 1]);
    v128_t nz = wasm_v128_load(q->var[VAR3D_NRM + 2]);
    {
        v128_t l = wasm_f32x4_sqrt(wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_mul(nx, nx), wasm_f32x4_mul(ny, ny)),
                                                  wasm_f32x4_mul(nz, nz)));
        v128_t inv = wasm_f32x4_div(one, l);
        v128_t pos = wasm_f32x4_gt(l, zero);
        nx = wasm_v128_bitselect(wasm_f32x4_mul(nx, inv), nx, pos);
        ny = wasm_v128_bitselect(wasm_f32x4_mul(ny, inv), ny, pos);
        nz = wasm_v128_bitselect(wasm_f32x4_mul(nz, inv), nz, pos);
    }
    v128_t ndl = wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_mul(nx, wasm_f32x4_splat(u->to_light.x)),
                                               wasm_f32x4_mul(ny, wasm_f32x4_splat(u->to_light.y))),
                                wasm_f32x4_mul(nz, wasm_f32x4_splat(u->to_light.z)));
    ndl = vmax(ndl, zero);
    v128_t hemi = wasm_f32x4_mul(wasm_f32x4_splat(u->ambient),
                                 wasm_f32x4_add(wasm_f32x4_splat(0.75f), wasm_f32x4_mul(wasm_f32x4_splat(0.25f), ny)));
    v128_t lr = wasm_f32x4_add(hemi, wasm_f32x4_mul(wasm_f32x4_splat(u->light_color.x), ndl));
    v128_t lg = wasm_f32x4_add(hemi, wasm_f32x4_mul(wasm_f32x4_splat(u->light_color.y), ndl));
    v128_t lb = wasm_f32x4_add(hemi, wasm_f32x4_mul(wasm_f32x4_splat(u->light_color.z), ndl));

    /* ---- doku rengi × tint × ışık ---- */
    const v128_t k = wasm_f32x4_splat(1.0f / 255.0f);
    const v128_t m8 = wasm_i32x4_splat(0xFF);
    v128_t tr = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(texel, m8)), k);
    v128_t tg = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(texel, 8), m8)), k);
    v128_t tb = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_v128_and(wasm_u32x4_shr(texel, 16), m8)), k);
    v128_t cr = wasm_f32x4_mul(wasm_f32x4_mul(tr, wasm_f32x4_splat(u->tint.x)), lr);
    v128_t cg = wasm_f32x4_mul(wasm_f32x4_mul(tg, wasm_f32x4_splat(u->tint.y)), lg);
    v128_t cb = wasm_f32x4_mul(wasm_f32x4_mul(tb, wasm_f32x4_splat(u->tint.z)), lb);

    /* ---- sis ---- */
    f32 fog_scale = 1.0f / m_maxf(u->fog_end - u->fog_start, 1e-6f);
    v128_t dx = wasm_f32x4_sub(wasm_v128_load(q->var[VAR3D_WPOS]), wasm_f32x4_splat(u->eye.x));
    v128_t dy = wasm_f32x4_sub(wasm_v128_load(q->var[VAR3D_WPOS + 1]), wasm_f32x4_splat(u->eye.y));
    v128_t dz = wasm_f32x4_sub(wasm_v128_load(q->var[VAR3D_WPOS + 2]), wasm_f32x4_splat(u->eye.z));
    v128_t d = wasm_f32x4_sqrt(wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_mul(dx, dx), wasm_f32x4_mul(dy, dy)),
                                              wasm_f32x4_mul(dz, dz)));
    v128_t f = vclamp(wasm_f32x4_mul(wasm_f32x4_sub(d, wasm_f32x4_splat(u->fog_start)), wasm_f32x4_splat(fog_scale)),
                      zero, one);
    /* v3_lerp: a + (b - a) * t */
    cr = wasm_f32x4_add(cr, wasm_f32x4_mul(wasm_f32x4_sub(wasm_f32x4_splat(u->fog_color.x), cr), f));
    cg = wasm_f32x4_add(cg, wasm_f32x4_mul(wasm_f32x4_sub(wasm_f32x4_splat(u->fog_color.y), cg), f));
    cb = wasm_f32x4_add(cb, wasm_f32x4_mul(wasm_f32x4_sub(wasm_f32x4_splat(u->fog_color.z), cb), f));

    /* ---- paketle: r | g<<8 | b<<16 | 255<<24; maskesiz lane = 0 ---- */
    v128_t rgba = wasm_v128_or(wasm_v128_or(to_byte(cr, zero, one), wasm_i32x4_shl(to_byte(cg, zero, one), 8)),
                               wasm_v128_or(wasm_i32x4_shl(to_byte(cb, zero, one), 16), wasm_i32x4_splat((i32)0xFF000000u)));
    v128_t m = wasm_i32x4_ne(wasm_v128_and(wasm_i32x4_splat((i32)q->mask), wasm_i32x4_make(1, 2, 4, 8)),
                             wasm_i32x4_splat(0));
    wasm_v128_store(out, wasm_v128_and(rgba, m));
}
