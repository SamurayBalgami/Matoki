/* ui/ui.c — Immediate-mode hata ayıklama arayüzü (Debug UI) implementasyonu.
 * Kırpma dikdörtgeni yığını, yarı saydam alfa harmanlama, Bresenham çizgi çizimi,
 * etkileşimli widget'lar (buton, onay kutusu, kaydırıcı), 120 karelik çizgi grafiği ve
 * core/profiler.h bölgelerini gösteren canlı tablo.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/ui/ui.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "ui/ui.h"
#include "ui/bitfont.h"
#include "core/math.h"
#include "core/fmt.h"
#include "core/profiler.h"

/* UI Tema Renkleri (RGBA8) */
#define UI_COL_PANEL_BG     color_pack(18,  22,  30,  225)
#define UI_COL_PANEL_HEADER color_pack(32,  42,  58,  245)
#define UI_COL_BORDER       color_pack(70,  86,  112, 255)
#define UI_COL_TEXT         color_pack(232, 238, 248, 255)
#define UI_COL_TEXT_DIM     color_pack(150, 165, 188, 255)
#define UI_COL_BTN          color_pack(44,  56,  78,  255)
#define UI_COL_BTN_HOVER    color_pack(64,  84,  116, 255)
#define UI_COL_BTN_ACTIVE   color_pack(92,  132, 188, 255)
#define UI_COL_ACCENT       color_pack(72,  196, 142, 255)
#define UI_COL_WARN         color_pack(240, 176, 64,  255)
#define UI_COL_GRAPH_BG     color_pack(12,  15,  22,  240)

static u32 ui_hash_str(const char* s, u32 salt) {
    u32 h = 2166136261u ^ salt;
    if (!s) return h;
    while (*s) {
        h ^= (u8)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static inline void ui_blend_px(u32* dst, u32 rgba) {
    u32 sa = (rgba >> 24) & 0xFFu;
    if (sa == 255u) {
        *dst = rgba;
    } else if (sa > 0u) {
        u32 d = *dst;
        u32 inv_a = 255u - sa;
        u32 r = (((rgba >>  0) & 0xFFu) * sa + ((d >>  0) & 0xFFu) * inv_a + 127u) / 255u;
        u32 g = (((rgba >>  8) & 0xFFu) * sa + ((d >>  8) & 0xFFu) * inv_a + 127u) / 255u;
        u32 b = (((rgba >> 16) & 0xFFu) * sa + ((d >> 16) & 0xFFu) * inv_a + 127u) / 255u;
        *dst = color_pack(r, g, b, 255u);
    }
}

void ui_init(UiContext* ui, Framebuffer* fb) {
    if (!ui) return;
    u8* p = (u8*)ui;
    for (usize i = 0; i < sizeof(UiContext); i++) p[i] = 0;
    ui->fb         = fb;
    ui->padding    = 6;
    ui->spacing    = 4;
    ui->graph_open = TRUE;
}

UiRect ui_current_clip(const UiContext* ui) {
    if (ui && ui->clip_depth > 0) {
        return ui->clip_stack[ui->clip_depth - 1];
    }
    UiRect r = { 0, 0, ui && ui->fb ? (i32)((u32)ui->fb->w) : 0, ui && ui->fb ? (i32)((u32)ui->fb->h) : 0 };
    return r;
}

void ui_push_clip(UiContext* ui, i32 x, i32 y, i32 w, i32 h) {
    if (!ui || ui->clip_depth >= UI_CLIP_STACK_MAX) return;
    UiRect prev = ui_current_clip(ui);
    UiRect next;
    next.x0 = MAX(prev.x0, x);
    next.y0 = MAX(prev.y0, y);
    next.x1 = MIN(prev.x1, x + w);
    next.y1 = MIN(prev.y1, y + h);
    if (next.x1 < next.x0) next.x1 = next.x0;
    if (next.y1 < next.y0) next.y1 = next.y0;
    ui->clip_stack[ui->clip_depth++] = next;
}

void ui_pop_clip(UiContext* ui) {
    if (!ui || ui->clip_depth <= 0) return;
    ui->clip_depth--;
}

void ui_begin(UiContext* ui, const UiInput* input) {
    if (!ui) return;
    if (input) {
        ui->input = *input;
    }
    u32 cur_btn = ui->input.mouse_buttons & 1u;
    u32 prv_btn = ui->prev_mouse_buttons & 1u;
    ui->mouse_down     = (cur_btn != 0);
    ui->mouse_pressed  = (cur_btn != 0 && prv_btn == 0);
    ui->mouse_released = (cur_btn == 0 && prv_btn != 0);
    ui->prev_mouse_buttons = ui->input.mouse_buttons;

    ui->hot_id = 0;
    ui->clip_depth = 0;
    if (ui->fb) {
        ui_push_clip(ui, 0, 0, (i32)((u32)ui->fb->w), (i32)((u32)ui->fb->h));
    }
}

void ui_end(UiContext* ui) {
    if (!ui) return;
    if (!ui->mouse_down) {
        ui->active_id = 0;
    }
    ui->clip_depth = 0;
}

void ui_draw_rect(UiContext* ui, i32 x, i32 y, i32 w, i32 h, u32 rgba) {
    if (!ui || !ui->fb || !ui->fb->color || w <= 0 || h <= 0) return;
    UiRect clip = ui_current_clip(ui);
    i32 x0 = MAX(x, clip.x0);
    i32 y0 = MAX(y, clip.y0);
    i32 x1 = MIN(x + w, clip.x1);
    i32 y1 = MIN(y + h, clip.y1);
    if (x0 >= x1 || y0 >= y1) return;

    u32 stride = ((u32)ui->fb->w);
    for (i32 py = y0; py < y1; py++) {
        u32* row = ui->fb->color + (usize)py * stride;
        for (i32 px = x0; px < x1; px++) {
            ui_blend_px(&row[px], rgba);
        }
    }
}

void ui_draw_outline(UiContext* ui, i32 x, i32 y, i32 w, i32 h, u32 rgba) {
    if (w <= 0 || h <= 0) return;
    ui_draw_rect(ui, x,         y,         w, 1, rgba);
    ui_draw_rect(ui, x,         y + h - 1, w, 1, rgba);
    ui_draw_rect(ui, x,         y + 1,     1, h - 2, rgba);
    ui_draw_rect(ui, x + w - 1, y + 1,     1, h - 2, rgba);
}

void ui_draw_line(UiContext* ui, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgba) {
    if (!ui || !ui->fb || !ui->fb->color) return;
    UiRect clip = ui_current_clip(ui);

    i32 dx = x1 > x0 ? (x1 - x0) : (x0 - x1);
    i32 sx = x0 < x1 ? 1 : -1;
    i32 dy = y1 > y0 ? -(y1 - y0) : -(y0 - y1);
    i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;

    for (;;) {
        if (x0 >= clip.x0 && x0 < clip.x1 && y0 >= clip.y0 && y0 < clip.y1) {
            ui_blend_px(&ui->fb->color[(usize)y0 * ((u32)ui->fb->w) + (usize)x0], rgba);
        }
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ui_draw_text(UiContext* ui, i32 x, i32 y, const char* text, u32 rgba, i32 scale) {
    if (!ui || !ui->fb) return;
    UiRect clip = ui_current_clip(ui);
    bitfont_draw_clipped(ui->fb, x, y, text, rgba, scale, clip.x0, clip.y0, clip.x1, clip.y1);
}

static b32 ui_mouse_in_rect(const UiContext* ui, i32 x, i32 y, i32 w, i32 h) {
    UiRect clip = ui_current_clip(ui);
    i32 mx = ui->input.mouse_x;
    i32 my = ui->input.mouse_y;
    if (mx < clip.x0 || mx >= clip.x1 || my < clip.y0 || my >= clip.y1) return FALSE;
    return (mx >= x && mx < x + w && my >= y && my < y + h);
}

void ui_panel_begin(UiContext* ui, const char* title, i32 x, i32 y, i32 w, i32 h) {
    if (!ui) return;
    ui_push_clip(ui, x, y, w, h);
    ui_draw_rect(ui, x, y, w, h, UI_COL_PANEL_BG);

    i32 header_h = 18;
    ui_draw_rect(ui, x, y, w, header_h, UI_COL_PANEL_HEADER);
    ui_draw_outline(ui, x, y, w, h, UI_COL_BORDER);
    ui_draw_rect(ui, x, y + header_h, w, 1, UI_COL_BORDER);

    if (title) {
        ui_draw_text(ui, x + ui->padding, y + 5, title, UI_COL_ACCENT, 1);
    }

    ui->panel_x  = x;
    ui->panel_y  = y;
    ui->panel_w  = w;
    ui->cursor_x = x + ui->padding;
    ui->cursor_y = y + header_h + ui->padding;
}

void ui_panel_end(UiContext* ui) {
    ui_pop_clip(ui);
}

void ui_label(UiContext* ui, const char* text) {
    if (!ui || !text) return;
    ui_draw_text(ui, ui->cursor_x, ui->cursor_y + 2, text, UI_COL_TEXT, 1);
    ui->cursor_y += 12 + ui->spacing;
}

void ui_separator(UiContext* ui) {
    if (!ui) return;
    i32 w = ui->panel_w - ui->padding * 2;
    ui_draw_rect(ui, ui->cursor_x, ui->cursor_y + 2, w, 1, UI_COL_BORDER);
    ui->cursor_y += 5 + ui->spacing;
}

b32 ui_button(UiContext* ui, const char* label) {
    if (!ui || !label) return FALSE;
    u32 id = ui_hash_str(label, (u32)ui->cursor_y);
    i32 x = ui->cursor_x;
    i32 y = ui->cursor_y;
    i32 w = ui->panel_w - ui->padding * 2;
    i32 h = 16;

    b32 hovered = ui_mouse_in_rect(ui, x, y, w, h);
    if (hovered) {
        ui->hot_id = id;
        if (ui->active_id == 0 && ui->mouse_pressed) {
            ui->active_id = id;
        }
    }

    u32 bg = UI_COL_BTN;
    if (ui->active_id == id && ui->mouse_down) {
        bg = UI_COL_BTN_ACTIVE;
    } else if (ui->hot_id == id) {
        bg = UI_COL_BTN_HOVER;
    }

    ui_draw_rect(ui, x, y, w, h, bg);
    ui_draw_outline(ui, x, y, w, h, UI_COL_BORDER);

    i32 tw = bitfont_measure_width(label, 1);
    i32 tx = x + MAX((w - tw) / 2, 4);
    ui_draw_text(ui, tx, y + 4, label, UI_COL_TEXT, 1);

    ui->cursor_y += h + ui->spacing;
    return (hovered && ui->active_id == id && ui->mouse_released);
}

b32 ui_checkbox(UiContext* ui, const char* label, b32* value) {
    if (!ui || !label || !value) return FALSE;
    u32 id = ui_hash_str(label, (u32)ui->cursor_y ^ 0x1111u);
    i32 x = ui->cursor_x;
    i32 y = ui->cursor_y;
    i32 box_sz = 12;
    i32 w = ui->panel_w - ui->padding * 2;
    i32 h = 14;

    b32 hovered = ui_mouse_in_rect(ui, x, y, w, h);
    if (hovered) {
        ui->hot_id = id;
        if (ui->active_id == 0 && ui->mouse_pressed) {
            ui->active_id = id;
        }
    }

    b32 changed = FALSE;
    if (hovered && ui->active_id == id && ui->mouse_released) {
        *value = !(*value);
        changed = TRUE;
    }

    u32 bg = hovered ? UI_COL_BTN_HOVER : UI_COL_BTN;
    ui_draw_rect(ui, x, y + 1, box_sz, box_sz, bg);
    ui_draw_outline(ui, x, y + 1, box_sz, box_sz, UI_COL_BORDER);
    if (*value) {
        ui_draw_rect(ui, x + 3, y + 4, box_sz - 6, box_sz - 6, UI_COL_ACCENT);
    }

    ui_draw_text(ui, x + box_sz + 6, y + 3, label, UI_COL_TEXT, 1);
    ui->cursor_y += h + ui->spacing;
    return changed;
}

b32 ui_slider_f32(UiContext* ui, const char* label, f32* value, f32 min_val, f32 max_val) {
    if (!ui || !label || !value || max_val <= min_val) return FALSE;
    u32 id = ui_hash_str(label, (u32)ui->cursor_y ^ 0x2222u);
    i32 x = ui->cursor_x;
    i32 y = ui->cursor_y;
    i32 w = ui->panel_w - ui->padding * 2;
    i32 bar_y = y + 11;
    i32 bar_h = 10;

    b32 hovered = ui_mouse_in_rect(ui, x, bar_y, w, bar_h);
    if (hovered) {
        ui->hot_id = id;
        if (ui->active_id == 0 && ui->mouse_pressed) {
            ui->active_id = id;
        }
    }

    b32 changed = FALSE;
    if (ui->active_id == id && ui->mouse_down) {
        f32 t = (f32)(ui->input.mouse_x - x) / (f32)MAX(w, 1);
        t = m_clampf(t, 0.0f, 1.0f);
        f32 nv = min_val + t * (max_val - min_val);
        if (m_absf(nv - *value) > 1e-6f) {
            *value = nv;
            changed = TRUE;
        }
    }

    char buf[96];
    fmt_format(buf, sizeof(buf), "%s: %.2f", label, (f64)*value);
    ui_draw_text(ui, x, y, buf, UI_COL_TEXT, 1);

    ui_draw_rect(ui, x, bar_y, w, bar_h, UI_COL_GRAPH_BG);
    f32 norm = m_clampf((*value - min_val) / (max_val - min_val), 0.0f, 1.0f);
    i32 fill_w = (i32)((f32)(w - 2) * norm + 0.5f);
    if (fill_w > 0) {
        ui_draw_rect(ui, x + 1, bar_y + 1, fill_w, bar_h - 2, UI_COL_ACCENT);
    }
    ui_draw_outline(ui, x, bar_y, w, bar_h, UI_COL_BORDER);

    ui->cursor_y += 22 + ui->spacing;
    return changed;
}

b32 ui_slider_i32(UiContext* ui, const char* label, i32* value, i32 min_val, i32 max_val) {
    if (!ui || !label || !value || max_val <= min_val) return FALSE;
    f32 fv = (f32)(*value);
    u32 id = ui_hash_str(label, (u32)ui->cursor_y ^ 0x3333u);
    i32 x = ui->cursor_x;
    i32 y = ui->cursor_y;
    i32 w = ui->panel_w - ui->padding * 2;
    i32 bar_y = y + 11;
    i32 bar_h = 10;

    b32 hovered = ui_mouse_in_rect(ui, x, bar_y, w, bar_h);
    if (hovered) {
        ui->hot_id = id;
        if (ui->active_id == 0 && ui->mouse_pressed) {
            ui->active_id = id;
        }
    }

    b32 changed = FALSE;
    if (ui->active_id == id && ui->mouse_down) {
        f32 t = (f32)(ui->input.mouse_x - x) / (f32)MAX(w, 1);
        t = m_clampf(t, 0.0f, 1.0f);
        i32 nv = min_val + (i32)(t * (f32)(max_val - min_val) + 0.5f);
        nv = CLAMP(nv, min_val, max_val);
        if (nv != *value) {
            *value = nv;
            fv = (f32)nv;
            changed = TRUE;
        }
    }

    char buf[96];
    fmt_format(buf, sizeof(buf), "%s: %d", label, *value);
    ui_draw_text(ui, x, y, buf, UI_COL_TEXT, 1);

    ui_draw_rect(ui, x, bar_y, w, bar_h, UI_COL_GRAPH_BG);
    f32 norm = m_clampf((fv - (f32)min_val) / (f32)(max_val - min_val), 0.0f, 1.0f);
    i32 fill_w = (i32)((f32)(w - 2) * norm + 0.5f);
    if (fill_w > 0) {
        ui_draw_rect(ui, x + 1, bar_y + 1, fill_w, bar_h - 2, UI_COL_ACCENT);
    }
    ui_draw_outline(ui, x, bar_y, w, bar_h, UI_COL_BORDER);

    ui->cursor_y += 22 + ui->spacing;
    return changed;
}

void ui_record_frame_time(UiContext* ui, f32 frame_ms) {
    if (!ui) return;
    ui->frame_times[ui->frame_head] = frame_ms;
    ui->frame_head = (ui->frame_head + 1u) % UI_GRAPH_HISTORY;
    if (ui->frame_count < UI_GRAPH_HISTORY) {
        ui->frame_count++;
    }
}

void ui_frame_graph(UiContext* ui, const char* label, f32 scale_max_ms) {
    if (!ui) return;
    if (scale_max_ms <= 1.0f) scale_max_ms = 33.33f;

    /* Açılır başlık (collapsible header) */
    u32 id = ui_hash_str(label ? label : "Frame", 0x4444u);
    i32 x = ui->cursor_x;
    i32 y = ui->cursor_y;
    i32 w = ui->panel_w - ui->padding * 2;
    i32 hdr_h = 14;

    b32 hovered = ui_mouse_in_rect(ui, x, y, w, hdr_h);
    if (hovered) {
        ui->hot_id = id;
        if (ui->active_id == 0 && ui->mouse_pressed) {
            ui->active_id = id;
        }
    }
    if (hovered && ui->active_id == id && ui->mouse_released) {
        ui->graph_open = !ui->graph_open;
    }

    f32 last_ms = 0.0f, min_ms = 9999.0f, max_ms = 0.0f, sum_ms = 0.0f;
    for (u32 i = 0; i < ui->frame_count; i++) {
        u32 idx = (ui->frame_head + UI_GRAPH_HISTORY - ui->frame_count + i) % UI_GRAPH_HISTORY;
        f32 v = ui->frame_times[idx];
        last_ms = v;
        if (v < min_ms) min_ms = v;
        if (v > max_ms) max_ms = v;
        sum_ms += v;
    }
    f32 avg_ms = ui->frame_count > 0 ? (sum_ms / (f32)ui->frame_count) : 0.0f;
    if (ui->frame_count == 0) min_ms = 0.0f;

    char hdr[96];
    fmt_format(hdr, sizeof(hdr), "%s %s: %.1fms (ort %.1f)",
               ui->graph_open ? "[-]" : "[+]",
               label ? label : "Kare",
               (f64)last_ms, (f64)avg_ms);
    ui_draw_rect(ui, x, y, w, hdr_h, hovered ? UI_COL_BTN_HOVER : UI_COL_BTN);
    ui_draw_outline(ui, x, y, w, hdr_h, UI_COL_BORDER);
    ui_draw_text(ui, x + 4, y + 3, hdr, UI_COL_TEXT, 1);
    ui->cursor_y += hdr_h + 2;

    if (!ui->graph_open) {
        ui->cursor_y += ui->spacing;
        return;
    }

    i32 gx = x;
    i32 gy = ui->cursor_y;
    i32 gw = w;
    i32 gh = 48;

    ui_draw_rect(ui, gx, gy, gw, gh, UI_COL_GRAPH_BG);

    /* 16.67 ms (60 FPS) hedef referans çizgisi */
    f32 ref_norm = m_clampf(16.666f / scale_max_ms, 0.0f, 1.0f);
    i32 ref_y = gy + gh - 2 - (i32)(ref_norm * (f32)(gh - 4));
    ui_draw_rect(ui, gx + 1, ref_y, gw - 2, 1, color_pack(80, 95, 60, 180));

    if (ui->frame_count >= 2) {
        i32 prev_px = 0, prev_py = 0;
        for (u32 i = 0; i < ui->frame_count; i++) {
            u32 idx = (ui->frame_head + UI_GRAPH_HISTORY - ui->frame_count + i) % UI_GRAPH_HISTORY;
            f32 v = ui->frame_times[idx];
            f32 t_x = (f32)i / (f32)(UI_GRAPH_HISTORY - 1);
            f32 t_y = m_clampf(v / scale_max_ms, 0.0f, 1.0f);

            i32 px = gx + 2 + (i32)(t_x * (f32)(gw - 4));
            i32 py = gy + gh - 2 - (i32)(t_y * (f32)(gh - 4));
            if (i > 0) {
                u32 col = (v > 16.7f) ? UI_COL_WARN : UI_COL_ACCENT;
                ui_draw_line(ui, prev_px, prev_py, px, py, col);
            }
            prev_px = px;
            prev_py = py;
        }
    }

    ui_draw_outline(ui, gx, gy, gw, gh, UI_COL_BORDER);
    ui->cursor_y += gh + ui->spacing;
}

void ui_profiler_table(UiContext* ui) {
    if (!ui) return;
    u32 count = prof_zone_count();
    i32 x = ui->cursor_x;
    i32 w = ui->panel_w - ui->padding * 2;

    ui_draw_text(ui, x, ui->cursor_y, "BOLGE            SON(ms) MAX(ms) #", UI_COL_TEXT_DIM, 1);
    ui->cursor_y += 10;
    ui_draw_rect(ui, x, ui->cursor_y, w, 1, UI_COL_BORDER);
    ui->cursor_y += 3;

    if (count == 0) {
        ui_draw_text(ui, x, ui->cursor_y, "(aktif bolge yok)", UI_COL_TEXT_DIM, 1);
        ui->cursor_y += 10 + ui->spacing;
        return;
    }

    for (u32 i = 0; i < count; i++) {
        const ProfZone* z = prof_zone_get(i);
        if (!z) continue;
        f64 ms = z->last_ms > 0.0 ? z->last_ms : z->cur_ms;
        u32 calls = z->last_calls > 0 ? z->last_calls : z->cur_calls;

        char row[96];
        fmt_format(row, sizeof(row), "%-15s %6.2f  %6.2f %3u",
                   z->name ? z->name : "?", ms, z->max_ms, calls);
        ui_draw_text(ui, x, ui->cursor_y, row, UI_COL_TEXT, 1);
        ui->cursor_y += 10;
    }
    ui->cursor_y += ui->spacing;
}
