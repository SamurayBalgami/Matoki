/* img/image.c — mip zinciri yerleşimi ve sRGB-doğru kutu filtresiyle indirgeme (renk doğrusal ışıkta ortalanır).
 * Kare olmayan dokularda bir kenar 1'e indiğinde o eksende indirgeme durur
 * (kaynak indeksi kenara kenetlenir), diğer eksen yarılanmaya devam eder. */
#include "img/image.h"
#include "core/math.h"
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

/* sRGB ↔ doğrusal tablolar: renk kanalları sRGB kodlu; doğru ortalama doğrusal ışıkta alınır
 * (sRGB'de ortalama siyah/beyazı 128'e, doğrusal ortalama ~188'e götürür; uzak doku kararır). */
static u16 g_s2l[256];      /* sRGB bayt → doğrusal, 0..65535 */
static u8  g_l2s[4096];     /* doğrusal 12 bit → sRGB bayt (en yakın) */
static b32 g_srgb_ready;

static f32 srgb_to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : m_powf((c + 0.055f) / 1.055f, 2.4f); }
static f32 linear_to_srgb(f32 c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * m_powf(c, 1.0f / 2.4f) - 0.055f; }

static void srgb_tables(void) {
    if (g_srgb_ready) return;
    for (u32 i = 0; i < 256; i++) g_s2l[i] = (u16)m_f2i_sat(srgb_to_linear((f32)i / 255.0f) * 65535.0f + 0.5f);
    for (u32 i = 0; i < 4096; i++) {
        f32 s = linear_to_srgb((f32)i / 4095.0f) * 255.0f + 0.5f;
        g_l2s[i] = (u8)CLAMP(m_f2i_sat(s), 0, 255);
    }
    g_srgb_ready = TRUE;
}

void img_build_mips(u32* px, u32 w, u32 h, u32 levels) {
    srgb_tables();
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
                for (u32 ch = 0; ch < 3; ch++) {
                    /* renk: doğrusal ışıkta ortalama (16 bit), 12 bite yuvarla, sRGB'ye geri */
                    u32 s = 0;
                    for (u32 i = 0; i < 4; i++) s += g_s2l[(c[i] >> (ch * 8)) & 0xFF];
                    out |= (u32)g_l2s[MIN((s + 32) >> 6, 4095u)] << (ch * 8);
                }
                u32 a = 2;   /* alfa doğrusal */
                for (u32 i = 0; i < 4; i++) a += c[i] >> 24;
                out |= (a >> 2) << 24;
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
