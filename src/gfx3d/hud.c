/* gfx3d/hud.c — hud.h uygulaması.
 * Panel, en geniş satırın ölçüsü + iç boşlukla boyutlanır; metin 1 px gölgeli (koyu zeminde de okunur).
 * Font ve atlas MatokiEngine'den taşındı (font/, D37); burada yalnız düzen ve karışım var. */
#include "gfx3d/hud.h"
#include "core/math.h"
#include "font/matoki_sans_data.h"
#include "raster/shade_abi.h"

b32 hud_init(Hud* h, Arena* perm, Arena* scratch) {
    h->ready = FALSE;
    if (!ttf_parse(g_matoki_sans_ttf, g_matoki_sans_ttf_len, &h->font)) return FALSE;
    usize mark = arena_mark(scratch);
    b32 ok = font_atlas_build(&h->font, 14.0f, 0, 0, perm, scratch, &h->small) &&
             font_atlas_build(&h->font, 20.0f, 0, 0, perm, scratch, &h->big);
    arena_reset(scratch, mark);
    h->ready = ok;
    return ok;
}

const FontAtlas* hud_atlas(const Hud* h, const Framebuffer* fb) { return fb->h >= 900 ? &h->big : &h->small; }

static u32 div255(u32 x) { return (x + 128u + ((x + 128u) >> 8)) >> 8; }

void hud_panel(Framebuffer* fb, i32 x, i32 y, i32 w, i32 h, u32 rgba) {
    i32 x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN(x + w, fb->w), y1 = MIN(y + h, fb->h);
    u32 a = rgba >> 24, ia = 255u - a;
    for (i32 py = y0; py < y1; py++) {
        u32* row = fb->color + (usize)py * (usize)fb->w;
        for (i32 px = x0; px < x1; px++) {
            u32 d = row[px], r = 0;
            for (u32 k = 0; k < 24; k += 8) r |= div255(((rgba >> k) & 0xFFu) * a + ((d >> k) & 0xFFu) * ia) << k;
            row[px] = r | ((a + div255((d >> 24) * ia)) << 24);
        }
    }
}

i32 hud_draw_lines(const Hud* h, Framebuffer* fb, i32 x, i32 y, const char* const* lines, u32 n) {
    if (!h->ready || !fb->color || n == 0) return 0;
    const FontAtlas* at = hud_atlas(h, fb);
    f32 wmax = 0.0f;
    for (u32 i = 0; i < n; i++) wmax = m_maxf(wmax, font_measure(at, lines[i], NULL, NULL));
    i32 pad = (i32)(at->line_height * 0.45f);
    i32 lh = (i32)m_ceilf(at->line_height);
    i32 pw = (i32)m_ceilf(wmax) + 2 * pad, ph = lh * (i32)n + 2 * pad;
    hud_panel(fb, x, y, pw, ph, color_pack(8, 10, 16, 170));
    TextStyle st = text_style_default(color_pack(236, 240, 246, 255));
    st.shadow_color = color_pack(0, 0, 0, 220);
    st.shadow_dx = 1;
    st.shadow_dy = 1;
    for (u32 i = 0; i < n; i++) text_draw(fb, at, x + pad, y + pad + (i32)i * lh, lines[i], &st);
    return ph;
}
