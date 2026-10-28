/* tests/test_main.c — test koşucusu (wasm export'u test_main).
 * Her testi sırayla çalıştırır, test başına en çok 8 hata satırı basar.
 * Dönüş değeri: başarısız test sayısı (run.mjs bunu çıkış koduna çevirir). */
#include "test.h"
#include "core/fmt.h"
#include "core/sys.h"

extern const TestEntry g_tests[];
extern const u32 g_test_count;

static Arena g_arena;
static b32 g_arena_ok;

Arena* test_arena(void) { return &g_arena; }

static void fail_header(TestCtx* t, const char* file, i32 line, StrBuf* sb) {
    sb_cstr(sb, "  FAIL ");
    sb_cstr(sb, t->name);
    sb_cstr(sb, " @ ");
    const char* base = file;
    for (const char* p = file; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    sb_cstr(sb, base);
    sb_char(sb, ':');
    sb_i64(sb, line);
    sb_cstr(sb, ": ");
}

void test_fail(TestCtx* t, const char* file, i32 line, const char* expr) {
    t->failures++;
    if (t->failures > 8) return;
    char buf[512];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    fail_header(t, file, line, &sb);
    sb_cstr(&sb, expr);
    log_sb(&sb);
}

void test_fail_near(TestCtx* t, const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps) {
    t->failures++;
    if (t->failures > 8) return;
    char buf[512];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    fail_header(t, file, line, &sb);
    sb_cstr(&sb, expr);
    sb_cstr(&sb, "  (");
    sb_f64(&sb, a, 7);
    sb_cstr(&sb, " vs ");
    sb_f64(&sb, b, 7);
    sb_cstr(&sb, ", eps ");
    sb_f64(&sb, eps, 7);
    sb_char(&sb, ')');
    log_sb(&sb);
}

void test_note(const char* label, f64 value, u32 decimals) {
    char buf[256];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    sb_cstr(&sb, "    . ");
    sb_cstr(&sb, label);
    sb_cstr(&sb, " = ");
    sb_f64(&sb, value, decimals);
    log_sb(&sb);
}

EXPORT(test_main) i32 test_main(void) {
    if (!g_arena_ok) {
        if (!arena_init_sys(&g_arena, (usize)256 << 20)) {
            log_cstr("test arena ayrılamadı");
            return 1;
        }
        g_arena_ok = TRUE;
    }
    u32 failed = 0, checks = 0;
    for (u32 i = 0; i < g_test_count; i++) {
        TestCtx t = { g_tests[i].name, 0, 0 };
        arena_reset(&g_arena, 0);
        f64 t0 = sys_now_ms();
        g_tests[i].fn(&t);
        f64 t1 = sys_now_ms();
        checks += t.checks;
        char buf[256];
        StrBuf sb;
        sb_init(&sb, buf, sizeof(buf));
        sb_cstr(&sb, t.failures ? "FAIL " : "ok   ");
        sb_cstr(&sb, t.name);
        sb_cstr(&sb, "  (");
        sb_u64(&sb, t.checks);
        sb_cstr(&sb, " kontrol, ");
        sb_f64(&sb, t1 - t0, 1);
        sb_cstr(&sb, " ms)");
        log_sb(&sb);
        if (t.failures) failed++;
    }
    char buf[128];
    StrBuf sb;
    sb_init(&sb, buf, sizeof(buf));
    sb_u64(&sb, g_test_count - failed);
    sb_char(&sb, '/');
    sb_u64(&sb, g_test_count);
    sb_cstr(&sb, " test geçti, ");
    sb_u64(&sb, checks);
    sb_cstr(&sb, " kontrol");
    log_sb(&sb);
    return (i32)failed;
}
