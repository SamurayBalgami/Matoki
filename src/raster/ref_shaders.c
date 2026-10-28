/* raster/ref_shaders.c — referans örnekleme + 3D C shader'ları.
 * Mip seviyesi tamamen tamsayı aritmetiğiyle seçilir (karekök/log yok) → Matoki codegen'i
 * aynı komut dizisini üreterek bit-bit eşleşebilir. f32.max semantiği: NaN yayılır.
 * Dikkat: bu dosyadaki her işlem sırası sözleşmenin parçasıdır; "sadeleştirme" yapma. */
#include "raster/ref_shaders.h"
#include "img/image.h"

u32 ref_sample_level(const Texture* t, f32 u, f32 v, u32 level) {
    u32 levels = t->levels ? t->levels : 1;
    if (level >= levels) level = levels - 1;
    u32 wk = img_mip_dim(t->w, level), hk = img_mip_dim(t->h, level);
    const u32* base = t->px + img_mip_offset(t->w, t->h, level);
    i32 ix = m_f2i_sat(m_floorf(u * (f32)wk));
    i32 iy = m_f2i_sat(m_floorf(v * (f32)hk));
    if (t->flags & TEX_FLAG_CLAMP) {
        ix = CLAMP(ix, 0, (i32)wk - 1);
        iy = CLAMP(iy, 0, (i32)hk - 1);
    } else {
        ix &= (i32)(wk - 1);
        iy &= (i32)(hk - 1);
    }
    return base[(u32)iy * wk + (u32)ix];
}

static inline f32 wasm_f32_max(f32 a, f32 b) {
    if (a != a) return a;
    if (b != b) return b;
    return a > b ? a : b;
}

u32 ref_mip_level_from_rho2(f32 rho2, u32 levels) {
    if (levels <= 1) return 0;
    if (rho2 != rho2) return 0;
    u32 bits = m_f32_bits(rho2);
    i32 e = (i32)((bits >> 23) & 0xFF) - 127;
    if (e < 0) return 0;               /* rho2 < 1, sıfır, denormal */
    i32 level = e >> 1;                /* floor(log2(rho)) */
    const i32 lod_bias = 0;
    level += lod_bias;
    if (level < 0) level = 0;
    if (level > (i32)levels - 1) level = (i32)levels - 1;
    return (u32)level;
}

u32 ref_mip_level(const Texture* t, const f32 u[4], const f32 v[4]) {
    f32 fw = (f32)t->w, fh = (f32)t->h;
    f32 dudx = (u[1] - u[0]) * fw;
    f32 dvdx = (v[1] - v[0]) * fh;
    f32 dudy = (u[2] - u[0]) * fw;
    f32 dvdy = (v[2] - v[0]) * fh;
    f32 px2 = dudx * dudx + dvdx * dvdx;
    f32 py2 = dudy * dudy + dvdy * dvdy;
    return ref_mip_level_from_rho2(wasm_f32_max(px2, py2), t->levels ? t->levels : 1);
}

void ref_sample_tex_mip(const Texture* t, const f32 u[4], const f32 v[4], u32 out[4]) {
    u32 level = (t->flags & TEX_FLAG_MIPMAP) ? ref_mip_level(t, u, v) : 0;
    for (u32 l = 0; l < 4; l++) out[l] = ref_sample_level(t, u[l], v[l], level);
}

/* ---- 3D shader'lar ---- */

static inline Vec3 unpack_rgb(u32 c) {
    const f32 k = 1.0f / 255.0f;
    return v3((f32)(c & 0xFF) * k, (f32)((c >> 8) & 0xFF) * k, (f32)((c >> 16) & 0xFF) * k);
}

static inline u32 pack_rgb(Vec3 c) {
    u32 r = (u32)m_f2i_sat(m_clampf(c.x, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 g = (u32)m_f2i_sat(m_clampf(c.y, 0.0f, 1.0f) * 255.0f + 0.5f);
    u32 b = (u32)m_f2i_sat(m_clampf(c.z, 0.0f, 1.0f) * 255.0f + 0.5f);
    return color_pack(r, g, b, 255);
}

void lit_uniforms_prepare(LitUniforms* u) { u->lv_count = tex_level_table(u->tex, u->lv); }

/* Işık + sis; texel'ler örneklendikten sonra tüm uygulamalar bunu (veya SIMD eşini) kullanır. */
static void lit_shade_lanes(const ShadeQuad* q, const LitUniforms* u, const u32 texel[4], u32 out[4]) {
    f32 fog_scale = 1.0f / m_maxf(u->fog_end - u->fog_start, 1e-6f);
    for (u32 l = 0; l < 4; l++) {
        if (!(q->mask & (1u << l))) {
            out[l] = 0;
            continue;
        }
        Vec3 n = v3_norm(v3(q->var[VAR3D_NRM][l], q->var[VAR3D_NRM + 1][l], q->var[VAR3D_NRM + 2][l]));
        f32 ndl = m_maxf(v3_dot(n, u->to_light), 0.0f);
        /* yarım küre ortam ışığı: yukarı bakan yüzeyler biraz daha aydınlık */
        f32 hemi = u->ambient * (0.75f + 0.25f * n.y);
        Vec3 light = v3_add(v3(hemi, hemi, hemi), v3_scale(u->light_color, ndl));
        Vec3 c = v3_mul(v3_mul(unpack_rgb(texel[l]), u->tint), light);
        Vec3 p = v3(q->var[VAR3D_WPOS][l], q->var[VAR3D_WPOS + 1][l], q->var[VAR3D_WPOS + 2][l]);
        f32 d = v3_len(v3_sub(p, u->eye));
        f32 f = m_clampf((d - u->fog_start) * fog_scale, 0.0f, 1.0f);
        out[l] = pack_rgb(v3_lerp(c, u->fog_color, f));
    }
}

void shade_lit_tex(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const LitUniforms* u = (const LitUniforms*)uniforms;
    u32 texel[4];
    if (u->use_mip) {
        ref_sample_tex_mip(u->tex, q->var[VAR3D_UV], q->var[VAR3D_UV + 1], texel);
    } else {
        for (u32 l = 0; l < 4; l++) texel[l] = ref_sample_level(u->tex, q->var[VAR3D_UV][l], q->var[VAR3D_UV + 1][l], 0);
    }
    lit_shade_lanes(q, u, texel, out);
}

/* ref_sample_level ile aynı aritmetik; seviye ofseti tablodan. */
static inline u32 sample_lut(const TexLevel* lv, u32 flags, f32 u, f32 v) {
    i32 ix = m_f2i_sat(m_floorf(u * lv->fw));
    i32 iy = m_f2i_sat(m_floorf(v * lv->fh));
    if (flags & TEX_FLAG_CLAMP) {
        ix = CLAMP(ix, 0, (i32)lv->w - 1);
        iy = CLAMP(iy, 0, (i32)lv->h - 1);
    } else {
        ix &= (i32)(lv->w - 1);
        iy &= (i32)(lv->h - 1);
    }
    return lv->px[(u32)iy * lv->w + (u32)ix];
}

u32 lit_mip_level(const LitUniforms* u, const f32 uu[4], const f32 vv[4]) {
    if (!u->use_mip || !(u->tex->flags & TEX_FLAG_MIPMAP)) return 0;
    u32 level = ref_mip_level(u->tex, uu, vv);
    return level < u->lv_count ? level : u->lv_count - 1;
}

void shade_lit_tex_lut(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    const LitUniforms* u = (const LitUniforms*)uniforms;
    const f32* uu = q->var[VAR3D_UV];
    const f32* vv = q->var[VAR3D_UV + 1];
    const TexLevel* lv = &u->lv[lit_mip_level(u, uu, vv)];
    u32 texel[4];
    for (u32 l = 0; l < 4; l++) texel[l] = sample_lut(lv, u->tex->flags, uu[l], vv[l]);
    lit_shade_lanes(q, u, texel, out);
}

void shade_flat(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    UNUSED(uniforms);
    u32 c = color_pack((u32)(q->x & 0xFF), (u32)(q->y & 0xFF), 128, 255);
    for (u32 l = 0; l < 4; l++) out[l] = c;
}

void shade_uv_debug(const ShadeQuad* q, const void* uniforms, u32 out[4]) {
    UNUSED(uniforms);
    for (u32 l = 0; l < 4; l++) {
        f32 u = q->var[VAR3D_UV][l], v = q->var[VAR3D_UV + 1][l];
        out[l] = pack_rgb(v3(u - m_floorf(u), v - m_floorf(v), 0.25f));
    }
}
