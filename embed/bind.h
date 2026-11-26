//go:build cgo && libgogo

/* The C side of the binding (bind.c): the host table and the port driver
 * whose hooks call the Go functions exported by vm.go and port.go. Go
 * values cross as cgo.Handle numbers (uintptr_t). */
#ifndef GOGO_BIND_H
#define GOGO_BIND_H

#include <stdint.h>
#include <stdlib.h>
#include "gogo.h"

/* Which host hooks call Go; the others are bind.c's defaults (the OS
 * clocks and entropy) or absent (config_get). */
enum {
    BIND_NOW = 1,
    BIND_UNIX_TIME = 2,
    BIND_RANDOM = 4,
    BIND_CONFIG = 8
};

/* Makes a VM whose host hooks get the Go state h; the bytes it holds
 * from the host are counted, and given to Go's destroy hook. */
int bind_new(uintptr_t h, unsigned hooks, const gogo_config *cfg, gogo_vm **out);

/* gogo_register_port with the Go driver h. */
int bind_register_port(gogo_vm *vm, const char *name, size_t len, uintptr_t h);

/* The default monotonic clock (CLOCK_MONOTONIC, ms). */
uint64_t bind_now(void);

#endif
