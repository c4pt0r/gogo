/* Ports: peripherals as special actors with a PID.
 *
 * A fixed table of GOGO_MAX_PORTS entries lives in the Vm. A port PID is an
 * ordinary PID term whose slot is PORT_SLOT_BASE + index and whose
 * generation is the entry's 12-bit generation, bumped on every close, so a
 * stale PID never names a newer port. vm_init keeps the actor table below
 * PORT_SLOT_BASE, so actor_lookup returns NULL for a port PID (Link and
 * Monitor treat it as dead).
 *
 * Drivers never see terms: the VM decodes the int fields of the cfg and
 * command structs into an int32 array, and a driver reports input by
 * pushing PortEvent{index, gen, a, b, time_ms} to the VM's event ring (evq.h)
 * and waking it (the board's wake, or vm_wake). time_ms is the low 32 bits
 * of the host's clock (host->now_ms, not vm_now: port_drain converts). The scheduler drains the ring (port_drain)
 * and sends the owner an `event_type` struct {a, b} as an ordinary message,
 * subject to its mailbox quota. Events whose (index, gen) no longer names
 * an open port are dropped.
 *
 * Debounce (per port, debounce_ms set by the driver in open, 0 = off): an
 * event at least debounce_ms after the last delivery is delivered at once;
 * one inside the window is held as the pending trailing value (the latest
 * wins). When the window ends, the pending value is delivered if its b
 * differs from the last delivered b, and that delivery opens a new window.
 *
 * M5 additions:
 *  - Charges: a driver sets Port.charge_words in open for the state it
 *    keeps for the port (e.g. a TCP control block in a fixed C table); the
 *    owner is charged that (open fails if it does not fit its limit) until
 *    close, and Handoff moves it. A driver whose events are large sets
 *    Port.room_64w too (64-word units): open also fails unless the owner has that much
 *    more free (checked once, not charged), so a port whose every event
 *    would be over the owner's quota is refused instead of silent.
 *  - Port-held Bytes: port_bytes_new / port_bytes_retain pin off-heap Bytes
 *    on the owner's behalf (at most PORT_MAX_HELD per port), recorded in
 *    the port so close releases them and Handoff moves them.
 *  - Handoff: the owner gives the port (charge, pinned Bytes, events) to
 *    another actor; the driver's handoff callback runs afterwards.
 *  - An extra event source (port_add_source): e.g. the net stack's
 *    single-slot RX and link handoff. The port layer folds it into the scheduler hooks
 *    (port_work_pending, port_drain, port_any_live, port_next_deadline), so
 *    vm/ does not depend on net/.
 *  - A driver that delivers a message carrying Bytes handles must build it
 *    with MSG_HANDLES set and one bytes_msg_ref per handle occurrence (or
 *    with msg_build); port_deliver_ints only sends ints (flags 0).
 */
#ifndef GOGO_PORT_H
#define GOGO_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include "term.h"

struct Vm;
struct Actor;

#define PORT_SLOT_BASE 0xFFF0u

/* Most int fields the VM decodes from a cfg or command struct. */
#define PORT_MAX_FIELDS 8u

typedef struct Port Port;
typedef struct PortDriver {
    const char *name;
    uint32_t cfg_type;      /* reserved type index the cfg struct must have */
    uint32_t event_type;    /* reserved type of event messages: struct of 2 ints (a, b) */
    /* cfg: the cfg struct's int fields in order. 0 = ok, else refuse. */
    int  (*open)(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
    /* A command struct of reserved type `type` whose fields are all int. */
    void (*command)(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);
    void (*close)(struct Vm *vm, Port *p);
    bool (*can_event)(const struct Vm *vm, const Port *p);   /* may still produce events */
    /* Optional (NULL = none): called after port_handoff gave p to a new
     * owner (or to the same one: Handoff to itself), e.g. to resume a
     * paused connection. */
    void (*handoff)(struct Vm *vm, Port *p);
    /* Optional (NULL = use `command`): takes every command message raw,
     * read in place on the sender's heap (valid until the sender's next
     * allocation; no GC happens during the call), for commands that carry
     * nested structs or Bytes (net.SendTo). */
    void (*command_term)(struct Vm *vm, const struct Actor *from, Port *p, term msg);
    /* Optional (NULL = use `open`): takes the cfg raw, for a cfg struct
     * with non-int fields (net.DialTLS's Host string). port_open calls it
     * only for a struct of type cfg_type, read in place on the owner's heap
     * (valid during the call; no GC happens in it); it returns as `open`
     * does, and the charge and room checks follow alike. port_open_with
     * (C callers, int fields) uses `open` only: it refuses a driver that
     * has none. */
    int  (*open_term)(struct Vm *vm, const struct Actor *owner, Port *p, uint16_t index, term cfg);
} PortDriver;

/* Most off-heap Bytes one port pins for its owner (a TCP connection holds
 * at most one in flight and one queued write, spec M5 §5.2; a stream/
 * StreamOut port uses both for its running and queued Out). */
#define PORT_MAX_HELD 2u

/* Port.flags bits (VM-private debounce bookkeeping). */
enum { PORT_DELIVERED = 1, PORT_PENDING = 2 };

struct Port {
    const PortDriver *drv;  /* NULL: free slot */
    term owner, id;
    uint16_t gen, debounce_ms;   /* driver sets debounce_ms in open (0 = off) */
    uint32_t state[4];      /* driver-private */
    /* debounce bookkeeping: last delivered b and time, pending trailing value */
    uint64_t last_t;        /* ms of the last delivery (valid with PORT_DELIVERED) */
    int32_t last_b;
    int32_t pend_a, pend_b; /* valid with PORT_PENDING */
    uint8_t flags;
    uint8_t room_64w;       /* driver sets in open: words the owner must have free besides
                             * the charge (one stream Chunk), in 64-word units rounded up
                             * (fits the padding); checked at open, not charged */
    uint16_t charge_words;  /* driver sets in open: words charged to the owner */
    term held[PORT_MAX_HELD];   /* Bytes pinned for the owner; 0 = free (never a handle) */
};

term port_open(struct Vm *vm, struct Actor *owner, term name, term cfg);   /* port PID, or T_NIL */
/* Opens a port of driver drv for owner with decoded cfg fields f[0..n),
 * as port_open does after decoding (a C caller: the net stack opening an
 * accepted TCP connection for its listener's owner). */
term port_open_with(struct Vm *vm, struct Actor *owner, const PortDriver *drv,
                    const int32_t *f, uint32_t n);
bool port_is(term pid);
/* Hands the struct msg (read in place on `from`'s heap: no copy, no
 * mailbox, no quota) to the port's driver: raw to command_term if the
 * driver has one, else decoded; a message that is not a struct of at most
 * PORT_MAX_FIELDS ints is then ignored. A stale or non-port pid is
 * ignored. */
void port_command(struct Vm *vm, const struct Actor *from, term pid, term msg);
void port_close(struct Vm *vm, term pid);                /* stale ignored */
void port_close_owned(struct Vm *vm, term owner);        /* O(1) when owner has no ports */
void port_close_all(struct Vm *vm);                      /* VM shutdown: close every open port */
void port_drain(struct Vm *vm, uint64_t now);            /* ring -> messages, debounce */
bool port_next_deadline(const struct Vm *vm, uint64_t *deadline);   /* pending trailing debounce */
bool port_any_live(const struct Vm *vm);
void port_set_drivers(struct Vm *vm, const PortDriver *const *list);   /* NULL = board->ports; after vm_init */

/* Gives port pid to actor `to` if `from` owns it: the charge, the pinned
 * Bytes (re-pinned for `to`, released for `from`) and later events move;
 * messages already queued stay where they are. Then calls the driver's
 * handoff callback. False (nothing changes) if pid is not an open port
 * owned by from, `to` is not a live actor (or is marked to exit), or `to`
 * cannot take the charge and the pinned Bytes within its limit. Handoff to
 * from itself only calls the callback. */
bool port_handoff(struct Vm *vm, struct Actor *from, term pid, term to);

/* The port's owner if it is alive and not marked to exit, else NULL. */
struct Actor *port_owner(struct Vm *vm, const Port *p);

/* Port-held Bytes (bytes.h C API on the owner's behalf, recorded in p):
 * _new makes a pinned root of len bytes (T_NIL if p holds PORT_MAX_HELD
 * already, the owner is exiting, or bytes_new fails); _retain pins handle
 * h (false if full, the owner is exiting, or bytes_retain fails); _release undoes one of them (ignored if p does not
 * hold h). port_close releases whatever is left. */
term port_bytes_new(struct Vm *vm, Port *p, uint32_t len, uint8_t **data);
bool port_bytes_retain(struct Vm *vm, Port *p, term h);
void port_bytes_release(struct Vm *vm, Port *p, term h);

/* An extra event source drained by the scheduler, e.g. the
 * net stack's RX slot and link slot. pending: cheap check, true when drain
 * has work it can do NOW (it may be called from the scheduler at any time);
 * drain: runs on the scheduler thread inside port_drain; any_live: keeps
 * the VM running; next_deadline (optional): an absolute vm_now
 * deadline folded into the idle wait. Each callback gets the ctx given to
 * port_add_source. Up to GOGO_MAX_SOURCES per Vm, visited in no fixed
 * order.
 *
 * The rule pending must keep: the scheduler never sleeps while any
 * pending() is true (port_due_now), it drains again at once. So pending
 * may be true only when the next drain will make progress (consume an
 * event, advance a sliced step), and must turn false once drain has done
 * what it can now. Work that waits for time (a retry, a poll, a backoff)
 * belongs in next_deadline, with pending false until it is due. A pending
 * that stays true while drain makes no progress (a hardware flag stuck
 * set, an error retried at once forever) spins the CPU at 100%: such a
 * source must count those passes and back off (as kbd does for a failing
 * read or an INT line stuck low). */
typedef struct PortSource {
    bool (*pending)(const struct Vm *vm, void *ctx);
    void (*drain)(struct Vm *vm, void *ctx, uint64_t now);
    bool (*any_live)(const struct Vm *vm, void *ctx);
    bool (*next_deadline)(const struct Vm *vm, void *ctx, uint64_t *deadline);
    /* Optional: builtin net.Subscribe for actor a (false = refused). */
    bool (*subscribe)(struct Vm *vm, void *ctx, struct Actor *a);
} PortSource;
/* A registered source: Vm.sources[], terminated by s == NULL. */
typedef struct PortSrcSlot {
    const PortSource *s;
    void *ctx;
} PortSrcSlot;
/* Registers s with ctx on vm (after vm_init, which starts the list empty);
 * false when GOGO_MAX_SOURCES are in. s must not already be registered;
 * removal does not keep the registration order. */
bool port_add_source(struct Vm *vm, const PortSource *s, void *ctx);
void port_remove_source(struct Vm *vm, const PortSource *s);   /* unknown is ignored */

/* Sends p's owner struct `type` with n <= PORT_MAX_FIELDS int fields (bit i
 * of bool_mask set: field i is a bool, true when v[i] != 0). Non-fatal:
 * false, nothing charged, when the owner is gone or exiting, memory is out,
 * or its quota is full. */
bool port_deliver_ints(struct Vm *vm, const Port *p, uint32_t type, const int32_t *v,
                       uint32_t n, uint32_t bool_mask);

/* net.Subscribe through the source; false without one. */
bool port_subscribe(struct Vm *vm, struct Actor *a);

/* True when port_drain has work: a queued event, a pending trailing
 * debounce value, or a pending source. */
bool port_work_pending(const struct Vm *vm);

/* True when port_drain has work due NOW: a queued event or a pending
 * source. Unlike port_work_pending it leaves out the trailing debounce
 * values, which are due only at their deadline (port_next_deadline): the
 * scheduler checks this before it sleeps, so a held trailing value does not
 * keep it spinning through the debounce window. */
bool port_due_now(const struct Vm *vm);

#endif
