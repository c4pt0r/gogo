/* The VM's run-time parameters: one model for every platform (board
 * firmware, gogovm, libgogo). Not VM core: the platforms link this file
 * (the board firmware and gogovm beside board_main.c, libgogo in
 * libgogo.a) and hand the VM only the resolved numbers, which
 * vm_params_apply stores in the Vm's fields after vm_init. Each has a build default (config.h, or the
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
 *                                     (else cut to what the actors leave, said
 *                                     when the ports were given)
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
#include <stdbool.h>
#include "host.h"
#include "config.h"

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

/* The keys, in this order (params_overlay's texts, params_changed). */
enum { PARAM_ACTOR_MEMORY, PARAM_MEMORY, PARAM_MAX_ACTORS, PARAM_MAX_PORTS, PARAM_MAX_LINKS, PARAM_CODE_MEM,
       PARAM_HANDLES, PARAM_COUNT };

/* The numbers keys parse into: 32 bits in the board configuration (no
 * board value passes 4 GB: no 64-bit arithmetic there), 64 on the host;
 * a parse saturates at PARAMS_NUM_TOP. */
#if GOGO_WORDS_16
typedef uint32_t params_num;
#define PARAMS_NUM_TOP 0xFFFFFFFFu
#else
typedef uint64_t params_num;
#define PARAMS_NUM_TOP ((uint64_t)1 << 48)
#endif

/* The build's defaults (config.h); a platform then sets its own where it
 * differs (gogovm: 1024 ports, 512 links). */
void vm_params_default(VmParams *p);

/* Resolves p in place: on entry p holds the platform's defaults; each
 * field becomes its key's value (h->config_get with ctx, when h has one)
 * when set, good and not 0 (vm.memory and code.mem: 0 too), else stays
 * the default, then is checked (above). A flag or gogo_config field beats
 * the key because the platform answers it first in its config_get
 * (params_overlay). Lines go to h->write(ctx, 2, ...). */
void vm_params_resolve(const gogo_host *h, void *ctx, VmParams *p);

/* For a platform's config_get: answers key k (kl bytes) from text[PARAM_*]
 * (NULL: not given), as the store would (its length, the bytes into buf,
 * at most cap); -1 for any other key or one not given. gogovm gives its
 * flags as typed; libgogo its gogo_config fields in decimal ("/M" for
 * bytes_per_actor alone: vm.handles' M with its N kept). */
int32_t params_overlay(const char *const text[PARAM_COUNT], const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap);

/* The keys whose values differ between a and b, space-separated, into buf
 * (as vm_params_format); returns the whole length. */
size_t params_changed(const VmParams *a, const VmParams *b, char *buf, size_t cap);

/* Digits at s (n bytes), then with size an optional K, M or G (either
 * case), into *out (saturating at PARAMS_NUM_TOP): the bytes used, 0 if
 * not a number. Shared with the board's other size keys (fs.flash.size). */
uint32_t params_parse(const char *s, uint32_t n, bool size, params_num *out);

/* Applies resolved p to a Vm fresh from vm_init (its table: p->max_actors):
 * the default actor limit, the memory limit, the port and link caps, the
 * code memory limit and the Bytes caps. 0, or -1 if the Vm refuses the
 * port cap (vm_set_max_ports). */
int vm_params_apply(struct Vm *vm, const VmParams *p);

/* The memory limit, in bytes, a node agent of distributed actors should
 * have with parameters p (lib/node spawns it so, and refuses new boards
 * past three quarters of it): its state grows with the nodes it knows, so
 * it derives from the link cap and the actor memory: the most of 256 KB
 * (what it had before, and a board's ceiling), p->actor_memory and
 * NODE_AGENT_LINK_BYTES per link plus NODE_AGENT_BASE, at most the actor
 * ceiling (GOGO_ACTOR_MAX_WORDS). gogovm's config_get (platform/unix/main.c)
 * answers it for the name NODE_AGENT_KEY, "node.agent_memory" (longer
 * than any key the store holds), so lib/node reads it with sys.Config, no
 * image format change; a VM that does not answer (a board, whose ceiling
 * is 256 KB anyway; an older gogovm) leaves the agent at 256 KB. */
#define NODE_AGENT_KEY "node.agent_memory"
#define NODE_AGENT_LINK_BYTES 320u
#define NODE_AGENT_BASE 65536u
uint32_t params_node_agent_bytes(const VmParams *p);

/* p as one line of space-separated name=value fields, sizes as K / M / G
 * when they divide evenly, for `gogovm --info` and the board's `gogo
 * device info`:
 *   actor_memory=16K memory=160K max_actors=1024 max_ports=16 max_links=8
 *   code_mem=64K bytes_handles=64/16
 * (memory: the arena, GOGO_ARENA_BYTES, when there is no VM limit or a
 * larger one; "unlimited" when neither; max_links 0 as its default).
 * Writes at most cap - 1 bytes and a NUL; returns the length of the whole
 * line (as snprintf). */
size_t vm_params_format(const VmParams *p, char *buf, size_t cap);

#endif
