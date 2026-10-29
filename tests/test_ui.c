/* tests/test_ui.c — M13 faz 6: onların arayüz (ui), bitmap font (ui/bitfont) ve canlı shader editörü (ui/editor).
 * Onlarda bu modüllerin testi yoktu. Burada: Türkçe glifler + UTF-8 çözme, düğme yalnız içeride bas-bırakta tıklar,
 * kaydırıcı sınırda kalır, kırpma dışına piksel yazılmaz; editörde yazma / silme / geri alma / Ctrl+Enter, gerçek
 * lit.mtk bozulunca hata satırı ve paneli, 10 000 karelik rastgele girdi altında tampon değişmezleri. */
#include "test.h"
#include "core/embed.h"
#include "core/mem.h"
#include "core/rng.h"
#include "matoki/matoki.h"
#include "ui/bitfont.h"
#include "ui/editor.h"
#include "ui/ui.h"

static Framebuffer make_fb(i32 w, i32 h) {
    Framebuffer fb;
    mem_zero(&fb, sizeof(fb));
    fb.w = w;
    fb.h = h;
    fb.color = ARENA_ARRAY(test_arena(), u32, (usize)w * (usize)h);
    return fb;
}

TEST(bitfont_turkish_glyphs_and_utf8) {
    const char* s = "çğıöşüÇĞİÖŞÜ";
    const char* p = s;
    u32 n = 0, missing = 0;
    while (*p) {
        u32 cp = bitfont_utf8_next(&p);
        missing += !bitfont_has_glyph(cp);
        n++;
    }
    CHECK(n == 12);
    CHECK(missing == 0);
    CHECK(bitfont_measure_width(s, 1) == 12 * 8);
    CHECK(bitfont_measure_width("ab", 2) == 2 * 8 * 2);
    /* bozuk UTF-8 her adımda ilerler (sonsuz döngü yok) */
    const char bad[] = { (char)0xFF, (char)0xC3, (char)0xE2, (char)0x82, 'x', 0 };
    p = bad;
    u32 steps = 0;
    while (*p && steps < 16) {
        bitfont_utf8_next(&p);
        steps++;
    }
    CHECK(*p == 0 && steps <= 5);
}

/* bir kare: panel + düğme + onay kutusu + kaydırıcı */
typedef struct UiFrame {
    b32 clicked, toggled, slid;
} UiFrame;

static UiFrame ui_frame(UiContext* ui, const UiInput* in, b32* check, f32* slide) {
    UiFrame r;
    ui_begin(ui, in);
    ui_panel_begin(ui, "Panel", 10, 10, 200, 150);
    r.clicked = ui_button(ui, "Tamam");        /* y = 10 + 18 + 6 = 34, h 16 */
    r.toggled = ui_checkbox(ui, "Sis", check); /* y = 54, h 14 */
    r.slid = ui_slider_f32(ui, "Hız", slide, 0.0f, 10.0f);   /* çubuk y = 72 + 11 = 83, h 10, x 16..210 */
    ui_panel_end(ui);
    ui_end(ui);
    return r;
}

TEST(ui_button_checkbox_slider) {
    Framebuffer fb = make_fb(320, 200);
    UiContext ui;
    ui_init(&ui, &fb);
    UiInput in;
    mem_zero(&in, sizeof(in));
    b32 check = FALSE;
    f32 slide = 5.0f;

    /* düğme: içeride bas → içeride bırak = tık (basış karesinde değil) */
    in.mouse_x = 100; in.mouse_y = 40;
    CHECK(!ui_frame(&ui, &in, &check, &slide).clicked);
    in.mouse_buttons = 1;
    CHECK(!ui_frame(&ui, &in, &check, &slide).clicked);
    in.mouse_buttons = 0;
    CHECK(ui_frame(&ui, &in, &check, &slide).clicked);
    /* dışarıda bas, içeride bırak → tık yok */
    in.mouse_x = 300; in.mouse_y = 190; in.mouse_buttons = 1;
    ui_frame(&ui, &in, &check, &slide);
    in.mouse_x = 100; in.mouse_y = 40;
    ui_frame(&ui, &in, &check, &slide);
    in.mouse_buttons = 0;
    CHECK(!ui_frame(&ui, &in, &check, &slide).clicked);

    /* onay kutusu bas-bırak ile değişir */
    in.mouse_x = 30; in.mouse_y = 60; in.mouse_buttons = 1;
    ui_frame(&ui, &in, &check, &slide);
    in.mouse_buttons = 0;
    UiFrame f = ui_frame(&ui, &in, &check, &slide);
    CHECK(f.toggled && check);

    /* kaydırıcı: çubukta bas, sağ dışarı sürükle → üst sınırda kalır */
    in.mouse_x = 16; in.mouse_y = 87; in.mouse_buttons = 1;
    ui_frame(&ui, &in, &check, &slide);
    CHECK_NEAR(slide, 0.0f, 1e-6);
    in.mouse_x = 1000;
    ui_frame(&ui, &in, &check, &slide);
    CHECK_NEAR(slide, 10.0f, 1e-6);
    in.mouse_buttons = 0;
    ui_frame(&ui, &in, &check, &slide);
}

TEST(ui_clip_never_writes_outside) {
    Framebuffer fb = make_fb(64, 64);
    for (u32 i = 0; i < 64u * 64u; i++) fb.color[i] = 0x12345678u;
    UiContext ui;
    ui_init(&ui, &fb);
    UiInput in;
    mem_zero(&in, sizeof(in));
    ui_begin(&ui, &in);
    ui_push_clip(&ui, 10, 12, 20, 16);
    ui_draw_rect(&ui, -100, -100, 400, 400, color_pack(255, 0, 0, 255));
    ui_draw_line(&ui, -50, 70, 90, -20, color_pack(0, 255, 0, 255));
    ui_draw_text(&ui, 0, 14, "ÇOK UZUN BİR SATIR ŞĞÜ", color_pack(0, 0, 255, 255), 2);
    ui_pop_clip(&ui);
    ui_draw_rect(&ui, 60, 60, 100, 100, color_pack(255, 255, 0, 255));   /* tuval dışına taşan */
    ui_end(&ui);
    u32 outside = 0, inside_changed = 0;
    for (i32 y = 0; y < 64; y++) {
        for (i32 x = 0; x < 64; x++) {
            b32 in_clip = x >= 10 && x < 30 && y >= 12 && y < 28;
            b32 in_corner = x >= 60 && y >= 60;
            u32 c = fb.color[y * 64 + x];
            if (!in_clip && !in_corner) outside += c != 0x12345678u;
            if (in_clip) inside_changed += c != 0x12345678u;
        }
    }
    CHECK(outside == 0);
    CHECK(inside_changed == 20u * 16u);
}

#define K_CTRL 17
#define K_ENTER 13
#define K_BACK 8
#define K_Z 90

static b32 ed_frame(CodeEditor* ed, UiContext* ui, UiInput* in) {
    ui_begin(ui, in);
    b32 r = editor_update_and_draw(ed, ui, in, 1.0f / 60.0f, 0, 0, 480, 300);
    ui_end(ui);
    in->char_count = 0;
    in->wheel = 0;
    return r;
}

TEST(editor_typing_undo_and_compile_error) {
    Arena* a = test_arena();
    Framebuffer fb = make_fb(480, 300);
    UiContext ui;
    ui_init(&ui, &fb);
    CodeEditor* ed = ARENA_NEW(a, CodeEditor);
    const EmbedFile* lit = embed_find("lit.mtk");
    CHECK(lit != NULL);
    if (!lit) return;
    editor_init(ed, a, (const char*)lit->data, lit->len);
    CHECK(ed->len == lit->len && mem_equal(ed->buf, lit->data, lit->len));

    UiInput in;
    mem_zero(&in, sizeof(in));
    in.mouse_x = 200; in.mouse_y = 100;   /* editör üstünde: odak korunur */
    ed_frame(ed, &ui, &in);

    /* imleç başta: "ab" yaz, bir sil */
    in.chars[0] = 'a'; in.chars[1] = 'b'; in.char_count = 2;
    ed_frame(ed, &ui, &in);
    CHECK(ed->len == lit->len + 2 && ed->buf[0] == 'a' && ed->buf[1] == 'b');
    in.keys[K_BACK] = 1;
    ed_frame(ed, &ui, &in);
    in.keys[K_BACK] = 0;
    ed_frame(ed, &ui, &in);
    CHECK(ed->len == lit->len + 1 && ed->buf[0] == 'a');
    /* Türkçe karakter UTF-8 olarak girer */
    in.chars[0] = 0x15Fu; in.char_count = 1;   /* ş */
    ed_frame(ed, &ui, &in);
    CHECK(ed->len == lit->len + 3 && (u8)ed->buf[1] == 0xC5u && (u8)ed->buf[2] == 0x9Fu);

    /* Ctrl+Enter derleme ister; bozuk kaynakta (başta "aş") hata 1. satırda */
    in.keys[K_CTRL] = 1; in.keys[K_ENTER] = 1;
    CHECK(ed_frame(ed, &ui, &in));
    in.keys[K_ENTER] = 0;
    ed_frame(ed, &ui, &in);
    MatokiProgram* prog = ARENA_NEW(a, MatokiProgram);
    MatokiError err;
    mem_zero(&err, sizeof(err));
    b32 ok = matoki_compile(a, ed->buf, ed->len, NULL, prog, &err);
    CHECK(!ok && err.failed && err.line == 1);
    editor_set_diag(ed, &err, 0.5, 0);
    CHECK(ed->error_count == 1 && ed->errors[0].line == 1u && ed->diag_len > 0);
    ed_frame(ed, &ui, &in);   /* hata paneli çizilir */

    /* Ctrl+Z üç kez: özgün metne döner, derleme geçer, hata temizlenir */
    for (u32 k = 0; k < 3; k++) {
        in.keys[K_Z] = 1;
        ed_frame(ed, &ui, &in);
        in.keys[K_Z] = 0;
        ed_frame(ed, &ui, &in);
    }
    in.keys[K_CTRL] = 0;
    CHECK(ed->len == lit->len && mem_equal(ed->buf, lit->data, lit->len));
    mem_zero(&err, sizeof(err));
    CHECK(matoki_compile(a, ed->buf, ed->len, NULL, prog, &err));
    editor_set_diag(ed, &err, 0.5, 1234);
    CHECK(ed->error_count == 0 && ed->diag_len == 0);
}

/* gerileme (faz 6, tarayıcıda bulundu): yavaş karede (dt = 1 s) tek Enter dokunuşu aynı karede ~15 kez tekrarlanıyordu */
TEST(editor_slow_frame_single_tap_no_repeat) {
    Arena* a = test_arena();
    Framebuffer fb = make_fb(480, 300);
    UiContext ui;
    ui_init(&ui, &fb);
    CodeEditor* ed = ARENA_NEW(a, CodeEditor);
    editor_init(ed, a, "abc", 3);
    UiInput in;
    mem_zero(&in, sizeof(in));
    in.mouse_x = 200; in.mouse_y = 100;
    in.keys[K_ENTER] = 1;
    ui_begin(&ui, &in);
    editor_update_and_draw(ed, &ui, &in, 1.0f, 0, 0, 480, 300);
    ui_end(&ui);
    in.keys[K_ENTER] = 0;
    ui_begin(&ui, &in);
    editor_update_and_draw(ed, &ui, &in, 1.0f, 0, 0, 480, 300);
    ui_end(&ui);
    CHECK(ed->len == 4 && ed->buf[0] == '\n');
    /* basılı tutulan tuş: yavaş karede bile kare başına en çok 0,1 s'lik tekrar (≤ 3) */
    in.keys[K_ENTER] = 1;
    for (u32 f = 0; f < 3; f++) {
        ui_begin(&ui, &in);
        editor_update_and_draw(ed, &ui, &in, 1.0f, 0, 0, 480, 300);
        ui_end(&ui);
    }
    CHECK(ed->len >= 5 && ed->len <= 4 + 1 + 2 * 3);
}

TEST(editor_random_input_keeps_invariants) {
    Arena* a = test_arena();
    Framebuffer fb = make_fb(480, 300);
    UiContext ui;
    ui_init(&ui, &fb);
    CodeEditor* ed = ARENA_NEW(a, CodeEditor);
    const char* start = "uniform tint: vec3;\nfn frag() -> vec4 {\n    return vec4(tint, 1.0);\n}\n";
    editor_init(ed, a, start, (u32)cstr_len(start));
    static const u8 keys[] = { 8, 9, 13, 16, 17, 35, 36, 37, 38, 39, 40, 46, 65, 67, 86, 88, 89, 90 };
    Rng r = rng_make(0xED17u);
    UiInput in;
    mem_zero(&in, sizeof(in));
    u32 bad = 0, compiles = 0;
    for (u32 f = 0; f < 10000u; f++) {
        u32 roll = rng_u32(&r) % 100u;
        if (roll < 30u) {
            in.char_count = 1 + rng_u32(&r) % 4u;
            for (u32 k = 0; k < in.char_count; k++) {
                u32 c = rng_u32(&r) % 8u;
                in.chars[k] = c == 0 ? 0x11Fu : c == 1 ? 0x131u : c == 2 ? 10u : 32u + rng_u32(&r) % 95u;
            }
        } else if (roll < 70u) {
            u8 k = keys[rng_u32(&r) % ARRAY_COUNT(keys)];
            in.keys[k] = (u8)!in.keys[k];
        } else if (roll < 85u) {
            in.mouse_x = (i32)(rng_u32(&r) % 520u) - 20;
            in.mouse_y = (i32)(rng_u32(&r) % 340u) - 20;
            in.mouse_buttons = rng_u32(&r) & 1u;
        } else {
            in.wheel = (i32)(rng_u32(&r) % 5u) - 2;
        }
        compiles += ed_frame(ed, &ui, &in);
        bad += !(ed->len < EDITOR_BUF_CAP && ed->buf[ed->len] == 0 && ed->cursor <= ed->len && ed->sel_anchor <= ed->len &&
                 ed->scroll_y >= 0 && ed->scroll_x >= 0);
    }
    CHECK(bad == 0);
    CHECK(compiles > 0);
    test_note("rastgele girdi: son metin uzunluğu", (f64)ed->len, 0);
}
