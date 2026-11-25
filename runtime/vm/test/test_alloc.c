#include "actor.h"
#include "heap.h"
#include "config.h"
#include "test.h"
#include "alloc.h"
#include "actor.h"
#include "heap.h"
#include "config.h"
#include <stddef.h>

/* Compile-time guard: every config macro must be defined (and nonzero where
 * a zero makes no sense) in both the default and the -D MCU test builds. */
typedef char config_has_defaults[
    (GOGO_DEFAULT_MAX_WORDS > 0u && GOGO_TABLE_CAP > 0u && GOGO_MAX_PORTS > 0u &&
     GOGO_EVQ_CAP > 0u && GOGO_RESERVE_BYTES > 0u && GOGO_ARENA_BYTES >= 0u) ? 1 : -1];

/* unix platform test hooks */
void platform_unix_set_arena_cap(size_t bytes);
size_t platform_unix_arena_used(void);

TEST(alloc_rounds_to_class) {
    Vm vm;
    va_reset(&vm);
    void *p = va_alloc(&vm, 20);
    CHECK(p != NULL);
    CHECK_EQ(va_used_bytes(&vm), 24);
    va_free(&vm, p, 20);
    CHECK_EQ(va_used_bytes(&vm), 0);
}

TEST(alloc_free_reuses) {
    Vm vm;
    va_reset(&vm);
    void *p = va_alloc(&vm, 48);
    va_free(&vm, p, 48);
    void *q = va_alloc(&vm, 48);
    CHECK(p == q);
    va_free(&vm, q, 48);
}

TEST(alloc_large) {
    Vm vm;
    va_reset(&vm);
    size_t before = va_used_bytes(&vm);
    void *p = va_alloc(&vm, 1000);
    CHECK(p != NULL);
    CHECK_EQ(va_used_bytes(&vm), before + 1000);
    va_free(&vm, p, 1000);
    CHECK_EQ(va_used_bytes(&vm), before);
}

TEST(alloc_limit) {
    Vm vm;
    va_reset(&vm);
    va_set_limit(&vm, 64);
    void *a = va_alloc(&vm, 48);
    void *b = va_alloc(&vm, 48);
    CHECK(a != NULL);
    CHECK(b == NULL);
    va_free(&vm, a, 48);
    va_set_limit(&vm, 0);
}

TEST(alloc_many_distinct) {
    Vm vm;
    va_reset(&vm);
    void *p[100];
    for (int i = 0; i < 100; i++) {
        p[i] = va_alloc(&vm, 40);
        CHECK(p[i] != NULL);
        for (int j = 0; j < i; j++) CHECK(p[i] != p[j]);
    }
    for (int i = 0; i < 100; i++) va_free(&vm, p[i], 40);
    CHECK_EQ(va_used_bytes(&vm), 0);
}

/* The unix platform's arena cap refuses chunks past it; the VM reports
 * out of memory (heap_alloc fails, the actor stays valid) instead of
 * crashing. */
TEST(arena_cap) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 64);
    CHECK(a != NULL);
    if (!a) return;
    platform_unix_set_arena_cap(platform_unix_arena_used() + 5000u);
    void *p = va_alloc(&vm, 3000);
    CHECK(p != NULL);
    CHECK(va_alloc(&vm, 3000) == NULL);                /* would pass the cap */
    CHECK_EQ(heap_alloc(&vm, a, 4000, NULL, 0), UINT32_MAX);   /* 16000 bytes */
    CHECK_EQ(a->mem_words, 64);
    va_free(&vm, p, 3000);
    void *q = va_alloc(&vm, 3000);                     /* freed bytes are reusable */
    CHECK(q != NULL);
    va_free(&vm, q, 3000);
    platform_unix_set_arena_cap(GOGO_ARENA_BYTES);
}
