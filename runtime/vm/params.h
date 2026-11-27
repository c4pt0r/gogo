/* The VM's run-time parameters: one model for every platform (board
 * firmware, gogovm, libgogo). Each has a build default (config.h, or the
 * platform's own), a setting in the config store (the key below, read
 * once at VM start through the host's config_get) and, on a host, a
 * command-line flag or a gogo_config field. One precedence everywhere:
 *
 *     flag (or gogo_config field)  >  config key  >  platform default
 *
 * and every value is checked against the platform's ceiling as it is
 * resolved: out of range is clamped with one line on the console
 * ("gogo: vm.actor_memory 2M is over this VM's 256K; using 256K"), and a
 * value that is not a number is ignored with one line ("gogo: bad
 * vm.actor_memory; using the default"), never silently.
 *
 *   field            key              what it bounds; range
 *   actor_memory     vm.actor_memory  an actor's default memory limit (heap, stack,
 *                                     mailbox), bytes: 1K .. GOGO_ACTOR_MAX_WORDS
 *                                     words (256 KB - 4 on a board, 1 GB on the host)
 *   memory           vm.memory        all actors together (the VM allocator's
 *                                     limit), bytes; 0: none (a board's arena,
 *                                     GOGO_ARENA_BYTES, still bounds it, and is its
 *                                     ceiling)
 *   max_actors       vm.max_actors    live actors at once (the actor table):
 *                                     1 .. 65520
 *   max_ports        vm.max_ports     ports open at once: 1 .. PORT_MAX_CEILING, and
 *                                     max_actors + max_ports at most PORT_SLOT_TOP
 *                                     (else refused: the default, or what the
 *                                     actors leave)
 *   max_links        node.max_links   node links of distributed actors (0: the node
 *                                     port's default, DIST_LINKS): at most
 *                                     PORT_MAX_CEILING
 *   code_mem         code.mem         bytes of loaded module code (the boot image
 *                                     aside); at most 4 GB - 1
 *   bytes_handles    vm.handles       off-heap Bytes handles VM-wide, 1 ..
 *   bytes_per_actor                   GOGO_BYTES_HANDLES_MAX, and per actor, 1 ..
 *                                     bytes_handles: the key is "N" or "N/M" (M
 *                                     omitted: the default's, at most N); keys
 *                                     hold 15 characters (a board's NVS), so not
 *                                     vm.bytes_handles
 *
 * Integer keys are decimal; size keys (bytes) take an optional K, M or G
 * suffix (either case). A key value of 0 is the default, except
 * vm.memory and code.mem (0 is no limit, and no module code). Pure C99,
 * no state: the platform keeps its VmParams. */
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
 * the default actor limit, the memory limit, the port and link caps, the
 * code memory limit and the Bytes caps. 0, or -1 if the Vm refuses the
 * port cap (vm_set_max_ports). */
int vm_params_apply(struct Vm *vm, const VmParams *p);

/* The memory limit, in bytes, a node agent of distributed actors should
 * have on vm (lib/node spawns it so, and refuses new boards past three
 * quarters of it): its state grows with the nodes it knows, so it derives
 * from the link cap and vm.actor_memory: the most of 256 KB (what it had
 * before, and a board's ceiling), the VM's default actor memory and
 * NODE_AGENT_LINK_BYTES per link plus NODE_AGENT_BASE, at most the actor
 * ceiling (GOGO_ACTOR_MAX_WORDS). sys.Config answers it for the name
 * "node.agent_memory" (longer than any key the store holds), so lib/node
 * reads it with no image format change; an older VM answers nothing and
 * the agent keeps 256 KB. */
#define NODE_AGENT_LINK_BYTES 320u
#define NODE_AGENT_BASE 65536u
uint32_t vm_node_agent_bytes(const struct Vm *vm);

/* v as 16K / 1M / 2G when it divides evenly, else in bytes, at p (no
 * NUL); returns the end. At most 20 characters. */
char *vm_params_put_size(char *p, uint64_t v);

#endif
