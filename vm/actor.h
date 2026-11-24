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
    uint32_t deadline;
} Actor;

enum { A_RUNNABLE = 1, A_WAITING = 2, A_EXITED = 3 };

/* PID table: a live slot holds its Actor*; a free slot holds
 * (gen << 1) | 1, where gen is the 12-bit generation the next actor in
 * that slot will get. Actor pointers are aligned, so bit 0 tells them apart. */
typedef struct Vm {
    const Image *img;
    uintptr_t *table;
    uint16_t table_cap;
    Actor *runq_head, *runq_tail;
    uint32_t live;            /* actors currently allocated */
} Vm;

/* Returns 0, or -1 if the table cannot be allocated. */
int vm_init(Vm *vm, const Image *img, uint16_t table_cap);

/* Slot + PID assigned, block allocated, empty stack, state A_RUNNABLE,
 * not enqueued. NULL if the table is full or memory runs out. */
Actor *actor_new(Vm *vm, uint16_t initial_words);

/* Frees the block and the slot and bumps the slot's generation. The actor
 * must not be on the run queue. */
void actor_free(Vm *vm, Actor *a);

/* NULL if pid is not a PID, names a free slot, or has a stale generation. */
Actor *actor_lookup(Vm *vm, term pid);

void runq_push(Vm *vm, Actor *a);
Actor *runq_pop(Vm *vm);          /* NULL when empty */

#endif
