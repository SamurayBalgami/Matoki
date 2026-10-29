/* font/atlas.c — Raf (shelf) tabanlı glif paketleyici, UTF-8 çözücü ve kerning destekli
 * metin ölçümü. ASCII + Latin-1 + Türkçe karakter kümesini tek bir u8 alfa dokusunda toplar.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (src/font/atlas.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "font/atlas.h"
#include "core/math.h"

/* Latin-1 dışında kalan 6 Türkçe Unicode kod noktası (çÇöÖüÜ zaten Latin-1 0xA0..0xFF içindedir) */
static const u32 EXTRA_TR_CPS[6] = {
    0x011Eu, /* Ğ */
    0x011Fu, /* ğ */
    0x0130u, /* İ */
    0x0131u, /* ı */
    0x015Eu, /* Ş */
    0x015Fu  /* ş */
};

u32 font_atlas_utf8_next(const char** str) {
    if (!str || !*str) return 0u;
    const u8* p = (const u8*)*str;
    u8 b0 = p[0];
    if (b0 == 0u) return 0u;

    if (b0 < 0x80u) {
        *str = (const char*)(p + 1);
        return (u32)b0;
    }

    if ((b0 & 0xE0u) == 0xC0u) {
        u8 b1 = p[1];
        if ((b1 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        u32 cp = (((u32)(b0 & 0x1Fu)) << 6u) | (u32)(b1 & 0x3Fu);
        if (cp < 0x80u) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        *str = (const char*)(p + 2);
        return cp;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
        u8 b1 = p[1];
        if ((b1 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        u8 b2 = p[2];
        if ((b2 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 2);
            return 0xFFFDu;
        }
        u32 cp = (((u32)(b0 & 0x0Fu)) << 12u) |
                 (((u32)(b1 & 0x3Fu)) <<  6u) |
                 ((u32)(b2 & 0x3Fu));
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        *str = (const char*)(p + 3);
        return cp;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
        u8 b1 = p[1];
        if ((b1 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        u8 b2 = p[2];
        if ((b2 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 2);
            return 0xFFFDu;
        }
        u8 b3 = p[3];
        if ((b3 & 0xC0u) != 0x80u) {
            *str = (const char*)(p + 3);
            return 0xFFFDu;
        }
        u32 cp = (((u32)(b0 & 0x07u)) << 18u) |
                 (((u32)(b1 & 0x3Fu)) << 12u) |
                 (((u32)(b2 & 0x3Fu)) <<  6u) |
                 ((u32)(b3 & 0x3Fu));
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            *str = (const char*)(p + 1);
            return 0xFFFDu;
        }
        *str = (const char*)(p + 4);
        return cp;
    }

    *str = (const char*)(p + 1);
    return 0xFFFDu;
}

static u32 collect_codepoints(u32* out_cps) {
    u32 n = 0u;
    out_cps[n++] = 0u; /* .notdef yedek glifi */
    for (u32 cp = 32u; cp <= 126u; cp++) {
        out_cps[n++] = cp;
    }
    for (u32 cp = 160u; cp <= 255u; cp++) {
        out_cps[n++] = cp;
    }
    for (u32 i = 0u; i < ARRAY_COUNT(EXTRA_TR_CPS); i++) {
        out_cps[n++] = EXTRA_TR_CPS[i];
    }
    return n;
}

b32 font_atlas_build(const TtfFont* font, f32 px_height, u32 atlas_w, u32 atlas_h,
                     Arena* out_arena, Arena* scratch, FontAtlas* out) {
    if (!font || !out_arena || !scratch || !out || px_height <= 0.0f || font->units_per_em == 0u) {
        return FALSE;
    }
    for (usize i = 0; i < sizeof(FontAtlas); i++) ((u8*)out)[i] = 0;

    f32 scale = px_height / (f32)font->units_per_em;
    if (atlas_w == 0u || atlas_h == 0u) {
        u32 auto_dim = 256u;
        u32 target = (u32)m_ceilf(px_height * 18.0f);
        while (auto_dim < target && auto_dim < 2048u) {
            auto_dim <<= 1;
        }
        if (atlas_w == 0u) atlas_w = auto_dim;
        if (atlas_h == 0u) atlas_h = auto_dim;
    }

    u32 cps[208];
    u32 cp_count = collect_codepoints(cps);

    out->font        = font;
    out->px_height   = px_height;
    out->scale       = scale;
    out->ascent      = (f32)font->ascent * scale;
    out->descent     = (f32)font->descent * scale;
    out->line_gap    = (f32)font->line_gap * scale;
    out->line_height = (f32)(font->ascent - font->descent + font->line_gap) * scale;
    if (out->line_height <= 0.0f) out->line_height = px_height;

    out->w           = atlas_w;
    out->h           = atlas_h;
    out->px          = ARENA_ARRAY(out_arena, u8, (usize)atlas_w * (usize)atlas_h);
    out->glyph_count = cp_count;
    out->glyphs      = ARENA_ARRAY(out_arena, FontGlyph, cp_count);

    u32 pad     = 1u;
    u32 shelf_x = pad;
    u32 shelf_y = pad;
    u32 shelf_h = 0u;
    f32 inv_w   = 1.0f / (f32)atlas_w;
    f32 inv_h   = 1.0f / (f32)atlas_h;

    for (u32 idx = 0u; idx < cp_count; idx++) {
        u32 cp  = cps[idx];
        u16 gid = (cp == 0u) ? 0u : ttf_glyph_index(font, cp);

        FontGlyph* fg = &out->glyphs[idx];
        fg->codepoint   = cp;
        fg->glyph_index = gid;

        usize mark = arena_mark(scratch);
        TtfGlyph tg;
        if (!ttf_load_glyph(font, gid, scratch, &tg)) {
            arena_reset(scratch, mark);
            return FALSE;
        }

        fg->advance   = (f32)tg.advance * scale;
        fg->bearing_x = (f32)tg.lsb * scale;

        i32 ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
        font_glyph_bbox(&tg, scale, scale, 0.0f, 0.0f, &ix0, &iy0, &ix1, &iy1);
        fg->x0        = ix0;
        fg->y0        = iy0;
        fg->x1        = ix1;
        fg->y1        = iy1;
        fg->bearing_y = -(f32)iy0;

        u32 gw = (ix1 > ix0) ? (u32)(ix1 - ix0) : 0u;
        u32 gh = (iy1 > iy0) ? (u32)(iy1 - iy0) : 0u;

        if (gw > 0u && gh > 0u) {
            if (shelf_x + gw + pad > atlas_w) {
                shelf_y += shelf_h + pad;
                shelf_x  = pad;
                shelf_h  = 0u;
            }
            if (shelf_x + gw + pad > atlas_w || shelf_y + gh + pad > atlas_h) {
                arena_reset(scratch, mark);
                return FALSE;
            }

            u8* dst = out->px + (usize)shelf_y * (usize)atlas_w + (usize)shelf_x;
            if (!font_rasterize_glyph(&tg, scale, scale, 0.0f, 0.0f, dst, gw, gh, atlas_w, scratch)) {
                arena_reset(scratch, mark);
                return FALSE;
            }

            fg->x  = (u16)shelf_x;
            fg->y  = (u16)shelf_y;
            fg->w  = (u16)gw;
            fg->h  = (u16)gh;
            fg->u0 = (f32)shelf_x * inv_w;
            fg->v0 = (f32)shelf_y * inv_h;
            fg->u1 = (f32)(shelf_x + gw) * inv_w;
            fg->v1 = (f32)(shelf_y + gh) * inv_h;

            shelf_x += gw + pad;
            if (gh > shelf_h) shelf_h = gh;
        }

        if (cp < FONT_ATLAS_FAST_CP_CAP) {
            out->cp_map[cp] = (u16)(idx + 1u);
        }

        arena_reset(scratch, mark);
    }

    return TRUE;
}

const FontGlyph* font_atlas_glyph(const FontAtlas* atlas, u32 codepoint) {
    if (!atlas || !atlas->glyphs || atlas->glyph_count == 0u) return NULL;
    if (codepoint < FONT_ATLAS_FAST_CP_CAP) {
        u16 mapped = atlas->cp_map[codepoint];
        if (mapped > 0u) return &atlas->glyphs[mapped - 1u];
    }
    for (u32 i = 0u; i < atlas->glyph_count; i++) {
        if (atlas->glyphs[i].codepoint == codepoint) return &atlas->glyphs[i];
    }
    return &atlas->glyphs[0];
}

f32 font_measure(const FontAtlas* atlas, const char* utf8, f32* out_w, f32* out_h) {
    if (!atlas || !utf8 || !*utf8) {
        if (out_w) *out_w = 0.0f;
        if (out_h) *out_h = 0.0f;
        return 0.0f;
    }

    f32 max_w  = 0.0f;
    f32 cur_w  = 0.0f;
    u32 lines  = 1u;
    b32 has_prev = FALSE;
    u16 prev_gid = 0u;

    const char* p = utf8;
    while (*p) {
        u32 cp = font_atlas_utf8_next(&p);
        if (cp == 0u) break;
        if (cp == '\n') {
            if (cur_w > max_w) max_w = cur_w;
            cur_w    = 0.0f;
            has_prev = FALSE;
            lines++;
            continue;
        }
        if (cp == '\r') continue;

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
            }
        }

        if (has_prev && atlas->font) {
            i16 kern_fu = ttf_kern_advance(atlas->font, prev_gid, gid);
            cur_w += (f32)kern_fu * atlas->scale;
        }
        cur_w   += adv;
        prev_gid = gid;
        has_prev = TRUE;
    }

    if (cur_w > max_w) max_w = cur_w;
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (f32)lines * atlas->line_height;
    return max_w;
}
