/* Ports: peripherals as special actors with a PID (spec M3 §5.1-5.2).
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
 * pushing PortEvent{index, gen, a, b, time_ms} to the event ring (evq.h)
 * and calling platform_notify(). The scheduler drains the ring (port_drain)
 * and sends the owner an `event_type` struct {a, b} as an ordinary message,
 * subject to its mailbox quota. Events whose (index, gen) no longer names
 * an open port are dropped.
 *
 * Debounce (per port, debounce_ms set by the driver in open, 0 = off): an
 * event at least debounce_ms after the last delivery is delivered at once;
 * one inside the window is held as the pending trailing value (the latest
 * wins). When the window ends, the pending value is delivered if its b
 * differs from the last delivered b, and that delivery opens a new window.
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
    int  (*open)(Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
    /* A command struct of reserved type `type` whose fields are all int. */
    void (*command)(Port *p, uint32_t type, const int32_t *f, uint32_t n);
    void (*close)(Port *p);
    bool (*can_event)(const Port *p);   /* may still produce events */
} PortDriver;

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
};

term port_open(struct Vm *vm, struct Actor *owner, term name, term cfg);   /* port PID, or T_NIL */
bool port_is(term pid);
/* Hands the struct msg (read in place on `from`'s heap: no copy, no
 * mailbox, no quota) to the port's driver. A message that is not a struct
 * of at most PORT_MAX_FIELDS ints, and a stale or non-port pid, are
 * ignored. */
void port_command(struct Vm *vm, const struct Actor *from, term pid, term msg);
void port_close(struct Vm *vm, term pid);                /* stale ignored */
void port_close_owned(struct Vm *vm, term owner);        /* O(1) when owner has no ports */
void port_close_all(struct Vm *vm);                      /* VM shutdown: close every open port */
void port_drain(struct Vm *vm, uint64_t now);            /* ring -> messages, debounce */
bool port_next_deadline(const struct Vm *vm, uint64_t *deadline);   /* pending trailing debounce */
bool port_any_live(const struct Vm *vm);
void port_set_drivers(const PortDriver *const *list);        /* tests; NULL = platform list */

#endif
