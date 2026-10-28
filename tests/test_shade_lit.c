/* tests/test_shade_lit.c — lit shader varyantlarının bit-bit eşitliği ve mip tablosu.
 * shade_lit_tex (referans) == shade_lit_tex_lut == shade_lit_tex_simd; rastgele quad'lar ve uç
 * değerler (sıfır normal, NaN/sonsuz/dev UV, kare olmayan doku, clamp/wrap, mip açık/kapalı). */
#include "test.h"
#include "core/rng.h"
#include "img/procgen.h"
#include "raster/ref_shaders.h"

TEST(tex_level_table_matches_offsets) {
    static const u32 dims[4][2] = { { 256, 256 }, { 256, 16 }, { 16, 256 }, { 1, 1 } };
    for (u32 t = 0; t < 4; t++) {
        Texture* tex = tex_create(test_arena(), dims[t][0], dims[t][1], TEX_FLAG_MIPMAP);
        TexLevel lv[TEX_MAX_LEVELS];
        u32 n = tex_level_table(tex, lv);
        CHECK(n == tex->levels);
        for (u32 k = 0; k < TEX_MAX_LEVELS; k++) {
            u32 kk = k < n ? k : n - 1;
            CHECK(lv[k].px == tex->px + img_mip_offset(tex->w, tex->h, kk));
            CHECK(lv[k].w == img_mip_dim(tex->w, kk));
            CHECK(lv[k].h == img_mip_dim(tex->h, kk));
            CHECK(lv[k].fw == (f32)lv[k].w);
        }
    }
}

static f32 special(Rng* rng, f32 lo, f32 hi) {
    u32 r = rng_u32(rng) % 64;
    if (r == 0) return m_bits_f32(0x7FC00000u);    /* NaN */
    if (r == 1) return m_bits_f32(0x7F800000u);    /* +inf */
    if (r == 2) return -m_bits_f32(0x7F800000u);
    if (r == 3) return 0.0f;
    if (r == 4) return rng_range(rng, -1e9f, 1e9f);
    return rng_range(rng, lo, hi);
}

TEST(shade_lit_variants_bit_exact) {
    Arena* a = test_arena();
    Texture* texs[3];
    texs[0] = tex_create(a, 128, 128, TEX_FLAG_MIPMAP);
    procgen_bricks(texs[0], 3);
    texs[1] = tex_create(a, 64, 16, TEX_FLAG_MIPMAP | TEX_FLAG_CLAMP);
    texs[2] = tex_create(a, 32, 32, 0);
    for (u32 i = 0; i < img_mip_total(64, 16, texs[1]->levels); i++) texs[1]->px[i] = hash_u32(i) | 0xFF000000u;
    for (u32 i = 0; i < 32 * 32; i++) texs[2]->px[i] = hash_u32(i * 7 + 1);
    tex_build_mips(texs[0]);
    tex_build_mips(texs[1]);

    Rng rng = rng_make(0x5EED);
    u32 diff_lut = 0, diff_simd = 0, quads = 0;
    for (u32 it = 0; it < 60000; it++) {
        LitUniforms u;
        u.tex = texs[it % 3];
        u.to_light = v3_norm(v3(rng_range(&rng, -1, 1), rng_range(&rng, -1, 1), rng_range(&rng, -1, 1)));
        u.ambient = rng_range(&rng, 0, 1);
        u.light_color = v3(rng_range(&rng, 0, 2), rng_range(&rng, 0, 2), rng_range(&rng, 0, 2));
        u.fog_start = rng_range(&rng, 0, 50);
        u.fog_end = (it % 17 == 0) ? u.fog_start : u.fog_start + rng_range(&rng, 1, 500);
        u.fog_color = v3(rng_f32(&rng), rng_f32(&rng), rng_f32(&rng));
        u.eye = v3(rng_range(&rng, -50, 50), rng_range(&rng, -50, 50), rng_range(&rng, -50, 50));
        u.use_mip = (it & 1) != 0;
        u.tint = v3(rng_range(&rng, 0, 1.5f), rng_range(&rng, 0, 1.5f), rng_range(&rng, 0, 1.5f));
        lit_uniforms_prepare(&u);

        ShadeQuad q;
        q.x = (i32)(rng_u32(&rng) % 640);
        q.y = (i32)(rng_u32(&rng) % 360);
        q.mask = rng_u32(&rng) & 0xF;
        q.kill = 0;
        /* uv: komşu lane'ler arası türev ölçeği rastgele → farklı mip seviyeleri */
        f32 scale = rng_range(&rng, 0.0f, 0.2f) * (f32)(1u << (rng_u32(&rng) % 6));
        f32 u0 = special(&rng, -20, 20), v0 = special(&rng, -20, 20);
        for (u32 l = 0; l < 4; l++) {
            q.z[l] = rng_f32(&rng);
            for (u32 k = 0; k < 3; k++) q.var[VAR3D_WPOS + k][l] = special(&rng, -200, 200);
            for (u32 k = 0; k < 3; k++) q.var[VAR3D_NRM + k][l] = (it % 23 == 0) ? 0.0f : special(&rng, -2, 2);
            q.var[VAR3D_UV][l] = u0 + (f32)(l & 1) * scale + rng_range(&rng, -0.01f, 0.01f);
            q.var[VAR3D_UV + 1][l] = v0 + (f32)(l >> 1) * scale + rng_range(&rng, -0.01f, 0.01f);
        }

        u32 ref[4], lut[4], simd[4];
        shade_lit_tex(&q, &u, ref);
        shade_lit_tex_lut(&q, &u, lut);
        shade_lit_tex_simd(&q, &u, simd);
        for (u32 l = 0; l < 4; l++) {
            diff_lut += ref[l] != lut[l];
            diff_simd += ref[l] != simd[l];
        }
        quads++;
    }
    test_note("quad", quads, 0);
    CHECK(diff_lut == 0);
    CHECK(diff_simd == 0);
}
