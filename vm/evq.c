#include "evq.h"
#include "config.h"
#include "platform.h"

#if (GOGO_EVQ_CAP) < 2u || ((GOGO_EVQ_CAP) & ((GOGO_EVQ_CAP) - 1u)) != 0u
#error "GOGO_EVQ_CAP must be a power of two"
#endif
#define EVQ_MASK ((GOGO_EVQ_CAP) - 1u)

static PortEvent slots[GOGO_EVQ_CAP];
/* Free-running indices (wrap mod 2^32; CAP divides 2^32). head: producer
 * only. tail: consumer only. dropped: producer only (consumer reads). */
static volatile uint32_t head, tail;
static volatile uint32_t dropped;

bool evq_push(const PortEvent *e) {
    uint32_t h = head;
    if (h - tail >= GOGO_EVQ_CAP) {      /* full: drop the new event */
        dropped = dropped + 1u;
        return false;
    }
    slots[h & EVQ_MASK] = *e;
    platform_barrier();                  /* slot visible before the index */
    head = h + 1u;
    return true;
}

bool evq_pop(PortEvent *out) {
    uint32_t t = tail;
    if (t == head) return false;
    platform_barrier();                  /* index read before the slot */
    *out = slots[t & EVQ_MASK];
    platform_barrier();                  /* slot copied before it is freed */
    tail = t + 1u;
    return true;
}

bool evq_empty(void) { return tail == head; }

uint32_t evq_dropped(void) { return dropped; }

void evq_reset(void) {
    head = 0; tail = 0; dropped = 0;
}
