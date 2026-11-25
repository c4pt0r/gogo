#include "evq.h"
#include "actor.h"
#include "config.h"
#include "host.h"

#if (GOGO_EVQ_CAP) < 2u || ((GOGO_EVQ_CAP) & ((GOGO_EVQ_CAP) - 1u)) != 0u
#error "GOGO_EVQ_CAP must be a power of two"
#endif
#define EVQ_MASK ((GOGO_EVQ_CAP) - 1u)

/* Vm.evq_head, evq_tail: free-running indices (wrap mod 2^32; CAP divides
 * 2^32). head: producer only. tail: consumer only. evq_dropped: producer
 * only (consumer reads). All three volatile; Vm.evq_slots holds the events. */

bool evq_push(Vm *vm, const PortEvent *e) {
    uint32_t h = vm->evq_head;
    if (h - vm->evq_tail >= GOGO_EVQ_CAP) {   /* full: drop the new event */
        vm->evq_dropped = vm->evq_dropped + 1u;
        return false;
    }
    vm->evq_slots[h & EVQ_MASK] = *e;
    vm_barrier();                  /* slot visible before the index */
    vm->evq_head = h + 1u;
    return true;
}

bool evq_pop(Vm *vm, PortEvent *out) {
    uint32_t t = vm->evq_tail;
    if (t == vm->evq_head) return false;
    vm_barrier();                  /* index read before the slot */
    *out = vm->evq_slots[t & EVQ_MASK];
    vm_barrier();                  /* slot copied before it is freed */
    vm->evq_tail = t + 1u;
    return true;
}

bool evq_empty(const Vm *vm) { return vm->evq_tail == vm->evq_head; }

uint32_t evq_dropped(const Vm *vm) { return vm->evq_dropped; }

void evq_reset(Vm *vm) {
    vm->evq_head = 0; vm->evq_tail = 0; vm->evq_dropped = 0;
}
