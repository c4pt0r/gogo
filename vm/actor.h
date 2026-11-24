/* Actors, the PID table and the run queue.
 *
 * Each actor owns one memory block of mem_words 32-bit words: the heap
 * grows up from word 0 (hp = next free word) and the stack grows down from
 * the top (sp = lowest used word; empty stack has sp == mem_words). */
#ifndef GOGO_ACTOR_H
#define GOGO_ACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "term.h"
#include "image.h"

typedef struct Msg Msg;   /* defined with the mailbox (Task 5) */
typedef struct Timer Timer;   /* defined in timer.h */
typedef struct Name Name;     /* defined in names.c */

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
    uint32_t reserved;        /* unused (timers live in Vm.timers); keeps 48 B on 32-bit */
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

/* PID table: a live slot holds its Actor*; a free slot holds
 * (gen << 1) | 1, where gen is the 12-bit generation the next actor in
 * that slot will get. Actor pointers are aligned, so bit 0 tells them apart. */
typedef struct Vm {
    const Image *img;
    uintptr_t *table;
    uint16_t table_cap;
    Actor *runq_head, *runq_tail;
    uint32_t live;            /* actors currently allocated */
    uint64_t next_ref;        /* serial of the next Ref, from 1 */
    Timer *timers;            /* pending timers, sorted by deadline (timer.h) */
    Name *names;              /* registered names (names.h) */
} Vm;

/* Returns 0, or -1 if the table cannot be allocated. */
int vm_init(Vm *vm, const Image *img, uint16_t table_cap);

/* Slot + PID assigned, block allocated, empty stack, state A_RUNNABLE,
 * max_words 0 (default limit), not enqueued. NULL if the table is full or memory runs out. */
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

void runq_push(Vm *vm, Actor *a);
Actor *runq_pop(Vm *vm);          /* NULL when empty */

#endif
