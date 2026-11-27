/* The VM's run-time parameters: one model for every platform (board
 * firmware, gogovm, libgogo). Each has a build default (config.h, or the
 * platform's own), a setting in the config store (the key below, read
 * once at VM start through the host's config_get) and, on a host, a
 * command-line flag or a gogo_config field. One precedence everywhere:
 *
 *     flag (or gogo_config field)  >  config key  >  platform default
 *
 * and every value is checked against the platform's ceiling as it is
 * resolved: out of range is clamped (or refused, where noted) with one
 * line on the console, and a value that is not a number is ignored with
 * one line ("gogo: bad <key>; using the default"), never silently.
 *
 *   field        key              what it bounds
 *   max_ports    vm.max_ports     ports open at once: 1..PORT_MAX_CEILING, and
 *                                 max_actors + max_ports at most PORT_SLOT_TOP
 *                                 (else refused: the default is kept)
 *   max_links    node.max_links   node links of distributed actors (0: the node
 *                                 port's default, DIST_LINKS); at most PORT_MAX_CEILING
 *   code_mem     code.mem         bytes of loaded module code (the boot image aside)
 *
 * The others are the build's, reported as they are: actor_memory (an
 * actor's default memory limit, GOGO_DEFAULT_MAX_WORDS), memory (all
 * actors together: the VM allocator's limit, 0 none; a board's arena,
 * GOGO_ARENA_BYTES, bounds it besides), max_actors (the actor table,
 * the platform's table_cap), bytes_handles and bytes_per_actor (the
 * off-heap Bytes table, GOGO_BYTES_HANDLES and GOGO_BYTES_PER_ACTOR).
 *
 * Integer keys are decimal; size keys (bytes) take an optional K or M
 * suffix (either case). Pure C99, no state: the platform keeps its
 * VmParams. */
#ifndef GOGO_PARAMS_H
#define GOGO_PARAMS_H

#include <stddef.h>
#include <stdint.h>
#include "host.h"

struct Vm;

typedef struct VmParams {
    uint64_t memory;            /* bytes all actors may take together; 0 = no VM limit */
    uint32_t actor_memory;      /* an actor's default limit, bytes */
    uint32_t max_actors;        /* live actors at once */
    uint32_t max_ports;         /* ports open at once */
    uint32_t max_links;         /* node links; 0 = the node port's default */
    uint32_t code_mem;          /* bytes of loaded module code */
    uint32_t bytes_handles;     /* off-heap Bytes, VM-wide */
    uint32_t bytes_per_actor;   /* and per actor */
} VmParams;

/* The build's defaults (config.h); a platform then sets its own where it
 * differs (gogovm: 1024 ports, 512 links). */
void vm_params_default(VmParams *p);

/* Resolves p in place: on entry p holds the platform's defaults; each
 * field becomes set's (when set is not NULL and its field is not 0), else
 * the config key's (h->config_get with ctx, when h has one), else stays
 * the default, then is checked (above). Lines go to h->write(ctx, 2, ...). */
void vm_params_resolve(const gogo_host *h, void *ctx, const VmParams *set, VmParams *p);

/* Applies resolved p to a Vm fresh from vm_init (its table: p->max_actors):
 * the port and link caps and the code memory limit. 0, or -1 if the Vm
 * refuses the port cap (vm_set_max_ports). */
int vm_params_apply(struct Vm *vm, const VmParams *p);

#endif
