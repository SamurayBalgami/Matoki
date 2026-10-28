/* img/image.c — mip zinciri yerleşimi ve kutu filtresiyle indirgeme.
 * Kare olmayan dokularda bir kenar 1'e indiğinde o eksende indirgeme durur
 * (kaynak indeksi kenara kenetlenir), diğer eksen yarılanmaya devam eder. */
#include "img/image.h"
#include "core/sys.h"

u32 img_mip_level_count(u32 w, u32 h) {
    u32 m = w > h ? w : h;
    u32 n = 1;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

usize img_mip_offset(u32 w, u32 h, u32 level) {
    usize off = 0;
    for (u32 i = 0; i < level; i++) off += (usize)img_mip_dim(w, i) * img_mip_dim(h, i);
    return off;
}

usize img_mip_total(u32 w, u32 h, u32 levels) { return img_mip_offset(w, h, levels); }

void img_build_mips(u32* px, u32 w, u32 h, u32 levels) {
    for (u32 k = 1; k < levels; k++) {
        u32 sw = img_mip_dim(w, k - 1), sh = img_mip_dim(h, k - 1);
        u32 dw = img_mip_dim(w, k), dh = img_mip_dim(h, k);
        const u32* src = px + img_mip_offset(w, h, k - 1);
        u32* dst = px + img_mip_offset(w, h, k);
        for (u32 y = 0; y < dh; y++) {
            u32 y0 = MIN(y * 2, sh - 1), y1 = MIN(y * 2 + 1, sh - 1);
            for (u32 x = 0; x < dw; x++) {
                u32 x0 = MIN(x * 2, sw - 1), x1 = MIN(x * 2 + 1, sw - 1);
                u32 c[4] = { src[y0 * sw + x0], src[y0 * sw + x1], src[y1 * sw + x0], src[y1 * sw + x1] };
                u32 out = 0;
                for (u32 ch = 0; ch < 4; ch++) {
                    u32 s = 2;
                    for (u32 i = 0; i < 4; i++) s += (c[i] >> (ch * 8)) & 0xFF;
                    out |= (s >> 2) << (ch * 8);
                }
                dst[y * dw + x] = out;
            }
        }
    }
}

static b32 is_pow2(u32 x) { return x && !(x & (x - 1)); }

Texture* tex_create(Arena* a, u32 w, u32 h, u32 flags) {
    if (!is_pow2(w) || !is_pow2(h)) sys_panic("tex_create: boyut 2'nin kuvveti olmalı");
    Texture* t = ARENA_NEW(a, Texture);
    u32 levels = (flags & TEX_FLAG_MIPMAP) ? img_mip_level_count(w, h) : 1;
    t->px = ARENA_ARRAY(a, u32, img_mip_total(w, h, levels));
    t->w = w;
    t->h = h;
    t->w_mask = w - 1;
    t->h_mask = h - 1;
    t->flags = flags;
    u32 l2 = 0;
    while ((1u << l2) < w) l2++;
    t->log2_w = l2;
    t->levels = levels;
    tex_fill_level_offsets(t);
    return t;
}

void tex_fill_level_offsets(Texture* t) {
    u32 levels = t->levels ? t->levels : 1;
    if (levels > TEX_MAX_LEVELS) levels = TEX_MAX_LEVELS;
    for (u32 k = 0; k < TEX_MAX_LEVELS; k++) {
        u32 kk = k < levels ? k : levels - 1;
        t->lv_ofs[k] = (u32)img_mip_offset(t->w, t->h, kk);
        t->lv_w[k] = img_mip_dim(t->w, kk);
        t->lv_h[k] = img_mip_dim(t->h, kk);
        t->lv_fw[k] = (f32)t->lv_w[k];
        t->lv_fh[k] = (f32)t->lv_h[k];
    }
}

u32 tex_level_table(const Texture* t, TexLevel* out) {
    u32 levels = t->levels ? t->levels : 1;
    if (levels > TEX_MAX_LEVELS) levels = TEX_MAX_LEVELS;
    usize off = 0;
    for (u32 k = 0; k < TEX_MAX_LEVELS; k++) {
        u32 kk = k < levels ? k : levels - 1;   /* kullanılmayan girişler son seviyeyi tekrarlar */
        if (k < levels && k > 0) off += (usize)img_mip_dim(t->w, k - 1) * img_mip_dim(t->h, k - 1);
        u32 w = img_mip_dim(t->w, kk), h = img_mip_dim(t->h, kk);
        out[k].px = k < levels ? t->px + off : out[levels - 1].px;
        out[k].w = w;
        out[k].h = h;
        out[k].fw = (f32)w;
        out[k].fh = (f32)h;
    }
    return levels;
}

void tex_build_mips(Texture* t) { img_build_mips(t->px, t->w, t->h, t->levels ? t->levels : 1); }
