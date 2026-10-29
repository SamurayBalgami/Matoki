/* tests/test_log.c — core/log.c mini printf testleri.
 * Kaynak: Xassemblianist/MatokiEngine@0a7b782 (tests/test_log.c), izinle uyarlandı; adlar tools/port_names.py ile çevrildi.
 */
#include "test.h"
#include "core/mem.h"
#include "core/fmt.h"
#include "core/fmt.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

TEST(fmt_integers) {
    char b[64];
    fmt_format(b, sizeof b, "%d|%u|%x|%X", -42, 42u, 0xbeefu, 0xbeefu);
    CHECK(str_eq(b, "-42|42|beef|BEEF"));
    fmt_format(b, sizeof b, "%5d|%-5d|%05d|%08x", 42, 42, -42, 0x1fu);
    CHECK(str_eq(b, "   42|42   |-0042|0000001f"));
    fmt_format(b, sizeof b, "%lld|%llu", -9000000000LL, 18000000000ULL);
    CHECK(str_eq(b, "-9000000000|18000000000"));
    fmt_format(b, sizeof b, "%d", I32_MIN);
    CHECK(str_eq(b, "-2147483648"));
}

TEST(fmt_floats) {
    char b[64];
    fmt_format(b, sizeof b, "%f|%.2f|%.0f", 1.5, -3.14159, 2.5);
    CHECK(str_eq(b, "1.500000|-3.14|3"));
    fmt_format(b, sizeof b, "%.3f|%.1f", 0.9995, 9.96);
    CHECK(str_eq(b, "1.000|10.0"));
    fmt_format(b, sizeof b, "%08.2f", -1.5);
    CHECK(str_eq(b, "-0001.50"));
}

TEST(fmt_strings_and_trunc) {
    char b[8];
    u32 n = fmt_format(b, sizeof b, "%s-%c", "abcdef", 'z');
    CHECK(n == 7);
    CHECK(str_eq(b, "abcdef-"));
    fmt_format(b, sizeof b, "%-4s|", "ab");
    CHECK(str_eq(b, "ab  |"));
    fmt_format(b, sizeof b, "%.2s%%", "xyz");
    CHECK(str_eq(b, "xy%"));
}
