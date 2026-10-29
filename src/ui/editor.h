/* ui/editor.h — Motor içi canlı Matoki shader metin editörü.
 * Kendi 8x8 bitmap fontumuzla çizilen, satır numaralı, Matoki sözdizimi renklendirmeli
 * (src/matoki/lexer.h ile), imleç/seçim/kaydırma, halka tamponlu geri al/yinele (Ctrl+Z/Y),
 * pano (Ctrl+C/V/X), tuş tekrarı ve hata alt çizgisi + hata paneli destekli kod editörü.
 * Kaynak: Xassemblianist/MatokiEngine@2ac467a (src/ui/editor.h), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#ifndef UI_EDITOR_H
#define UI_EDITOR_H

#include "core/types.h"
#include "ui/ui.h"
#include "matoki/matoki.h"

#define EDITOR_BUF_CAP    8192
#define EDITOR_UNDO_CAP   16
#define EDITOR_CLIP_CAP   4096
#define EDITOR_MAX_ERRORS 20
#define EDITOR_LEX_BYTES  (512u * 1024u)   /* sözdizimi renklendirme token'ları */

typedef struct {
    char buf[EDITOR_BUF_CAP];
    u32  len;
    u32  cursor;
} EditorSnapshot;

typedef struct {
    u32 line;
    u32 col;
} EditorErrorLoc;

typedef struct {
    char buf[EDITOR_BUF_CAP];
    u32  len;
    u32  cursor;       /* byte ofseti */
    u32  sel_anchor;   /* seçim başlangıç byte ofseti (sel_anchor == cursor ise seçim yok) */

    i32  scroll_y;     /* piksel cinsinden dikey kaydırma */
    i32  scroll_x;     /* piksel cinsinden yatay kaydırma */
    b32  focused;
    b32  dragging;
    f32  blink_timer;

    /* Sabit boyutlu geri al / yinele (undo / redo) halka tamponu */
    EditorSnapshot undo_ring[EDITOR_UNDO_CAP];
    u32            undo_head;  /* en eski kayıt indeksi */
    u32            undo_count; /* toplam geçerli kayıt sayısı */
    u32            undo_pos;   /* aktif durumun undo_head'e göre ofseti (0..undo_count-1) */

    /* İç pano (Ctrl+C / Ctrl+V / Ctrl+X) */
    char clipboard[EDITOR_CLIP_CAP];
    u32  clip_len;

    /* Tuş tekrarı durumu */
    u8   prev_keys[256];
    u32  repeat_key;
    f32  repeat_timer;

    /* Derleme hataları ve MtkDiag mesaj paneli */
    char           diag_text[256];   /* bizim MatokiError: "satır:sütun: mesaj" */
    u32            diag_len;
    EditorErrorLoc errors[EDITOR_MAX_ERRORS];
    u32            error_count;
    f64            last_compile_ms;
    u32            last_wasm_bytes;
    b32            compile_requested;
    Arena          lex_arena;   /* renklendirme: her çizimde geri sarılır */
} CodeEditor;

/* Editörü ilklendirir ve başlangıç kaynak metnini yükler; renklendirme belleği a'dan (EDITOR_LEX_BYTES). */
void editor_init(CodeEditor* ed, Arena* a, const char* initial_src, u32 initial_len);

/* Editör metnini dışarıdan değiştirir ve geri alma geçmişini sıfırlar. */
void editor_set_text(CodeEditor* ed, const char* src, u32 len);

/* Derleme sonucu: err NULL ya da failed değilse başarı; değilse hata konumu + metni (alt çizgi, panel). */
void editor_set_diag(CodeEditor* ed, const MatokiError* err, f64 compile_ms, u32 wasm_bytes);

/* Girdileri işler ve editörü verilen dikdörtgen (x, y, w, h) içine çizer.
 * Kullanıcı Ctrl+Enter veya "Derle" butonuna bastığında TRUE döner. */
b32  editor_update_and_draw(CodeEditor* ed, UiContext* ui, const UiInput* input,
                            f32 dt, i32 x, i32 y, i32 w, i32 h);

#endif
