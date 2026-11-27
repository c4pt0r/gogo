#include "actor.h"
#include "bytes.h"
#include "alloc.h"
#include "msg.h"
#include "config.h"
#include "signals.h"
#include "module.h"
#include <string.h>

#define GEN_MASK 0xFFFu

static uintptr_t free_slot(uint16_t gen) { return ((uintptr_t)(gen & GEN_MASK) << 1) | 1u; }
static bool slot_is_free(uintptr_t w) { return (w & 1u) != 0; }
static uint16_t slot_gen(uintptr_t w) { return (uint16_t)((w >> 1) & GEN_MASK); }

/* Generation for the next actor of slot `slot` after generation gen: slot 0
 * never gets generation 0, so no actor's PID is T_ZEROPID (term.h). */
static uint16_t next_gen(uint16_t slot, uint16_t gen) {
    uint16_t g = (uint16_t)((gen + 1u) & GEN_MASK);    /* wraps at 12 bits */
    return (slot == 0 && g == 0) ? 1u : g;
}

static const VmBoard no_board;

int vm_init(Vm *vm, const gogo_host *host, void *host_ctx, const VmBoard *board, uint16_t table_cap) {
    memset(vm, 0, sizeof *vm);
    vm->host = host;
    vm->host_ctx = host_ctx;
    if (!board) board = &no_board;
    vm->board = board;
    vm->port_max = GOGO_MAX_PORTS;
    vm->port_base = (uint16_t)(PORT_SLOT_TOP - GOGO_MAX_PORTS);
    if (table_cap > vm->port_base) return -1;
    vm->next_ref = 1;
    vm->mod_cap = GOGO_CODE_MEM_DEFAULT;   /* vm_params_apply may change it */
    vm->table = va_alloc(vm, (size_t)table_cap * sizeof(uintptr_t));
    if (!vm->table) return -1;
    vm->table_cap = table_cap;
    for (uint16_t i = 0; i < table_cap; i++) vm->table[i] = free_slot(0);
    if (table_cap) vm->table[0] = free_slot(1);      /* see next_gen */
    return 0;
}

Actor *actor_new(Vm *vm, uint16_t initial_words) { return actor_new_in(vm, initial_words, 0); }

Actor *actor_new_in(Vm *vm, uint16_t initial_words, uint8_t mod) {
    uint16_t slot = vm->next_free;     /* the lowest free slot, as a scan from 0 finds */
    while (slot < vm->table_cap && !slot_is_free(vm->table[slot])) slot++;
    vm->next_free = slot;
    if (slot == vm->table_cap) return NULL;

    Actor *a = va_alloc(vm, sizeof(Actor));
    if (!a) return NULL;
    memset(a, 0, sizeof *a);
    a->mem = va_alloc(vm, (size_t)initial_words * 4u);
    if (!a->mem) {
        va_free(vm, a, sizeof(Actor));
        return NULL;
    }
    a->mem_words = initial_words;
    a->hp = 0;
    a->sp = a->fp = initial_words;          /* empty stack */
    a->id = term_make_pid(slot, slot_gen(vm->table[slot]));
    a->state = A_RUNNABLE;
    a->max_words = 0;                        /* default limit */
    a->mod = mod;
    vm->table[slot] = (uintptr_t)a;
    vm->live++;
    module_actor_enter(vm, a);
    return a;
}

uint32_t actor_max_words(const Actor *a) {
    if (a->max_words) return a->max_words;
    return GOGO_DEFAULT_MAX_WORDS > 65535u ? 65535u : GOGO_DEFAULT_MAX_WORDS;
}

void actor_free(Vm *vm, Actor *a) {
    while (a->mbox_head) {
        Msg *m = a->mbox_head;
        a->mbox_head = m->next;
        msg_free(vm, m);
    }
    actor_extra_free(vm, a);
    uint16_t slot = term_pid_slot(a->id);
    vm->table[slot] = free_slot(next_gen(slot, term_pid_gen(a->id)));
    if (slot < vm->next_free) vm->next_free = slot;
    va_free(vm, a->mem, (size_t)a->mem_words * 4u);
    module_actor_exit(vm, a);              /* may unload its module */
    va_free(vm, a, sizeof(Actor));
    vm->live--;
}

const VmDist *vm_dist_of(const Vm *vm, term pid) {
    return term_is_pid(pid) && term_pid_slot(pid) >= DIST_SLOT_BASE && term_pid_slot(pid) < vm->port_base ? vm->dist
                                                                                                         : NULL;
}

Actor *actor_lookup(Vm *vm, term pid) {
    if (!term_is_pid(pid)) return NULL;
    uint16_t slot = term_pid_slot(pid);
    if (slot >= vm->table_cap || slot_is_free(vm->table[slot])) return NULL;
    Actor *a = (Actor *)vm->table[slot];
    return a->id == pid ? a : NULL;          /* generation must match too */
}

Actor *actor_lookup_live(Vm *vm, term pid) {
    Actor *a = actor_lookup(vm, pid);
    return a && a->state != A_EXITED && !(a->flags & AF_EXIT_PENDING) ? a : NULL;
}

void runq_push(Vm *vm, Actor *a) {
    a->next = NULL;
    if (vm->runq_tail) vm->runq_tail->next = a;
    else vm->runq_head = a;
    vm->runq_tail = a;
}

Actor *runq_pop(Vm *vm) {
    Actor *a = vm->runq_head;
    if (!a) return NULL;
    vm->runq_head = a->next;
    if (!vm->runq_head) vm->runq_tail = NULL;
    a->next = NULL;
    return a;
}
