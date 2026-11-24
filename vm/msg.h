/* Messages: a sent term is copied into a malloc'd block queued on the
 * receiver. The copy is a tree copy (M1: no sharing or cycle preservation)
 * of the heap objects reachable from the message, laid out in `data`. Refs
 * inside `data` are word offsets relative to `data`; msg_receive moves the
 * words into the receiver's heap and adds the destination offset to them. */
#ifndef GOGO_MSG_H
#define GOGO_MSG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"

struct Msg {
    Msg *next;
    uint16_t words;       /* length of data, in words */
    uint16_t flags;
    term root;            /* the message term; a ref is relative to data */
    uint32_t data[];
};

/* Exact size of the allocation holding a message of `words` data words. */
static inline size_t msg_alloc_bytes(uint16_t words) {
    return sizeof(Msg) + (size_t)words * 4u;
}

/* Copies t out of src's heap into a new, unqueued message; NULL if out of
 * memory or the copy exceeds 65535 words. Used by msg_send and by spawn
 * (which moves a closure into the new actor the same way). */
Msg *msg_build(const Actor *src, term t);

/* Moves m's words into a's heap (relocating refs) and stores the message
 * term in *out. m is left untouched. May GC; false if a's heap cannot hold
 * it. */
bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out);

void msg_free(Msg *m);

/* 0 delivered, 1 dropped (dead or stale pid), -1 out of memory (sender),
 * -2 receiver's mailbox counter would overflow (M1: the sender crashes with
 * mailbox_full; the spec wants the receiver to exit instead, see M2). */
int msg_send(Vm *vm, Actor *from, term to_pid, term msg);

/* Dequeues the oldest message into a's heap and stores it in *out. False
 * if the mailbox is empty, or if the heap cannot hold the message (it then
 * stays queued; check a->mbox_head to tell the two apart). May GC. */
bool msg_receive(Vm *vm, Actor *a, term *out);

/* Heap words a copy of t needs, or UINT32_MAX if it cannot be copied
 * (out of memory or more than 65535 words). */
uint32_t msg_copy_words(const Actor *src, term t);

#endif
