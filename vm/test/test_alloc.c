#include "test.h"
#include "alloc.h"

TEST(alloc_rounds_to_class) {
    va_reset();
    void *p = va_alloc(20);
    CHECK(p != NULL);
    CHECK_EQ(va_used_bytes(), 24);
    va_free(p, 20);
    CHECK_EQ(va_used_bytes(), 0);
}

TEST(alloc_free_reuses) {
    va_reset();
    void *p = va_alloc(48);
    va_free(p, 48);
    void *q = va_alloc(48);
    CHECK(p == q);
    va_free(q, 48);
}

TEST(alloc_large) {
    va_reset();
    size_t before = va_used_bytes();
    void *p = va_alloc(1000);
    CHECK(p != NULL);
    CHECK_EQ(va_used_bytes(), before + 1000);
    va_free(p, 1000);
    CHECK_EQ(va_used_bytes(), before);
}

TEST(alloc_limit) {
    va_reset();
    va_set_limit(64);
    void *a = va_alloc(48);
    void *b = va_alloc(48);
    CHECK(a != NULL);
    CHECK(b == NULL);
    va_free(a, 48);
    va_set_limit(0);
}

TEST(alloc_many_distinct) {
    va_reset();
    void *p[100];
    for (int i = 0; i < 100; i++) {
        p[i] = va_alloc(40);
        CHECK(p[i] != NULL);
        for (int j = 0; j < i; j++) CHECK(p[i] != p[j]);
    }
    for (int i = 0; i < 100; i++) va_free(p[i], 40);
    CHECK_EQ(va_used_bytes(), 0);
}
