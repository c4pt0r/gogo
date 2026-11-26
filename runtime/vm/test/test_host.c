/* The C tests' host and board (test_host.h). */
#include "test_host.h"
#include "module.h"
#include "platform_unix.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

static uint64_t (*clock_fn)(void);
static size_t arena_used, arena_cap = GOGO_ARENA_BYTES;

void test_host_set_clock(uint64_t (*fn)(void)) { clock_fn = fn; }
uint64_t test_host_now(void) { return clock_fn ? clock_fn() : platform_now_ms(); }

void test_host_set_arena_cap(size_t bytes) { arena_cap = bytes; }
size_t test_host_arena_used(void) { return arena_used; }
void test_host_arena_forget(void) { arena_used = 0; }

static void t_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    if (stream == 2) platform_write_err(s, n);   /* platform_unix_err_hook captures it */
    else platform_write(s, n);
}
static void *t_alloc(void *ctx, size_t n) {
    (void)ctx;
    if (arena_cap && n > arena_cap - arena_used) return NULL;
    void *p = malloc(n);
    if (p) arena_used += n;
    return p;
}
static void t_free(void *ctx, void *p, size_t n) {
    (void)ctx;
    arena_used = n > arena_used ? 0 : arena_used - n;
    free(p);
}
static uint64_t t_now(void *ctx) { (void)ctx; return test_host_now(); }
static void t_wake(void *ctx) { (void)ctx; platform_notify(); }
static void t_wait(void *ctx, uint32_t ms) { (void)ctx; platform_wait(ms); }
static uint32_t t_random(void *ctx) { (void)ctx; return platform_random(); }
static int32_t t_config_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    return platform_config_get(k, kl, buf, cap);
}

gogo_host test_host = {
    t_write, t_alloc, t_free, t_now, t_wake, t_wait, NULL, NULL, t_random, NULL, t_config_get, NULL, NULL
};

static const char *const test_mounts[] = { "/flash", "/sd", NULL };

VmBoard test_board = { unix_base_ports, NULL, &unix_netif, NULL, test_mounts };

int test_vm_init(Vm *vm, const Image *img, uint16_t table_cap) {
    if (vm_init(vm, &test_host, NULL, &test_board, table_cap) != 0) return -1;
    if (img && module_install_static(vm, img) != CODE_OK) return -1;
    return 0;
}

void test_vm_bare(Vm *vm) {
    memset(vm, 0, sizeof *vm);
    vm->host = &test_host;
    vm->board = &test_board;
}

int test_vm_boot(Vm *vm, const uint32_t *words, uint32_t n_words, uint16_t table_cap) {
    if (test_vm_init(vm, NULL, table_cap) != 0) return VM_BOOT_NO_MEMORY;
    return vm_boot(vm, words, n_words);
}
