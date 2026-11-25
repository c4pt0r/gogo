#include "test.h"

#define MAX_TESTS 512

/* the test host (test_host.c): forget memory earlier tests leaked (matters under a cap) */
void test_host_arena_forget(void);

static struct { const char *name; test_fn fn; } tests[MAX_TESTS];
static int n_tests;
static int n_failed_checks;
static int n_dropped;

void test_register(const char *name, test_fn fn) {
    if (n_tests < MAX_TESTS) {
        tests[n_tests].name = name;
        tests[n_tests].fn = fn;
        n_tests++;
    } else {
        printf("ERROR: too many tests (MAX_TESTS=%d); dropped %s\n", MAX_TESTS, name);
        n_dropped++;
    }
}

void test_fail(const char *file, int line, const char *expr) {
    printf("  FAIL %s:%d: %s\n", file, line, expr);
    n_failed_checks++;
}

void test_fail_eq(const char *file, int line, const char *ea, const char *eb,
                  long long a, long long b) {
    printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", file, line, ea, eb, a, b);
    n_failed_checks++;
}

int main(void) {
    int passed = 0, failed = 0;
    for (int i = 0; i < n_tests; i++) {
        n_failed_checks = 0;
        test_host_arena_forget();
        tests[i].fn();
        if (n_failed_checks) {
            printf("FAIL %s\n", tests[i].name);
            failed++;
        } else {
            passed++;
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return (failed || n_dropped) ? 1 : 0;
}
