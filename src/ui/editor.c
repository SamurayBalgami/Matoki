/* ui/editor.c — Motor içi canlı Matoki shader metin editörü implementasyonu.
 * src/matoki/lexer.h ile sözdizimi renklendirme, UTF-8/ASCII imleç yönetimi,
 * seçim, kaydırma, satır numaraları, sabit boyutlu geri al/yinele halkası,
 * hata satırı kırmızı dalgalı alt çizgisi ve MtkDiag hata mesajı paneli.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/ui/editor.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "ui/editor.h"
#include "ui/bitfont.h"
#include "matoki/lexer.h"
#include "core/mem.h"
#include "core/math.h"
#include "core/fmt.h"

/* Sözdizimi renklendirme paleti (RGBA8) */
#define ED_COL_BG          color_pack(14,  17,  24,  255)
#define ED_COL_GUTTER_BG   color_pack(20,  25,  35,  255)
#define ED_COL_HEADER_BG   color_pack(26,  34,  48,  255)
#define ED_COL_DIAG_BG     color_pack(24,  16,  20,  250)
#define ED_COL_DIAG_OK_BG  color_pack(16,  26,  22,  250)
#define ED_COL_BORDER      color_pack(64,  80,  108, 255)
#define ED_COL_CUR_LINE    color_pack(26,  34,  50,  255)
#define ED_COL_SELECTION   color_pack(52,  92,  156, 200)
#define ED_COL_CURSOR      color_pack(245, 220, 100, 255)
#define ED_COL_LINE_NUM    color_pack(105, 120, 145, 255)
#define ED_COL_LINE_ACT    color_pack(220, 230, 248, 255)

#define ED_COL_DEFAULT     color_pack(225, 232, 244, 255)
#define ED_COL_KEYWORD     color_pack(255, 122, 148, 255)
#define ED_COL_TYPE        color_pack(92,  214, 255, 255)
#define ED_COL_BUILTIN     color_pack(245, 206, 98,  255)
#define ED_COL_NUMBER      color_pack(186, 148, 255, 255)
#define ED_COL_COMMENT     color_pack(108, 128, 150, 255)
#define ED_COL_OP          color_pack(155, 195, 230, 255)
#define ED_COL_ERROR       color_pack(255, 72,  72,  255)
#define ED_COL_OK          color_pack(82,  214, 142, 255)

/* Tuş kodları */
#define KEY_BACKSPACE 8
#define KEY_TAB       9
#define KEY_ENTER     13
#define KEY_SHIFT     16
#define KEY_CTRL      17
#define KEY_END       35
#define KEY_HOME      36
#define KEY_LEFT      37
#define KEY_UP        38
#define KEY_RIGHT     39
#define KEY_DOWN      40
#define KEY_DELETE    46
#define KEY_A         65
#define KEY_C         67
#define KEY_V         86
#define KEY_X         88
#define KEY_Y         89
#define KEY_Z         90

#define ED_LINE_H     10
#define ED_CHAR_W     8
#define ED_GUTTER_W   28
#define ED_HEADER_H   18

static b32 ed_str_eq_len(const char* a, u32 alen, const char* b) {
    u32 i = 0;
    while (i < alen && b[i] != '\0') {
        if (a[i] != b[i]) return FALSE;
        i++;
    }
    return i == alen && b[i] == '\0';
}

static b32 ed_is_type_ident(const char* s, u32 len) {
    static const char* const k_types[] = {
        "f32", "i32", "u32", "bool", "rgba", "texture2d",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "mat3", "mat4"
    };
    for (u32 i = 0; i < ARRAY_COUNT(k_types); i++) {
        if (ed_str_eq_len(s, len, k_types[i])) return TRUE;
    }
    return FALSE;
}

static b32 ed_is_builtin_ident(const char* s, u32 len) {
    static const char* const k_builtins[] = {
        "abs", "min", "max", "clamp", "mix", "step", "smoothstep",
        "floor", "fract", "sqrt", "rsqrt", "dot", "cross", "length",
        "normalize", "sin", "cos", "pow", "exp2", "log2", "sample", "frag",
        "sample_level", "dFdx", "dFdy", "fwidth", "tex_size"
    };
    for (u32 i = 0; i < ARRAY_COUNT(k_builtins); i++) {
        if (ed_str_eq_len(s, len, k_builtins[i])) return TRUE;
    }
    return FALSE;
}

static u32 ed_token_color(const Token* tok) {
    switch (tok->kind) {
    case T_FN:
    case T_LET:
    case T_VAR:
    case T_IF:
    case T_ELSE:
    case T_FOR:
    case T_IN:
    case T_RETURN:
    case T_DISCARD:
    case T_UNIFORM:
    case T_VARYING:
    case T_ATTRIBUTE:
        return ED_COL_KEYWORD;
    case T_TRUE:
    case T_FALSE:
    case T_INT:
    case T_FLOAT:
        return ED_COL_NUMBER;
    case T_IDENT:
        if (ed_is_type_ident(tok->p, tok->len)) return ED_COL_TYPE;
        if (ed_is_builtin_ident(tok->p, tok->len)) return ED_COL_BUILTIN;
        return ED_COL_DEFAULT;
    case T_AT:
        return ED_COL_BUILTIN;
    case T_ERROR:
        return ED_COL_ERROR;
    default:
        return ED_COL_OP;
    }
}

/* Kaynak metnin her baytı için sözdizimi rengi dizisini doldurur (src/matoki/lexer.h ile). */
static void ed_colorize(Arena* lex, const char* src, u32 len, u32* col_out) {
    for (u32 i = 0; i < len; i++) {
        col_out[i] = ED_COL_COMMENT; /* Lexer'ın atladığı yorumlar varsayılan olarak yorum rengi alır */
    }
    for (u32 i = 0; i < len; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            col_out[i] = ED_COL_DEFAULT;
        }
    }

    /* bizim lexer kaynağın tamamını tek seferde token dizisine çevirir; hata token'ında durur */
    if (!lex->base) return;
    usize mark = arena_mark(lex);
    TokenList tl;
    lex_source(lex, src, len, &tl);
    for (u32 k = 0; k < tl.count; k++) {
        const Token* tok = &tl.toks[k];
        if (tok->kind == T_EOF) break;
        u32 start = (u32)(tok->p - src);
        u32 end   = start + tok->len;
        if (start > len) continue;
        if (end > len) end = len;
        u32 c = ed_token_color(tok);
        for (u32 i = start; i < end; i++) {
            col_out[i] = c;
        }
    }
    arena_reset(lex, mark);
}

/* ---------- Undo / Redo Halka Tamponu ---------- */

static void ed_save_snapshot(CodeEditor* ed) {
    if (ed->undo_count > 0) {
        u32 cur_idx = (ed->undo_head + ed->undo_pos) % EDITOR_UNDO_CAP;
        const EditorSnapshot* cur = &ed->undo_ring[cur_idx];
        if (cur->len == ed->len && mem_equal(cur->buf, ed->buf, ed->len)) {
            return;
        }
        /* Aktif konumdan sonraki redo geçmişini buda */
        ed->undo_count = ed->undo_pos + 1;
    }

    if (ed->undo_count < EDITOR_UNDO_CAP) {
        u32 dst_idx = (ed->undo_head + ed->undo_count) % EDITOR_UNDO_CAP;
        EditorSnapshot* snap = &ed->undo_ring[dst_idx];
        __builtin_memcpy(snap->buf, ed->buf, ed->len);
        snap->buf[ed->len] = '\0';
        snap->len    = ed->len;
        snap->cursor = ed->cursor;
        ed->undo_pos = ed->undo_count;
        ed->undo_count++;
    } else {
        EditorSnapshot* snap = &ed->undo_ring[ed->undo_head];
        __builtin_memcpy(snap->buf, ed->buf, ed->len);
        snap->buf[ed->len] = '\0';
        snap->len    = ed->len;
        snap->cursor = ed->cursor;
        ed->undo_head = (ed->undo_head + 1u) % EDITOR_UNDO_CAP;
        ed->undo_pos  = EDITOR_UNDO_CAP - 1u;
    }
}

static void ed_undo(CodeEditor* ed) {
    if (ed->undo_count == 0 || ed->undo_pos == 0) return;
    ed->undo_pos--;
    u32 idx = (ed->undo_head + ed->undo_pos) % EDITOR_UNDO_CAP;
    const EditorSnapshot* snap = &ed->undo_ring[idx];
    __builtin_memcpy(ed->buf, snap->buf, snap->len);
    ed->buf[snap->len] = '\0';
    ed->len        = snap->len;
    ed->cursor     = MIN(snap->cursor, ed->len);
    ed->sel_anchor = ed->cursor;
}

static void ed_redo(CodeEditor* ed) {
    if (ed->undo_count == 0 || ed->undo_pos + 1 >= ed->undo_count) return;
    ed->undo_pos++;
    u32 idx = (ed->undo_head + ed->undo_pos) % EDITOR_UNDO_CAP;
    const EditorSnapshot* snap = &ed->undo_ring[idx];
    __builtin_memcpy(ed->buf, snap->buf, snap->len);
    ed->buf[snap->len] = '\0';
    ed->len        = snap->len;
    ed->cursor     = MIN(snap->cursor, ed->len);
    ed->sel_anchor = ed->cursor;
}

/* ---------- Metin ve İmleç Yardımcıları ---------- */

static b32 ed_has_selection(const CodeEditor* ed) {
    return ed->cursor != ed->sel_anchor;
}

static void ed_selection_range(const CodeEditor* ed, u32* out_start, u32* out_end) {
    if (ed->cursor < ed->sel_anchor) {
        *out_start = ed->cursor;
        *out_end   = ed->sel_anchor;
    } else {
        *out_start = ed->sel_anchor;
        *out_end   = ed->cursor;
    }
}

static void ed_delete_range(CodeEditor* ed, u32 start, u32 end) {
    if (start >= end || end > ed->len) return;
    u32 rem = ed->len - end;
    if (rem > 0) {
        __builtin_memmove(ed->buf + start, ed->buf + end, rem);
    }
    ed->len -= (end - start);
    ed->buf[ed->len] = '\0';
    ed->cursor     = start;
    ed->sel_anchor = start;
}

static void ed_delete_selection(CodeEditor* ed) {
    if (!ed_has_selection(ed)) return;
    u32 s0, s1;
    ed_selection_range(ed, &s0, &s1);
    ed_delete_range(ed, s0, s1);
}

static void ed_insert_bytes(CodeEditor* ed, const char* s, u32 n) {
    if (ed_has_selection(ed)) {
        ed_delete_selection(ed);
    }
    if (n == 0 || ed->len + n >= EDITOR_BUF_CAP) return;
    u32 tail = ed->len - ed->cursor;
    if (tail > 0) {
        __builtin_memmove(ed->buf + ed->cursor + n, ed->buf + ed->cursor, tail);
    }
    __builtin_memcpy(ed->buf + ed->cursor, s, n);
    ed->len        += n;
    ed->buf[ed->len] = '\0';
    ed->cursor     += n;
    ed->sel_anchor  = ed->cursor;
}

static u32 ed_encode_utf8(u32 cp, char out[4]) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    } else if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    } else if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static u32 ed_prev_char_pos(const CodeEditor* ed, u32 pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((u8)ed->buf[pos] & 0xC0u) == 0x80u) {
        pos--;
    }
    return pos;
}

static u32 ed_next_char_pos(const CodeEditor* ed, u32 pos) {
    if (pos >= ed->len) return ed->len;
    pos++;
    while (pos < ed->len && ((u8)ed->buf[pos] & 0xC0u) == 0x80u) {
        pos++;
    }
    return pos;
}

static u32 ed_line_start(const CodeEditor* ed, u32 pos) {
    while (pos > 0 && ed->buf[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static u32 ed_line_end(const CodeEditor* ed, u32 pos) {
    while (pos < ed->len && ed->buf[pos] != '\n') {
        pos++;
    }
    return pos;
}

static void ed_pos_to_line_col(const CodeEditor* ed, u32 pos, u32* out_line, u32* out_col) {
    u32 line = 1;
    u32 col  = 1;
    u32 i    = 0;
    if (pos > ed->len) pos = ed->len;
    while (i < pos) {
        if (ed->buf[i] == '\n') {
            line++;
            col = 1;
            i++;
        } else {
            i = ed_next_char_pos(ed, i);
            col++;
        }
    }
    *out_line = line;
    *out_col  = col;
}

static u32 ed_line_col_to_pos(const CodeEditor* ed, u32 target_line, u32 target_col) {
    u32 cur_line = 1;
    u32 pos      = 0;
    while (pos < ed->len && cur_line < target_line) {
        if (ed->buf[pos] == '\n') cur_line++;
        pos++;
    }
    u32 col = 1;
    while (pos < ed->len && ed->buf[pos] != '\n' && col < target_col) {
        pos = ed_next_char_pos(ed, pos);
        col++;
    }
    return pos;
}

static u32 ed_total_lines(const CodeEditor* ed) {
    u32 lines = 1;
    for (u32 i = 0; i < ed->len; i++) {
        if (ed->buf[i] == '\n') lines++;
    }
    return lines;
}

/* ---------- İlklendirme ve Tanılama ---------- */

void editor_init(CodeEditor* ed, Arena* a, const char* initial_src, u32 initial_len) {
    if (!ed) return;
    __builtin_memset(ed, 0, sizeof(*ed));
    ed->focused = TRUE;
    /* renklendirme belleği yoksa (a NULL / dolu) metin renksiz çizilir, editör çalışır */
    void* lex_mem = a ? arena_try(a, EDITOR_LEX_BYTES, 16) : NULL;
    if (lex_mem) arena_init(&ed->lex_arena, lex_mem, EDITOR_LEX_BYTES);
    editor_set_text(ed, initial_src, initial_len);
}

void editor_set_text(CodeEditor* ed, const char* src, u32 len) {
    if (!ed) return;
    if (len >= EDITOR_BUF_CAP) len = EDITOR_BUF_CAP - 1u;
    if (src && len > 0) {
        __builtin_memcpy(ed->buf, src, len);
    }
    ed->buf[len]   = '\0';
    ed->len        = len;
    ed->cursor     = 0;
    ed->sel_anchor = 0;
    ed->scroll_x   = 0;
    ed->scroll_y   = 0;
    ed->undo_head  = 0;
    ed->undo_count = 0;
    ed->undo_pos   = 0;
    ed_save_snapshot(ed);
}

/* Bizim derleyici tek hata döner (MatokiError: satır, sütun, mesaj); metin "satır:sütun: mesaj" biçiminde panele. */
void editor_set_diag(CodeEditor* ed, const MatokiError* err, f64 compile_ms, u32 wasm_bytes) {
    if (!ed) return;
    ed->last_compile_ms = compile_ms;
    ed->last_wasm_bytes = wasm_bytes;
    ed->error_count     = 0;
    ed->diag_len        = 0;
    ed->diag_text[0]    = '\0';
    if (!err || !err->failed) return;
    StrBuf sb;
    sb_init(&sb, ed->diag_text, sizeof(ed->diag_text));
    matoki_format_error(err, &sb);
    ed->diag_len = sb.len;
    ed->errors[0].line = err->line > 0 ? (u32)err->line : 1u;
    ed->errors[0].col  = err->col > 0 ? (u32)err->col : 1u;
    ed->error_count = 1;
}

/* ---------- Girdi ve Çizim ---------- */

static b32 ed_key_just_pressed(const CodeEditor* ed, const UiInput* inp, u32 code) {
    return inp->keys[code] && !ed->prev_keys[code];
}

static void ed_handle_nav_key(CodeEditor* ed, u32 code, b32 shift, b32 ctrl) {
    ed->blink_timer = 0.0f;

    if (code == KEY_LEFT) {
        if (!shift && ed_has_selection(ed)) {
            u32 s0, s1;
            ed_selection_range(ed, &s0, &s1);
            ed->cursor     = s0;
            ed->sel_anchor = s0;
        } else {
            ed->cursor = ed_prev_char_pos(ed, ed->cursor);
            if (!shift) ed->sel_anchor = ed->cursor;
        }
    } else if (code == KEY_RIGHT) {
        if (!shift && ed_has_selection(ed)) {
            u32 s0, s1;
            ed_selection_range(ed, &s0, &s1);
            ed->cursor     = s1;
            ed->sel_anchor = s1;
        } else {
            ed->cursor = ed_next_char_pos(ed, ed->cursor);
            if (!shift) ed->sel_anchor = ed->cursor;
        }
    } else if (code == KEY_UP) {
        u32 ln, col;
        ed_pos_to_line_col(ed, ed->cursor, &ln, &col);
        if (ln > 1) {
            ed->cursor = ed_line_col_to_pos(ed, ln - 1u, col);
        } else {
            ed->cursor = 0;
        }
        if (!shift) ed->sel_anchor = ed->cursor;
    } else if (code == KEY_DOWN) {
        u32 ln, col;
        ed_pos_to_line_col(ed, ed->cursor, &ln, &col);
        u32 total = ed_total_lines(ed);
        if (ln < total) {
            ed->cursor = ed_line_col_to_pos(ed, ln + 1u, col);
        } else {
            ed->cursor = ed->len;
        }
        if (!shift) ed->sel_anchor = ed->cursor;
    } else if (code == KEY_HOME) {
        ed->cursor = ctrl ? 0 : ed_line_start(ed, ed->cursor);
        if (!shift) ed->sel_anchor = ed->cursor;
    } else if (code == KEY_END) {
        ed->cursor = ctrl ? ed->len : ed_line_end(ed, ed->cursor);
        if (!shift) ed->sel_anchor = ed->cursor;
    } else if (code == KEY_BACKSPACE) {
        if (ed_has_selection(ed)) {
            ed_delete_selection(ed);
        } else if (ed->cursor > 0) {
            u32 prev = ed_prev_char_pos(ed, ed->cursor);
            ed_delete_range(ed, prev, ed->cursor);
        }
        ed_save_snapshot(ed);
    } else if (code == KEY_DELETE) {
        if (ed_has_selection(ed)) {
            ed_delete_selection(ed);
        } else if (ed->cursor < ed->len) {
            u32 next = ed_next_char_pos(ed, ed->cursor);
            ed_delete_range(ed, ed->cursor, next);
        }
        ed_save_snapshot(ed);
    } else if (code == KEY_ENTER && !ctrl) {
        /* Otomatik girinti (auto-indent): mevcut satırın başındaki boşlukları koru */
        u32 ls = ed_line_start(ed, ed->cursor);
        u32 indent = 0;
        while (ls + indent < ed->cursor && ed->buf[ls + indent] == ' ' && indent < 16) {
            indent++;
        }
        if (ed->cursor > 0 && ed->buf[ed->cursor - 1] == '{' && indent <= 12) {
            indent += 4;
        }
        char ins[20];
        ins[0] = '\n';
        for (u32 k = 0; k < indent; k++) ins[1 + k] = ' ';
        ed_insert_bytes(ed, ins, 1u + indent);
        ed_save_snapshot(ed);
    } else if (code == KEY_TAB) {
        ed_insert_bytes(ed, "    ", 4);
        ed_save_snapshot(ed);
    }
}

b32 editor_update_and_draw(CodeEditor* ed, UiContext* ui, const UiInput* input,
                           f32 dt, i32 x, i32 y, i32 w, i32 h) {
    if (!ed || !ui || !input || w <= 40 || h <= 40) return FALSE;

    ed->compile_requested = FALSE;
    ed->blink_timer += dt;
    if (ed->blink_timer >= 1.0f) ed->blink_timer -= 1.0f;

    i32 diag_h = (ed->error_count > 0) ? 58 : 18;
    i32 code_x = x + ED_GUTTER_W;
    i32 code_y = y + ED_HEADER_H;
    i32 code_w = w - ED_GUTTER_W;
    i32 code_h = h - ED_HEADER_H - diag_h;
    if (code_h < 20) code_h = 20;

    i32 mx = input->mouse_x;
    i32 my = input->mouse_y;
    b32 mouse_in_editor = (mx >= x && mx < x + w && my >= y && my < y + h);
    b32 mouse_in_code   = (mx >= code_x && mx < code_x + code_w && my >= code_y && my < code_y + code_h);

    /* Fare ile imleç konumlandırma ve sürükleyerek seçim */
    if (ui->mouse_pressed) {
        ed->focused = mouse_in_editor;
        if (mouse_in_code) {
            i32 rel_y = (my - code_y - 4) + ed->scroll_y;
            i32 rel_x = (mx - code_x - 4) + ed->scroll_x;
            u32 target_line = (u32)MAX(rel_y / ED_LINE_H, 0) + 1u;
            u32 target_col  = (u32)MAX((rel_x + ED_CHAR_W / 2) / ED_CHAR_W, 0) + 1u;
            ed->cursor = ed_line_col_to_pos(ed, target_line, target_col);
            if (!input->keys[KEY_SHIFT]) {
                ed->sel_anchor = ed->cursor;
            }
            ed->dragging    = TRUE;
            ed->blink_timer = 0.0f;
        }
    }
    if (ed->dragging && ui->mouse_down) {
        i32 rel_y = (my - code_y - 4) + ed->scroll_y;
        i32 rel_x = (mx - code_x - 4) + ed->scroll_x;
        u32 target_line = (u32)MAX(rel_y / ED_LINE_H, 0) + 1u;
        u32 target_col  = (u32)MAX((rel_x + ED_CHAR_W / 2) / ED_CHAR_W, 0) + 1u;
        ed->cursor = ed_line_col_to_pos(ed, target_line, target_col);
    }
    if (!ui->mouse_down) {
        ed->dragging = FALSE;
    }

    /* Fare tekerleği ile kaydırma */
    if (mouse_in_editor && input->wheel != 0) {
        ed->scroll_y += input->wheel * (ED_LINE_H * 3);
    }

    b32 ctrl  = (input->keys[KEY_CTRL] != 0);
    b32 shift = (input->keys[KEY_SHIFT] != 0);

    if (ed->focused) {
        /* Ctrl kısayolları */
        if (ctrl && ed_key_just_pressed(ed, input, KEY_ENTER)) {
            ed->compile_requested = TRUE;
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_Z)) {
            if (shift) ed_redo(ed);
            else       ed_undo(ed);
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_Y)) {
            ed_redo(ed);
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_A)) {
            ed->sel_anchor = 0;
            ed->cursor     = ed->len;
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_C)) {
            if (ed_has_selection(ed)) {
                u32 s0, s1;
                ed_selection_range(ed, &s0, &s1);
                u32 clen = MIN(s1 - s0, EDITOR_CLIP_CAP - 1u);
                __builtin_memcpy(ed->clipboard, ed->buf + s0, clen);
                ed->clipboard[clen] = '\0';
                ed->clip_len = clen;
            }
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_X)) {
            if (ed_has_selection(ed)) {
                u32 s0, s1;
                ed_selection_range(ed, &s0, &s1);
                u32 clen = MIN(s1 - s0, EDITOR_CLIP_CAP - 1u);
                __builtin_memcpy(ed->clipboard, ed->buf + s0, clen);
                ed->clipboard[clen] = '\0';
                ed->clip_len = clen;
                ed_delete_selection(ed);
                ed_save_snapshot(ed);
            }
        }
        if (ctrl && ed_key_just_pressed(ed, input, KEY_V)) {
            if (ed->clip_len > 0) {
                ed_insert_bytes(ed, ed->clipboard, ed->clip_len);
                ed_save_snapshot(ed);
            }
        }

        /* Yönlendirme ve silme tuşları + tuş tekrarı */
        static const u32 k_nav_keys[] = {
            KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END,
            KEY_BACKSPACE, KEY_DELETE, KEY_ENTER, KEY_TAB
        };
        b32 started = FALSE;
        for (u32 i = 0; i < ARRAY_COUNT(k_nav_keys); i++) {
            u32 k = k_nav_keys[i];
            if (ed_key_just_pressed(ed, input, k)) {
                if (!(ctrl && k == KEY_ENTER)) {
                    ed_handle_nav_key(ed, k, shift, ctrl);
                    ed->repeat_key   = k;
                    ed->repeat_timer = -0.35f; /* ilk tekrar gecikmesi */
                    started = TRUE;
                }
            }
        }
        /* Tekrar basış karesinde başlamaz ve kare süresi 0,1 s ile sınırlı (faz 6 düzeltmesi): yavaş karede (dt ≈ 1 s)
         * tek dokunuş aynı karede ~15 kez tekrarlanıyordu (Enter → 10+ boş satır). */
        if (ed->repeat_key != 0 && !started) {
            if (input->keys[ed->repeat_key]) {
                ed->repeat_timer += m_minf(dt, 0.1f);
                while (ed->repeat_timer >= 0.045f) {
                    ed->repeat_timer -= 0.045f;
                    ed_handle_nav_key(ed, ed->repeat_key, shift, ctrl);
                }
            } else {
                ed->repeat_key = 0;
            }
        }

        /* Metin girişi: UiInput.chars[] (UTF-32 kod noktaları). Ctrl'e bakılmaz: karakterleri platform katmanı süzer;
         * Windows'ta AltGr (Türkçe klavyede { } @ [ ]) Ctrl + Alt olarak gelir, Ctrl koşulu bu karakterleri yutardı. */
        if (input->char_count > 0) {
            u32 nchars = MIN(input->char_count, UI_MAX_CHARS);
            b32 inserted_any = FALSE;
            for (u32 i = 0; i < nchars; i++) {
                u32 cp = input->chars[i];
                if (cp >= 32u) {
                    char utf8[4];
                    u32 ulen = ed_encode_utf8(cp, utf8);
                    if (ulen > 0) {
                        ed_insert_bytes(ed, utf8, ulen);
                        inserted_any = TRUE;
                    }
                }
            }
            if (inserted_any) {
                ed->blink_timer = 0.0f;
                ed_save_snapshot(ed);
            }
        }
    }

    __builtin_memcpy(ed->prev_keys, input->keys, sizeof(ed->prev_keys));

    /* İmlecin görünür alanda kalması için otomatik kaydırma */
    u32 cur_line, cur_col;
    ed_pos_to_line_col(ed, ed->cursor, &cur_line, &cur_col);
    i32 cur_py = (i32)(cur_line - 1u) * ED_LINE_H;
    i32 cur_px = (i32)(cur_col - 1u) * ED_CHAR_W;
    i32 vis_h  = code_h - 8;
    i32 vis_w  = code_w - 12;

    if (cur_py < ed->scroll_y) {
        ed->scroll_y = cur_py;
    } else if (cur_py + ED_LINE_H > ed->scroll_y + vis_h) {
        ed->scroll_y = cur_py + ED_LINE_H - vis_h;
    }
    if (cur_px < ed->scroll_x) {
        ed->scroll_x = cur_px;
    } else if (cur_px + ED_CHAR_W > ed->scroll_x + vis_w) {
        ed->scroll_x = cur_px + ED_CHAR_W - vis_w;
    }

    u32 total_lines = ed_total_lines(ed);
    i32 max_scroll_y = MAX((i32)total_lines * ED_LINE_H - vis_h + ED_LINE_H, 0);
    ed->scroll_y = CLAMP(ed->scroll_y, 0, max_scroll_y);
    if (ed->scroll_x < 0) ed->scroll_x = 0;

    /* ---------- Çizim ---------- */
    ui_push_clip(ui, x, y, w, h);

    /* Üst başlık çubuğu + "Derle (Ctrl+Enter)" butonu */
    ui_draw_rect(ui, x, y, w, ED_HEADER_H, ED_COL_HEADER_BG);
    ui_draw_text(ui, x + 6, y + 5, "MATOKI SHADER EDITORU", color_pack(72, 196, 142, 255), 1);

    i32 btn_w = 108;
    i32 btn_x = x + w - btn_w - 4;
    i32 btn_y = y + 2;
    i32 btn_h = ED_HEADER_H - 4;
    b32 btn_hover = (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h);
    ui_draw_rect(ui, btn_x, btn_y, btn_w, btn_h,
                 btn_hover ? color_pack(64, 128, 96, 255) : color_pack(42, 92, 68, 255));
    ui_draw_outline(ui, btn_x, btn_y, btn_w, btn_h, ED_COL_BORDER);
    ui_draw_text(ui, btn_x + 6, btn_y + 3, "Derle [Ctrl+Ent]", color_pack(240, 248, 255, 255), 1);
    if (btn_hover && ui->mouse_pressed) {
        ed->compile_requested = TRUE;
    }

    /* Gövde ve satır numarası oluğu (gutter) arka planı */
    ui_draw_rect(ui, x, code_y, ED_GUTTER_W, code_h, ED_COL_GUTTER_BG);
    ui_draw_rect(ui, code_x, code_y, code_w, code_h, ED_COL_BG);
    ui_draw_rect(ui, code_x - 1, code_y, 1, code_h, ED_COL_BORDER);

    /* Sözdizimi renklerini hesapla */
    static u32 s_char_colors[EDITOR_BUF_CAP];
    ed_colorize(&ed->lex_arena, ed->buf, ed->len, s_char_colors);

    u32 sel_s0 = 0, sel_s1 = 0;
    ed_selection_range(ed, &sel_s0, &sel_s1);
    b32 has_sel = (sel_s0 < sel_s1);

    /* Satır numaraları ve aktif satır vurgusu */
    ui_push_clip(ui, x, code_y, w, code_h);

    u32 line_no = 1;
    u32 pos     = 0;
    while (pos <= ed->len) {
        u32 ls = pos;
        u32 le = ls;
        while (le < ed->len && ed->buf[le] != '\n') le++;

        i32 py = code_y + 4 + (i32)(line_no - 1u) * ED_LINE_H - ed->scroll_y;
        if (py + ED_LINE_H >= code_y && py < code_y + code_h) {
            /* Aktif satır arka planı */
            if (line_no == cur_line) {
                ui_draw_rect(ui, code_x, py - 1, code_w, ED_LINE_H, ED_COL_CUR_LINE);
            }

            /* Bu satırda hata var mı? */
            b32 line_has_err = FALSE;
            u32 err_col_min  = 1;
            for (u32 e = 0; e < ed->error_count; e++) {
                if (ed->errors[e].line == line_no) {
                    line_has_err = TRUE;
                    err_col_min  = ed->errors[e].col;
                    break;
                }
            }
            if (line_has_err) {
                ui_draw_rect(ui, code_x, py - 1, code_w, ED_LINE_H, color_pack(90, 22, 28, 120));
            }

            /* Satır numarası */
            char lnum_buf[12];
            fmt_format(lnum_buf, sizeof(lnum_buf), "%2u", line_no);
            u32 lcol = line_has_err ? ED_COL_ERROR : ((line_no == cur_line) ? ED_COL_LINE_ACT : ED_COL_LINE_NUM);
            ui_draw_text(ui, x + 5, py, lnum_buf, lcol, 1);

            /* Satır içi metin ve seçim çizimi */
            ui_push_clip(ui, code_x + 2, code_y, code_w - 4, code_h);
            u32 p = ls;
            u32 col_idx = 0;
            while (p < le) {
                u32 char_start = p;
                const char* cptr = ed->buf + p;
                u32 cp = bitfont_utf8_next(&cptr);
                u32 next_p = (u32)(cptr - ed->buf);
                if (next_p <= p) next_p = p + 1u;

                i32 px = code_x + 4 + (i32)col_idx * ED_CHAR_W - ed->scroll_x;
                if (has_sel && char_start >= sel_s0 && char_start < sel_s1) {
                    ui_draw_rect(ui, px, py - 1, ED_CHAR_W, ED_LINE_H, ED_COL_SELECTION);
                }

                if (cp != ' ' && cp != '\t' && px + ED_CHAR_W >= code_x && px < code_x + code_w) {
                    char tmp[5];
                    u32 clen = MIN(next_p - char_start, 4u);
                    for (u32 k = 0; k < clen; k++) tmp[k] = ed->buf[char_start + k];
                    tmp[clen] = '\0';
                    ui_draw_text(ui, px, py, tmp, s_char_colors[char_start], 1);
                }

                p = next_p;
                col_idx++;
            }

            /* Hata satırı kırmızı dalgalı alt çizgisi */
            if (line_has_err) {
                i32 u_start_col = (i32)(err_col_min > 0 ? err_col_min - 1u : 0u);
                i32 u_end_col   = MAX((i32)col_idx, u_start_col + 4);
                i32 ux0 = code_x + 4 + u_start_col * ED_CHAR_W - ed->scroll_x;
                i32 ux1 = code_x + 4 + u_end_col * ED_CHAR_W - ed->scroll_x;
                i32 uy  = py + 8;
                for (i32 wx = ux0; wx < ux1; wx++) {
                    i32 wy = uy + ((wx & 2) ? 1 : 0);
                    ui_draw_rect(ui, wx, wy, 1, 1, ED_COL_ERROR);
                }
            }

            ui_pop_clip(ui);
        }

        if (le >= ed->len) break;
        pos = le + 1u;
        line_no++;
    }

    /* Yanıp sönen imleç */
    if (ed->focused && ed->blink_timer < 0.55f) {
        i32 cx = code_x + 4 + (i32)(cur_col - 1u) * ED_CHAR_W - ed->scroll_x;
        i32 cy = code_y + 3 + (i32)(cur_line - 1u) * ED_LINE_H - ed->scroll_y;
        if (cx >= code_x && cx < code_x + code_w) {
            ui_draw_rect(ui, cx, cy, 2, ED_LINE_H - 1, ED_COL_CURSOR);
        }
    }

    ui_pop_clip(ui);

    /* Alt durum / MtkDiag hata paneli */
    i32 diag_y = y + h - diag_h;
    ui_draw_rect(ui, x, diag_y, w, diag_h, ed->error_count > 0 ? ED_COL_DIAG_BG : ED_COL_DIAG_OK_BG);
    ui_draw_rect(ui, x, diag_y, w, 1, ED_COL_BORDER);

    if (ed->error_count > 0) {
        ui_push_clip(ui, x + 4, diag_y + 2, w - 8, diag_h - 4);
        ui_draw_text(ui, x + 6, diag_y + 4, ed->diag_text, ED_COL_ERROR, 1);
        ui_pop_clip(ui);
    } else {
        char status[128];
        fmt_format(status, sizeof(status),
                   "OK (%.2f ms, %u B wasm) | Sat %u:%u | Ctrl+Z/Y Ctrl+C/V",
                   ed->last_compile_ms, ed->last_wasm_bytes, cur_line, cur_col);
        ui_draw_text(ui, x + 6, diag_y + 5, status, ED_COL_OK, 1);
    }

    ui_draw_outline(ui, x, y, w, h, ED_COL_BORDER);
    ui_pop_clip(ui);

    return ed->compile_requested;
}
