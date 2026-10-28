/* tests/test.h — MATOKI mini test çatısı.
 * Testler wasm'a derlenip node ile koşar (tests/run.mjs); host libc gerekmez.
 * build.py `TEST(isim)` satırlarını tarayıp build/test_registry.c kayıt tablosunu üretir.
 * Her test başında test_arena() sıfırlanır (256 MB). */
#ifndef TESTS_TEST_H
#define TESTS_TEST_H
#include "core/types.h"
#include "core/arena.h"

typedef struct TestCtx {
    const char* name;
    u32 checks;
    u32 failures;
} TestCtx;

typedef struct TestEntry {
    const char* name;
    void (*fn)(TestCtx* t_);
} TestEntry;

void   test_fail(TestCtx* t, const char* file, i32 line, const char* expr);
void   test_fail_near(TestCtx* t, const char* file, i32 line, const char* expr, f64 a, f64 b, f64 eps);
void   test_note(const char* label, f64 value, u32 decimals);
Arena* test_arena(void);

#define TEST(name) void test_##name(TestCtx* t_)

#define CHECK(c) do { \
        t_->checks++; \
        if (!(c)) test_fail(t_, __FILE__, __LINE__, #c); \
    } while (0)

#define CHECK_NEAR(a, b, eps) do { \
        f64 a_ = (f64)(a), b_ = (f64)(b), e_ = (f64)(eps); \
        t_->checks++; \
        if (!(a_ - b_ <= e_ && b_ - a_ <= e_)) test_fail_near(t_, __FILE__, __LINE__, #a " ~ " #b, a_, b_, e_); \
    } while (0)

#endif
