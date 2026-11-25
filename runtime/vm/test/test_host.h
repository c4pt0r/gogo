/* The C tests' host and board (test_host.c): the unix board's functions
 * behind a gogo_host the tests can change, a fake-able clock and a
 * cap-able arena. */
#ifndef GOGO_TEST_HOST_H
#define GOGO_TEST_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "actor.h"

/* Both mutable: a test may swap a hook (e.g. code_alloc) or the board's
 * input (the fake InputHw) and must put it back. */
extern gogo_host test_host;
extern VmBoard test_board;      /* unix base ports, unix netif, every cap */
extern uint32_t test_granted;   /* the caps test_vm_init grants (default all) */

/* vm_init on test_host / test_board with test_granted, then (img not NULL)
 * module_install_static. 0, or -1 on failure. */
int test_vm_init(Vm *vm, const Image *img, uint16_t table_cap);
/* A cleared vm tied to test_host / test_board, without vm_init's PID table
 * (allocator tests). */
void test_vm_bare(Vm *vm);
/* test_vm_init(vm, NULL, table_cap), then vm_boot. */
int test_vm_boot(Vm *vm, const uint32_t *words, uint32_t n_words, uint16_t table_cap);

/* The clock: fn (NULL = the unix board's), e.g. a fake or one counting reads. */
void test_host_set_clock(uint64_t (*fn)(void));
uint64_t test_host_now(void);   /* what test_host.now_ms returns */

/* The arena: a nonzero cap makes alloc fail past it (default
 * GOGO_ARENA_BYTES); used bytes; forget forgets what earlier tests leaked
 * (later frees of it saturate at 0). */
void test_host_set_arena_cap(size_t bytes);
size_t test_host_arena_used(void);
void test_host_arena_forget(void);

#endif
