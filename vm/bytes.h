/* Off-heap bytes.Bytes: the handle table, its holders and the C API for
 * ports (spec M4 §5.2, §9.5, §9.7, §9.8; M5 §9.2, §9.6).
 *
 * A Bytes value is T_EMPTYBYTES, a heap K_BYTES of any size, or a handle
 * term (index << 4) | 0xE naming an entry of one VM-wide table of
 * GOGO_BYTES_HANDLES entries {refcnt, off, len, parent, data}. Only
 * Buffer.Freeze (more than GOGO_BYTES_INLINE bytes), Bytes.Slice of a
 * handle and the C API make handles. A root entry owns its data block
 * (len bytes from the VM allocator); a slice entry shares its root's block
 * at [off, off + len) and holds one reference on the root (parent), so the
 * root outlives it. A slice of a slice names the root directly: parent
 * chains are one deep.
 *
 * References (refcnt) come from:
 *  - holders: every actor has a list (at most GOGO_BYTES_PER_ACTOR entries,
 *    allocated on first use, reached through its ActorExtra) of the handles
 *    it holds, one reference per entry. An entry is there because the
 *    actor's registers or heap may reach the handle (HELD_HEAP: Freeze,
 *    Slice, a received message, a spawn capture) and/or because C code (a
 *    port) pinned it on the actor's behalf (pins: bytes_new, bytes_retain);
 *  - message blocks: one reference per handle occurrence in the block, from
 *    msg_build until msg_free (msg.c), so a queued or SendAfter-pending
 *    message keeps its handles alive whatever its sender does;
 *  - slice entries: one each on their root.
 *
 * Charging (all holders in full, spec §8.2): a holder is charged the length
 * of each distinct root among its entries (a slice charges its root's
 * length, since it keeps the root alive) as held_bytes, counted in
 * actor_charged_words. A queued message is charged to its receiver's
 * mailbox usage for the distinct roots it carries (msg_charge_words), so
 * the M2 quota applies on enqueue (MailboxOverflow); a pending SendAfter's
 * message is charged to its creator the same way.
 *
 * GC (heap.c): a collection of an actor marks the held entries its stack,
 * roots and live heap objects reach; afterwards an entry that is neither
 * reached nor pinned is dropped. A dropped Bytes is thus released at its
 * holder's next GC. Exit drops every entry (bytes_actor_exit).
 *
 * The table is allocated on first use and then kept (it is fixed memory,
 * GOGO_BYTES_HANDLES entries, and keeping it avoids free/realloc churn on
 * the MCU arena when handles are made and dropped in a loop): a program
 * without off-heap Bytes pays nothing. It is process-global, like the
 * allocator: one Vm at a time (vm_init resets it).
 *
 * Table full: bytes_make / bytes_sub first collect the caller; if the table
 * is still full they collect every other actor that holds handles (idle
 * actors never collect on their own) and retry once more. */
#ifndef GOGO_BYTES_H
#define GOGO_BYTES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"
#include "config.h"

/* One entry of an actor's holder list. */
typedef struct Held {
    uint16_t idx;             /* table index */
    uint8_t pins;             /* C API references (bytes_new, bytes_retain) */
    uint8_t flags;            /* HELD_HEAP, HELD_MARK */
} Held;

enum {
    HELD_HEAP = 1,            /* the actor's registers or heap may reach it */
    HELD_MARK = 2             /* reached by the GC in progress */
};

/* An actor's holder list, allocated on its first handle and freed when it
 * holds none; its ActorExtra points to it (actor_held). While it exists its
 * size is charged to the actor (ActorExtra.charged_words). */
struct HeldBytes {
    uint32_t n;               /* entries in use */
    uint32_t bytes;           /* held_bytes: lengths of the distinct roots held */
    Held e[GOGO_BYTES_PER_ACTOR];
};

/* ---- C API for ports (M5) ---- */

/* A new root of len bytes (zeroed) charged to owner and pinned on its
 * behalf (released with bytes_release); *data is where to write it. T_NIL
 * if the table is full, owner already holds GOGO_BYTES_PER_ACTOR handles,
 * the bytes would take owner past its limit, or memory runs out. Never
 * collects. */
term bytes_new(Vm *vm, Actor *owner, uint32_t len, uint8_t **data);

/* Pins handle h on holder's behalf (one more C reference, charged to
 * holder in full). False if h is not a live handle or holder cannot take
 * another entry (cap, memory); nothing changes then. The limit is not
 * checked: a port's buffers are its owner's, as Handoff moves them. */
bool bytes_retain(Vm *vm, Actor *holder, term h);

/* Undoes one bytes_new / bytes_retain of h by holder. When no pin is left
 * and the holder's code cannot reach h (it never had it, or a GC since
 * found it unreachable), the entry goes at once and the charge with it;
 * otherwise it stays until the holder's next GC. */
void bytes_release(Vm *vm, Actor *holder, term h);

/* The bytes of handle h, *len of them; NULL if h is not a live handle. The
 * block never moves while referenced. */
const uint8_t *bytes_data(term h, uint32_t *len);

/* The bytes of any Bytes value as seen by actor a: a handle, a heap or
 * const K_BYTES (valid until a's next allocation) or T_EMPTYBYTES (len 0,
 * NULL). False for anything else. */
bool bytes_view(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len);

/* Drops all of a's entries (pins included) and their charge. Called from
 * actor_extra_free, so on every exit and for actors freed unrun. */
void bytes_actor_exit(Vm *vm, Actor *a);

/* ---- VM internals ---- */

/* Makes a root of len bytes held by a (HELD_HEAP) for Buffer.Freeze; *data
 * is its block (uninitialized). If the table, a's list or a's limit has no
 * room, collects a's heap once (heap_gc, no roots: the caller keeps its
 * terms in registers and re-derives heap pointers) and retries; if the
 * table is still full, collects the other actors holding handles (never a:
 * its block does not move) and retries once more. T_NIL if it still fails
 * (the caller exits with OutOfMemory). */
term bytes_make(Vm *vm, Actor *a, uint32_t len, uint8_t **data);

/* The slice [lo, lo + len) of handle h (held by a) as a new entry sharing
 * h's root, held by a; collects and retries like bytes_make. T_NIL on
 * failure. */
term bytes_sub(Vm *vm, Actor *a, term h, uint32_t lo, uint32_t len);

/* True if h names a live entry. */
bool bytes_valid(term h);

/* GC (heap.c): where a's list is if it has one, else NULL. begin clears the
 * marks, mark records that h was reached, sweep drops what was neither
 * reached nor pinned (run only after a successful collection). */
HeldBytes **bytes_gc_begin(Actor *a);
void bytes_gc_mark(HeldBytes **slot, term h);
void bytes_gc_sweep(Actor *a, HeldBytes **slot);

/* Words taking a first handle would add to a's charge: the holder list's
 * size if a has none yet, else 0 (msg_enqueue counts it in the quota for a
 * message carrying handles, so the overflow is MailboxOverflow on enqueue
 * rather than OutOfMemory after the receive). */
uint32_t bytes_list_words(const Actor *a);

/* Message references (msg.c): one per occurrence. */
void bytes_msg_ref(term h);
void bytes_msg_unref(term h);

/* a received h in a message or spawn capture: a holds it (HELD_HEAP)
 * unless it already does. False if a's list is full or memory runs out.
 * Charged even past a's limit: the mailbox already counted it. */
bool bytes_adopt(Actor *a, term h);

/* Sets of distinct roots, for charging: add each handle, then read the
 * bytes of the distinct roots seen. */
typedef struct BytesSet {
    uint32_t seen[(GOGO_BYTES_HANDLES + 31u) / 32u];
    uint32_t bytes;
} BytesSet;
void bytes_set_init(BytesSet *s);
void bytes_set_add(BytesSet *s, term h);

/* Words for b bytes, rounded up. */
static inline uint32_t bytes_words(uint32_t b) { return (uint32_t)(((uint64_t)b + 3u) / 4u); }

/* Live table entries (tests). */
uint32_t bytes_table_used(void);

/* a's held_bytes and number of held entries (tests, Info). */
uint32_t bytes_held_bytes(const Actor *a);
uint32_t bytes_held_count(const Actor *a);

/* Forgets the table (vm_init, tests). */
void bytes_reset(void);

#endif
