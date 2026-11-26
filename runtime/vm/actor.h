/* Actors, the PID table and the run queue.
 *
 * Each actor owns one memory block of mem_words 32-bit words: the heap
 * grows up from word 0 (hp = next free word) and the stack grows down from
 * the top (sp = lowest used word; empty stack has sp == mem_words). */
#ifndef GOGO_ACTOR_H
#define GOGO_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "term.h"
#include "image.h"
#include "config.h"
#include "port.h"
#include "alloc.h"
#include "evq.h"
#include "host.h"

typedef struct Msg Msg;   /* defined with the mailbox */
typedef struct Timer Timer;   /* defined in timer.h */
typedef struct Name Name;     /* defined in names.c */
typedef struct HeldBytes HeldBytes;   /* defined in bytes.h */
typedef struct ModEntry ModEntry;     /* defined in module.h */
struct BytesEntry;                    /* defined in bytes.c */

/* Field order keeps the struct free of padding on both 32- and 64-bit
 * hosts: 48 bytes on 32-bit (checked in check32.c), 72 on 64-bit. */
typedef struct Actor {
    struct Actor *next;       /* run queue link */
    uint32_t *mem;            /* memory block */
    uint16_t mem_words, hp, sp, fp;
    const uint32_t *pc;
    Msg *mbox_head;
    Msg *mbox_tail;
    void *extra;
    uint32_t id;              /* PID term */
    uint8_t state, flags;
    uint16_t reductions;
    uint16_t mbox_words, max_words;
    uint8_t mod;              /* module slot whose code it runs (module.h); counted in its live */
    uint8_t pad8;
    uint16_t pad16;           /* keeps 48 B on 32-bit */
} Actor;

enum { A_RUNNABLE = 1, A_WAITING = 2, A_EXITED = 3 };

/* Actor.flags bits. */
enum {
    AF_EXIT_PENDING = 1,  /* marked by actor_mark_exit; code in extra */
    AF_TRAP_EXIT = 2,     /* exit signals arrive as ExitMsg (sig_set_trap) */
    AF_WAKEUP_ARMED = 4,  /* a ReceiveTimeout/Sleep wakeup is pending (timer.h) */
    AF_TIMED_OUT = 8,     /* that wakeup fired; the builtin has not seen it yet */
    AF_CALL_PENDING = 16  /* a Call sent its request and awaits the reply (interp.c) */
};

struct Out;

/* Distribution hooks (runtime/dist/, design.md §5.13): set by the "node"
 * port driver when a program opens its first node link, NULL before (and
 * always in builds without it). The core calls them only for a proxy PID
 * (term_is_proxy) that names no local actor, and on every actor exit.
 *  - send: Send (serial 0) or a Call reply (serial: the Ref's) to a remote
 *    PID; returns as msg_send (0 sent or dropped, or a negated exit reason
 *    of the sender: R_BAD_MSG for a value that may not leave the node).
 *  - monitor: a's monitor `serial` of remote pid (false: out of memory).
 *  - link: link (on) or unlink a and remote pid (false: out of memory).
 *  - demonitor: a dropped monitor `serial` (any serial; unknown ignored).
 *  - exited: a is exiting with code (its remote links and monitors).
 *  - print: writes a proxy PID's number (println, "<pid N@node>").
 *  - kill: actor.Kill of a remote pid by `from` (false: out of memory).
 * SendAfter to a remote PID is BadMsg (no remote timers). */
typedef struct VmDist {
    int  (*send)(struct Vm *vm, struct Actor *from, term to, uint64_t serial, term msg);
    bool (*monitor)(struct Vm *vm, struct Actor *a, term pid, uint64_t serial);
    bool (*link)(struct Vm *vm, struct Actor *a, term pid, bool on);
    void (*demonitor)(struct Vm *vm, struct Actor *a, uint64_t serial);
    void (*exited)(struct Vm *vm, struct Actor *a, uint32_t code);
    void (*print)(struct Out *o, const struct Vm *vm, term pid);
    bool (*kill)(struct Vm *vm, struct Actor *from, term pid, uint32_t code);
} VmDist;

/* vm->dist if pid is a remote (proxy) PID and the VM has distribution,
 * else NULL: the one test before every hook call but exited (actor.c). */
const VmDist *vm_dist_of(const struct Vm *vm, term pid);

/* PID table: a live slot holds its Actor*; a free slot holds
 * (gen << 1) | 1, where gen is the 12-bit generation the next actor in
 * that slot will get. Actor pointers are aligned, so bit 0 tells them apart. */
typedef struct Vm {
    /* First: every host.c wrapper loads these (offsets in short-load range). */
    const gogo_host *host;    /* the host's hooks (host.h wrappers only) */
    void *host_ctx;           /* their ctx */
    const VmBoard *board;     /* board hardware; never NULL after vm_init */
    ModEntry *mods[GOGO_MAX_MODULES];   /* module table (module.h); NULL = free slot */
    uint16_t mod_gen[GOGO_MAX_MODULES]; /* each slot's current generation */
    uint32_t mod_mem;         /* code bytes of the counted (loaded, not boot) modules */
    uint32_t mod_cap;         /* their limit: config code.mem (module_config) */
    uint8_t mod_next;         /* where module_load's round-robin slot search starts */
    uint8_t open_err;         /* why the last refused port open failed: GOGO_E_* (port.h) */
    uint32_t rand_x;          /* vm_random's fallback state */
    uint64_t t0;              /* host->now_ms at the time base; vm_now counts from it */
    uintptr_t *table;
    uint16_t table_cap;
    uint16_t next_free;       /* every slot below it is taken: actor_new_in scans from it */
    Actor *runq_head, *runq_tail;
    uint32_t live;            /* actors currently allocated */
    uint64_t next_ref;        /* serial of the next Ref, from 1 */
    Timer **timers;           /* min-heap of pending timers, by (deadline, seq) (timer.h) */
    uint32_t ntimers;         /* entries in use */
    uint32_t timers_cap;      /* entries allocated; the array is freed when empty */
    uint64_t timer_seq;       /* next insertion sequence number (tie-break) */
    uint32_t timer_scanned;   /* heap entries timer_cancel_actor has visited (tests) */
    Name *names;              /* registered names (names.h) */
    struct BytesEntry *bytes_tab; /* off-heap Bytes handle table (bytes.h), NULL until first use */
    uint32_t bytes_used;      /* its live entries */
    void *reserve;            /* emergency reserve (signals.h), NULL while used up */
    bool reserve_wanted;      /* between vm_reserve_init and _release */
    bool wall_known;          /* wall clock set (walltime.c) */
    uint16_t ports_pending;   /* ports holding a trailing debounce value */
    uint8_t run_state;        /* vm_start / vm_step / vm_stop: VM_ST_* (interp.h) */
    int64_t wall_offset;      /* unix ms - host->now_ms at the last set (no RTC) */
    /* Event ring indices (evq.h): head producer-only (ISR), tail consumer-only. */
    volatile uint32_t evq_head, evq_tail, evq_dropped;
    const PortDriver *const *drivers;   /* port_set_drivers; NULL = board->ports */
    PortSrcSlot sources[GOGO_MAX_SOURCES + 1u];   /* port_add_source; s NULL-terminated */
    /* B_FS (vfs_init; NULL = no storage linked: bad code). */
    uint32_t (*fs_builtin)(struct Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs);
    struct VfsState *vfs;     /* vfs mounts and open files (vfs_init); NULL = none */
    struct InputState *input; /* input port state (input_install); NULL = none */
    struct NetState *net;     /* the net stack (net_install, runtime/net/); NULL = none */
    const VmDist *dist;       /* distribution hooks (runtime/dist/); NULL = none */
    VaHeap va;                /* allocator (alloc.h): scalars, then its free lists */
    /* The port table (port.h), in chunks of PORT_CHUNK that never move
     * (drivers keep Port pointers): port_n ports exist (none until the
     * first open), at most port_max; index i is slot port_base + i. The
     * first chunk, and the chunk list of a table of one chunk (a board's
     * 16), live here, as the fixed table did: no port takes arena memory
     * on a board. */
    Port **port_chunk;
    Port *port_chunk1;        /* port_chunk's storage when it has one entry */
    uint16_t port_n, port_max, port_base;
    Port port_first[PORT_CHUNK];
    uint16_t max_links;       /* node links of distribution (runtime/dist); 0 = its default */
    PortEvent evq_slots[GOGO_EVQ_CAP];   /* event ring storage (evq.h), last: large */
} Vm;

/* Port i (i < vm->port_n) and the index of port p. */
static inline Port *port_at(const Vm *vm, uint32_t i) {
    return &vm->port_chunk[i / PORT_CHUNK][i % PORT_CHUNK];
}
static inline uint32_t port_index(const Vm *vm, const Port *p) { return term_pid_slot(p->id) - vm->port_base; }

/* One-line accessors, inline (smaller than a call); declared here, after
 * Vm, though they belong to evq.h, host.h and port.h. */
static inline bool evq_empty(const Vm *vm) { return vm->evq_tail == vm->evq_head; }
static inline uint32_t evq_dropped(const Vm *vm) { return vm->evq_dropped; }
static inline void vm_chunk_free(Vm *vm, void *p, size_t n) { vm->host->free(vm->host_ctx, p, n); }   /* p never NULL */
static inline void port_set_drivers(Vm *vm, const PortDriver *const *list) { vm->drivers = list; }
static inline void vm_wake(const Vm *vm) {
    if (vm->host->wake) vm->host->wake(vm->host_ctx);
}

/* Clears vm and ties it to its host (host_ctx goes to every hook) and
 * board (NULL = no board hardware). Then allocates the PID table. Returns 0, or
 * -1 if the table cannot be allocated or table_cap exceeds the default
 * port_base (PORT_SLOT_TOP - GOGO_MAX_PORTS: the slots above it name ports). Boot code then calls vm_boot; tests
 * install a hand-built image with module_install_static. */
int vm_init(Vm *vm, const gogo_host *host, void *host_ctx, const VmBoard *board, uint16_t table_cap);

/* Slot + PID assigned, block allocated, empty stack, state A_RUNNABLE,
 * max_words 0 (default limit), not enqueued, running module slot `mod`'s
 * code (counted in that module's live actors). NULL if the table is full
 * or memory runs out. */
Actor *actor_new_in(Vm *vm, uint16_t initial_words, uint8_t mod);

/* actor_new_in(vm, initial_words, 0): an actor of module 0 (tests, C
 * helpers); actors spawned by code inherit their spawner's module. */
Actor *actor_new(Vm *vm, uint16_t initial_words);

/* The actor's memory limit in words (heap block + queued messages):
 * max_words, or GOGO_DEFAULT_MAX_WORDS when max_words is 0. */
uint32_t actor_max_words(const Actor *a);

/* Frees the queued messages, the extra block, the memory block and the
 * slot, and bumps the slot's generation. The actor must not be on the run
 * queue. Low level: a live actor dies through actor_exit (signals.h); this
 * is called directly only for an actor that never ran. */
void actor_free(Vm *vm, Actor *a);

/* NULL if pid is not a PID, names a free slot, or has a stale generation. */
Actor *actor_lookup(Vm *vm, term pid);

/* Like actor_lookup, but also NULL for an actor that exited or is marked
 * to exit: one that may still get messages and signals. */
Actor *actor_lookup_live(Vm *vm, term pid);

void runq_push(Vm *vm, Actor *a);
Actor *runq_pop(Vm *vm);          /* NULL when empty */

#endif
