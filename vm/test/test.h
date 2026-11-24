/* Minimal test harness: TEST(name) { ... } registers via a constructor. */
#ifndef GOGO_TEST_H
#define GOGO_TEST_H

#include <stdio.h>

typedef void (*test_fn)(void);

void test_register(const char *name, test_fn fn);
void test_fail(const char *file, int line, const char *expr);
void test_fail_eq(const char *file, int line, const char *ea, const char *eb,
                  long long a, long long b);

#define TEST(name)                                                   \
    static void test_##name(void);                                   \
    __attribute__((constructor)) static void reg_##name(void) {      \
        test_register(#name, test_##name);                           \
    }                                                                \
    static void test_##name(void)

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) test_fail(__FILE__, __LINE__, #cond);           \
    } while (0)

#define CHECK_EQ(a, b)                                               \
    do {                                                             \
        long long a_ = (long long)(a), b_ = (long long)(b);          \
        if (a_ != b_) test_fail_eq(__FILE__, __LINE__, #a, #b, a_, b_); \
    } while (0)

#endif
