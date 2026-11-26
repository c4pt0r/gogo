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

/* Appends the const-area object-start map and the trailer
 * (n_requires = 0, n_exports = 0) to the hand-built image w (header counts
 * already set, room for map + 2 words after the code) and returns the
 * image's length in words. */
#include <stdint.h>
#include <string.h>
static inline uint32_t img_seal(uint32_t *w) {
    uint32_t *c = w + 8 + w[2] * 3u + w[3] * 2u, nc = w[4], *map = c + nc + w[5];
    memset(map, 0, (nc + 31u) / 32u * 4u);
    for (uint32_t off = 0; off < nc; off += (c[off] >> 6) ? (c[off] >> 6) : 1u)
        map[off >> 5] |= 1u << (off & 31u);
    uint32_t *tr = map + (nc + 31u) / 32u;
    tr[0] = 0; tr[1] = 0;
    return (uint32_t)(tr - w) + 2u;
}

/* img_seal, then exports function 0 as "main" (its name must be the const
 * string at offset 0, "main"): an image vm_run can boot (module 0's main
 * starts by its export, never the header's entry). Needs 2 more words. */
static inline uint32_t img_seal_main(uint32_t *w) {
    uint32_t n = img_seal(w);
    w[n - 1] = 1;
    w[n] = 0; w[n + 1] = 0;
    return n + 2u;
}

/* Port PIDs of a Vm with the default cap (GOGO_MAX_PORTS): the tests'
 * own (vm_set_max_ports moves the slots). */
#include "port.h"
#include "config.h"
#define PORT_SLOT_BASE (PORT_SLOT_TOP - GOGO_MAX_PORTS)
static inline bool test_port_is(term pid) { return term_is_pid(pid) && term_pid_slot(pid) >= PORT_SLOT_BASE; }

#endif
