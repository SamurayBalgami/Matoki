/* ui/ui.h — Immediate-mode hata ayıklama arayüzü (Debug UI).
 * Panel, etiket, buton, onay kutusu, kaydırıcı (f32/i32), 120 karesel açılır çizgi grafiği,
 * profiler bölge tablosu (core/profiler.h) ve kırpma dikdörtgeni (clip rect) yığını.
 * Doğrudan Framebuffer üzerinde hızlı CPU blit ile çalışır; girdi kaynağı UiInput'tir.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/ui/ui.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef UI_UI_H
#define UI_UI_H

#include "core/types.h"
#include "raster/raster.h"

/* Arayüz girdisi (onların plat/input.h InputState'i; bizde demo kendi InputState'inden doldurur) */
#define UI_MAX_CHARS 32
typedef struct UiInput {
    u8  keys[256];            /* KeyboardEvent.keyCode -> 0/1 */
    i32 mouse_x, mouse_y;     /* framebuffer koordinatında */
    i32 mouse_dx, mouse_dy;
    u32 mouse_buttons;        /* bit0 sol, bit1 sağ, bit2 orta */
    i32 wheel;
    u32 chars[UI_MAX_CHARS];  /* kare başına yazılan UTF-32 kod noktaları */
    u32 char_count;
} UiInput;

#define UI_CLIP_STACK_MAX  16
#define UI_GRAPH_HISTORY   120

typedef struct {
    i32 x0, y0, x1, y1;
} UiRect;

typedef struct {
    Framebuffer* fb;
    UiInput   input;
    u32          prev_mouse_buttons;
    b32          mouse_pressed;
    b32          mouse_released;
    b32          mouse_down;

    u32          hot_id;
    u32          active_id;

    /* Aktif panel yerleşim (layout) durumu */
    i32          panel_x, panel_y, panel_w;
    i32          cursor_x, cursor_y;
    i32          padding;
    i32          spacing;

    /* Kırpma dikdörtgeni yığını */
    UiRect       clip_stack[UI_CLIP_STACK_MAX];
    i32          clip_depth;

    /* Son 120 frame'lik halka tampon (frame süresi grafiği için) */
    f32          frame_times[UI_GRAPH_HISTORY];
    u32          frame_head;
    u32          frame_count;
    b32          graph_open;
} UiContext;

/* Bağlam ilklendirme ve kare başı/sonu */
void   ui_init(UiContext* ui, Framebuffer* fb);
void   ui_begin(UiContext* ui, const UiInput* input);
void   ui_end(UiContext* ui);

/* Kırpma (clipping) yığını */
void   ui_push_clip(UiContext* ui, i32 x, i32 y, i32 w, i32 h);
void   ui_pop_clip(UiContext* ui);
UiRect ui_current_clip(const UiContext* ui);

/* Kırpma duyarlı temel çizim yardımcıları */
void   ui_draw_rect(UiContext* ui, i32 x, i32 y, i32 w, i32 h, u32 rgba);
void   ui_draw_outline(UiContext* ui, i32 x, i32 y, i32 w, i32 h, u32 rgba);
void   ui_draw_line(UiContext* ui, i32 x0, i32 y0, i32 x1, i32 y1, u32 rgba);
void   ui_draw_text(UiContext* ui, i32 x, i32 y, const char* text, u32 rgba, i32 scale);

/* Immediate-mode bileşenler */
void   ui_panel_begin(UiContext* ui, const char* title, i32 x, i32 y, i32 w, i32 h);
void   ui_panel_end(UiContext* ui);

void   ui_label(UiContext* ui, const char* text);
void   ui_separator(UiContext* ui);
b32    ui_button(UiContext* ui, const char* label);
b32    ui_checkbox(UiContext* ui, const char* label, b32* value);
b32    ui_slider_f32(UiContext* ui, const char* label, f32* value, f32 min_val, f32 max_val);
b32    ui_slider_i32(UiContext* ui, const char* label, i32* value, i32 min_val, i32 max_val);

/* 120 karelik frame süresi kaydı ve açılır çizgi grafiği */
void   ui_record_frame_time(UiContext* ui, f32 frame_ms);
void   ui_frame_graph(UiContext* ui, const char* label, f32 scale_max_ms);

/* core/profiler.h bölgelerini okuyan hizalı tablo */
void   ui_profiler_table(UiContext* ui);

#endif
