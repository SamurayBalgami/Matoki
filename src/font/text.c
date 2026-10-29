/* font/text.c — TrueType FontAtlas tabanlı UTF-8 metin ölçümü, kelime sarmalama (word wrap)
 * ve Framebuffer üzerine alfa harmanlamalı metin çizimi.
 * Bozuk UTF-8, negatif/aşırı koordinatlar, sıfır boyutlu Framebuffer ve aşırı kontur kalınlığına
 * karşı sınır denetimlidir; dinamik bellek (malloc/arena) ayırmadan akışlı (streaming) çalışır.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/text.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "font/text.h"
#include "raster/shade_abi.h"
#include "core/math.h"

static inline b32 is_wrap_space(u32 cp) {
    return (cp == ' ' || cp == '\t') ? TRUE : FALSE;
}

static inline b32 is_finite_pos(f32 v) {
    /* NaN ve <= 0 değerlerini reddeder; > 1e8 ise sarmalamasız kabul eder */
    return (v == v && v > 0.0f && v < 1e8f) ? TRUE : FALSE;
}

static inline f32 sanitize_spacing(f32 v) {
    if (!(v == v)) return 0.0f;
    return CLAMP(v, -1000.0f, 1000.0f);
}

static inline f32 compute_line_step(const FontAtlas* atlas, const TextStyle* style) {
    f32 lh = (atlas->line_height > 0.0f) ? atlas->line_height : atlas->px_height;
    if (lh <= 0.0f) lh = 16.0f;
    if (style && style->line_spacing == style->line_spacing && style->line_spacing > 0.0f) {
        f32 mult = CLAMP(style->line_spacing, 0.1f, 100.0f);
        return lh * mult;
    }
    return lh;
}

static void glyph_lookup_and_metrics(const FontAtlas* atlas, u32 cp,
                                     const FontGlyph** out_fg, u16* out_gid, f32* out_adv) {
    const FontGlyph* g = font_atlas_glyph(atlas, cp);
    u16 gid = 0u;
    f32 adv = 0.0f;

    if (g && (g->codepoint == cp || g->glyph_index != 0u)) {
        gid = g->glyph_index;
        adv = g->advance;
    } else if (atlas->font) {
        gid = ttf_glyph_index(atlas->font, cp);
        u16 adv_fu = 0u;
        if (ttf_glyph_metrics(atlas->font, gid, &adv_fu, NULL)) {
            adv = (f32)adv_fu * atlas->scale;
        } else if (g) {
            adv = g->advance;
        }
    } else if (g) {
        adv = g->advance;
    }

    if (out_fg)  *out_fg  = g;
    if (out_gid) *out_gid = gid;
    if (out_adv) *out_adv = adv;
}

typedef struct {
    const char* cur;
    const char* end_limit;
    b32         done;
    b32         pending_empty_line;
} WrapIter;

static void wrap_iter_init(WrapIter* it, const char* utf8) {
    it->cur                = utf8;
    it->end_limit          = utf8 ? (utf8 + TEXT_MAX_INPUT_BYTES) : NULL;
    it->done               = (!utf8 || *utf8 == '\0') ? TRUE : FALSE;
    it->pending_empty_line = FALSE;
}

static b32 wrap_iter_next(WrapIter* it, const FontAtlas* atlas,
                          b32 wrap_on, f32 max_width,
                          b32 use_kern, f32 letter_spacing,
                          TextLine* out_line) {
    if (it->pending_empty_line) {
        it->pending_empty_line = FALSE;
        it->done               = TRUE;
        out_line->start        = it->cur;
        out_line->byte_len     = 0u;
        out_line->width        = 0.0f;
        return TRUE;
    }
    if (it->done || !it->cur || it->cur >= it->end_limit || *it->cur == '\0') {
        it->done = TRUE;
        return FALSE;
    }

    const char* line_start      = it->cur;
    const char* p               = line_start;
    const char* trimmed_end     = line_start;
    f32         trimmed_w       = 0.0f;
    f32         cur_w           = 0.0f;
    const char* last_break_end  = NULL;
    const char* last_break_next = NULL;
    f32         last_break_w    = 0.0f;
    b32         has_prev        = FALSE;
    u16         prev_gid        = 0u;
    u32         char_count      = 0u;
    b32         seen_non_space  = FALSE;

    while (p < it->end_limit && *p != '\0') {
        const char* char_start = p;
        u32 cp = font_atlas_utf8_next(&p);
        if (p > it->end_limit) p = it->end_limit;
        if (cp == 0u) break;

        if (cp == '\r') {
            continue;
        }
        if (cp == '\n') {
            out_line->start    = line_start;
            out_line->byte_len = (u32)(trimmed_end - line_start);
            out_line->width    = trimmed_w;
            it->cur            = p;
            if (p >= it->end_limit || *p == '\0') {
                it->pending_empty_line = TRUE;
            }
            return TRUE;
        }

        u16 gid = 0u;
        f32 adv = 0.0f;
        glyph_lookup_and_metrics(atlas, cp, NULL, &gid, &adv);

        f32 kern = 0.0f;
        if (has_prev) {
            if (use_kern && atlas->font) {
                kern = (f32)ttf_kern_advance(atlas->font, prev_gid, gid) * atlas->scale;
            }
            kern += letter_spacing;
        }
        f32 next_w = cur_w + kern + adv;

        if (wrap_on && is_wrap_space(cp)) {
            if (seen_non_space) {
                if (last_break_end != trimmed_end) {
                    last_break_end = trimmed_end;
                    last_break_w   = trimmed_w;
                }
                last_break_next = p;
                cur_w           = next_w;
                prev_gid        = gid;
                has_prev        = TRUE;
                char_count++;
                continue;
            } else {
                if (char_count > 0u && next_w > max_width) {
                    out_line->start    = line_start;
                    out_line->byte_len = (u32)(char_start - line_start);
                    out_line->width    = cur_w;
                    while (p < it->end_limit && (*p == ' ' || *p == '\t')) p++;
                    it->cur = p;
                    if (it->cur >= it->end_limit || *it->cur == '\0') it->done = TRUE;
                    return TRUE;
                }
                cur_w       = next_w;
                trimmed_end = p;
                trimmed_w   = cur_w;
                prev_gid    = gid;
                has_prev    = TRUE;
                char_count++;
                continue;
            }
        }

        if (wrap_on && next_w > max_width) {
            if (last_break_end != NULL) {
                out_line->start    = line_start;
                out_line->byte_len = (u32)(last_break_end - line_start);
                out_line->width    = last_break_w;
                it->cur            = last_break_next;
                if (it->cur >= it->end_limit || *it->cur == '\0') it->done = TRUE;
                return TRUE;
            }
            if (char_count > 0u) {
                out_line->start    = line_start;
                out_line->byte_len = (u32)(char_start - line_start);
                out_line->width    = cur_w;
                it->cur            = char_start;
                return TRUE;
            }
            /* Tek bir karakter bile max_width'ten genişse: sonsuz döngüyü önlemek için 1 karakter ilerle */
            out_line->start    = line_start;
            out_line->byte_len = (u32)(p - line_start);
            out_line->width    = next_w;
            it->cur            = p;
            if (it->cur >= it->end_limit || *it->cur == '\0') it->done = TRUE;
            return TRUE;
        }

        cur_w       = next_w;
        trimmed_end = p;
        trimmed_w   = cur_w;
        prev_gid    = gid;
        has_prev    = TRUE;
        char_count++;
        if (!is_wrap_space(cp)) {
            seen_non_space = TRUE;
        }
    }

    out_line->start    = line_start;
    out_line->byte_len = (u32)(trimmed_end - line_start);
    out_line->width    = trimmed_w;
    it->cur            = p;
    it->done           = TRUE;
    return TRUE;
}

u32 text_wrap_ex(const FontAtlas* atlas, const char* utf8, f32 max_width,
                 const TextStyle* style, TextLine* out_lines, u32 max_lines) {
    if (!atlas || !utf8 || *utf8 == '\0') return 0u;

    b32 wrap_on        = is_finite_pos(max_width);
    b32 use_kern       = (!style || !style->no_kerning) ? TRUE : FALSE;
    f32 letter_spacing = style ? sanitize_spacing(style->letter_spacing) : 0.0f;

    WrapIter it;
    wrap_iter_init(&it, utf8);

    u32 count = 0u;
    TextLine line;
    while (wrap_iter_next(&it, atlas, wrap_on, max_width, use_kern, letter_spacing, &line)) {
        if (out_lines && count < max_lines) {
            out_lines[count] = line;
        }
        count++;
    }
    return count;
}

u32 text_wrap(const FontAtlas* atlas, const char* utf8, f32 max_width,
              TextLine* out_lines, u32 max_lines) {
    return text_wrap_ex(atlas, utf8, max_width, NULL, out_lines, max_lines);
}

f32 text_measure(const FontAtlas* atlas, const char* utf8, const TextStyle* style,
                 f32* out_w, f32* out_h) {
    if (!atlas || !utf8 || *utf8 == '\0') {
        if (out_w) *out_w = 0.0f;
        if (out_h) *out_h = 0.0f;
        return 0.0f;
    }

    f32 max_width      = style ? style->max_width : 0.0f;
    b32 wrap_on        = is_finite_pos(max_width);
    b32 use_kern       = (!style || !style->no_kerning) ? TRUE : FALSE;
    f32 letter_spacing = style ? sanitize_spacing(style->letter_spacing) : 0.0f;
    f32 line_step      = compute_line_step(atlas, style);

    WrapIter it;
    wrap_iter_init(&it, utf8);

    f32 max_w = 0.0f;
    u32 lines = 0u;
    TextLine line;
    while (wrap_iter_next(&it, atlas, wrap_on, max_width, use_kern, letter_spacing, &line)) {
        if (line.width > max_w) max_w = line.width;
        lines++;
    }

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (f32)lines * line_step;
    return max_w;
}

/* (x + 127) / 255 işleminin 0 <= x <= 65025 aralığında bit-bit eşdeğeri hızlı kaydırma özdeşliği */
static inline u32 div255_round(u32 x) {
    u32 t = x + 128u;
    return (t + (t >> 8u)) >> 8u;
}

static void blit_glyph_alpha(Framebuffer* fb, const FontAtlas* atlas,
                             const FontGlyph* fg, i64 dst_x, i64 dst_y, u32 color) {
    if (!fg || fg->w == 0u || fg->h == 0u) return;
    if ((u64)fg->x + (u64)fg->w > (u64)atlas->w ||
        (u64)fg->y + (u64)fg->h > (u64)atlas->h) {
        return;
    }

    u32 sa = (color >> 24u) & 0xFFu;
    if (sa == 0u) return;

    i64 fb_w = (i64)MIN(((u32)fb->w), (u32)I32_MAX);
    i64 fb_h = (i64)MIN(((u32)fb->h), (u32)I32_MAX);
    i64 gw   = (i64)fg->w;
    i64 gh   = (i64)fg->h;

    i64 vis_x0 = (dst_x > 0) ? dst_x : 0;
    i64 vis_y0 = (dst_y > 0) ? dst_y : 0;
    i64 vis_x1 = (dst_x + gw < fb_w) ? (dst_x + gw) : fb_w;
    i64 vis_y1 = (dst_y + gh < fb_h) ? (dst_y + gh) : fb_h;
    if (vis_x0 >= vis_x1 || vis_y0 >= vis_y1) return;

    u32 src_off_x  = (u32)(vis_x0 - dst_x);
    u32 src_off_y  = (u32)(vis_y0 - dst_y);
    u32 vis_w      = (u32)(vis_x1 - vis_x0);
    u32 vis_h      = (u32)(vis_y1 - vis_y0);
    u32 sr         = (color >>  0u) & 0xFFu;
    u32 sg         = (color >>  8u) & 0xFFu;
    u32 sb         = (color >> 16u) & 0xFFu;
    u32 opaque_rgb = (color & 0x00FFFFFFu) | 0xFF000000u;
    usize fb_stride = (usize)((u32)fb->w);
    usize at_stride = (usize)atlas->w;

    for (u32 r = 0u; r < vis_h; r++) {
        const u8* src_row = atlas->px + (usize)(fg->y + src_off_y + r) * at_stride + (usize)(fg->x + src_off_x);
        u32*      dst_row = fb->color    + (usize)((u32)vis_y0 + r) * fb_stride       + (usize)(u32)vis_x0;

        for (u32 c = 0u; c < vis_w; c++) {
            u32 cov = src_row[c];
            if (cov == 0u) continue;

            u32 a = (sa == 255u) ? cov : div255_round(cov * sa);
            if (a == 255u) {
                dst_row[c] = opaque_rgb;
            } else if (a > 0u) {
                u32 d     = dst_row[c];
                u32 inv_a = 255u - a;
                u32 dr    = (d >>  0u) & 0xFFu;
                u32 dg    = (d >>  8u) & 0xFFu;
                u32 db    = (d >> 16u) & 0xFFu;
                u32 da    = (d >> 24u) & 0xFFu;
                u32 out_r = div255_round(sr * a + dr * inv_a);
                u32 out_g = div255_round(sg * a + dg * inv_a);
                u32 out_b = div255_round(sb * a + db * inv_a);
                u32 out_a = (da == 255u) ? 255u : (a + div255_round(da * inv_a));
                dst_row[c] = color_pack(out_r, out_g, out_b, out_a);
            }
        }
    }
}

static void draw_line_pass(Framebuffer* fb, const FontAtlas* atlas,
                           const TextLine* line, f32 start_x, i64 baseline_y,
                           b32 use_kern, f32 letter_spacing,
                           i64 off_x, i64 off_y, u32 color) {
    if ((color >> 24u) == 0u || line->byte_len == 0u) return;

    i64 fb_h = (i64)MIN(((u32)fb->h), (u32)I32_MAX);
    /* Satır tamamen ekranın çok üstünde veya altındaysa erken çık */
    i64 max_glyph_h = (i64)atlas->h + 64;
    if (baseline_y + off_y + max_glyph_h < 0 || baseline_y + off_y - max_glyph_h > fb_h) {
        return;
    }

    const char* p   = line->start;
    const char* end = line->start + line->byte_len;
    f32 pen_x       = start_x;
    b32 has_prev    = FALSE;
    u16 prev_gid    = 0u;

    while (p < end && *p != '\0') {
        u32 cp = font_atlas_utf8_next(&p);
        if (p > end) p = end;
        if (cp == 0u || cp == '\r' || cp == '\n') continue;

        const FontGlyph* fg = NULL;
        u16 gid = 0u;
        f32 adv = 0.0f;
        glyph_lookup_and_metrics(atlas, cp, &fg, &gid, &adv);

        if (has_prev) {
            if (use_kern && atlas->font) {
                i16 kern_fu = ttf_kern_advance(atlas->font, prev_gid, gid);
                pen_x += (f32)kern_fu * atlas->scale;
            }
            pen_x += letter_spacing;
        }

        if (fg && fg->w > 0u && fg->h > 0u &&
            pen_x > -32768.0f && pen_x < 32768.0f) {
            i64 gx = (i64)m_floorf(pen_x + 0.5f) + (i64)fg->x0 + off_x;
            i64 gy = baseline_y + (i64)fg->y0 + off_y;
            blit_glyph_alpha(fb, atlas, fg, gx, gy, color);
        }

        pen_x   += adv;
        prev_gid = gid;
        has_prev = TRUE;
    }
}

void text_draw(Framebuffer* fb, const FontAtlas* atlas, i32 x, i32 y,
               const char* utf8, const TextStyle* style) {
    if (!fb || !fb->color || ((u32)fb->w) == 0u || ((u32)fb->h) == 0u) return;
    if (!atlas || !atlas->px || atlas->w == 0u || atlas->h == 0u ||
        !atlas->glyphs || atlas->glyph_count == 0u) {
        return;
    }
    if (!utf8 || *utf8 == '\0') return;

    TextStyle def_style = text_style_default(color_pack(255, 255, 255, 255));
    const TextStyle* st = style ? style : &def_style;

    b32 has_main    = ((st->color >> 24u) != 0u) ? TRUE : FALSE;
    b32 has_shadow  = ((st->shadow_color >> 24u) != 0u &&
                       (st->shadow_dx != 0 || st->shadow_dy != 0)) ? TRUE : FALSE;
    i32 outline_r   = (st->outline_px > 0) ? MIN(st->outline_px, TEXT_MAX_OUTLINE_PX) : 0;
    b32 has_outline = (outline_r > 0 && (st->outline_color >> 24u) != 0u) ? TRUE : FALSE;

    if (!has_main && !has_shadow && !has_outline) return;

    f32 max_width      = st->max_width;
    b32 wrap_on        = is_finite_pos(max_width);
    b32 use_kern       = !st->no_kerning;
    f32 letter_spacing = sanitize_spacing(st->letter_spacing);
    f32 line_step      = compute_line_step(atlas, st);

    i64 base_y0 = st->y_is_baseline
        ? (i64)y
        : (i64)y + (i64)m_ceilf(atlas->ascent);

    WrapIter it;
    wrap_iter_init(&it, utf8);

    u32 line_idx = 0u;
    TextLine line;
    while (wrap_iter_next(&it, atlas, wrap_on, max_width, use_kern, letter_spacing, &line)) {
        f32 line_x = (f32)x;
        if (st->align == TEXT_ALIGN_CENTER) {
            if (wrap_on) {
                line_x = (f32)x + 0.5f * (max_width - line.width);
            } else {
                line_x = (f32)x - 0.5f * line.width;
            }
        } else if (st->align == TEXT_ALIGN_RIGHT) {
            if (wrap_on) {
                line_x = (f32)x + (max_width - line.width);
            } else {
                line_x = (f32)x - line.width;
            }
        }

        f32 dy_f       = (f32)line_idx * line_step;
        i64 baseline_y = base_y0 + (i64)m_floorf(dy_f + 0.5f);

        if (has_shadow) {
            draw_line_pass(fb, atlas, &line, line_x, baseline_y,
                           use_kern, letter_spacing,
                           (i64)st->shadow_dx, (i64)st->shadow_dy, st->shadow_color);
        }

        if (has_outline) {
            i32 r2_lim = outline_r * outline_r + 1;
            for (i32 oy = -outline_r; oy <= outline_r; oy++) {
                for (i32 ox = -outline_r; ox <= outline_r; ox++) {
                    if (ox == 0 && oy == 0) continue;
                    if (ox * ox + oy * oy > r2_lim) continue;
                    draw_line_pass(fb, atlas, &line, line_x, baseline_y,
                                   use_kern, letter_spacing,
                                   (i64)ox, (i64)oy, st->outline_color);
                }
            }
        }

        if (has_main) {
            draw_line_pass(fb, atlas, &line, line_x, baseline_y,
                           use_kern, letter_spacing,
                           0, 0, st->color);
        }

        line_idx++;
    }
}
