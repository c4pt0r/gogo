/* Messages: a sent term is copied into a malloc'd block queued on the
 * receiver. The copy holds the heap objects reachable from the message,
 * laid out in `data`, and preserves sharing: an object reached twice is
 * copied once and both refs point at the copy (cycles would terminate too). Refs
 * inside `data` are word offsets relative to `data`; msg_receive moves the
 * words into the receiver's heap and adds the destination offset to them. */
#ifndef GOGO_MSG_H
#define GOGO_MSG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"
#include "actor.h"

/* Msg.flags bit: a reply to a Call (spec M2 §5.3). Its data starts with
 * two words, the low and high half of the Ref serial being answered; the
 * body's objects follow, and refs inside are relative to data as always
 * (so they count the two words). Receive never returns such a block. */
#define MSG_REPLY 1u
#define MSG_REPLY_HDR 2u
/* Msg.flags bit: the block carries off-heap Bytes handles and holds one
 * reference per occurrence until msg_free (bytes.h). */
#define MSG_HANDLES 2u

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

/* Words the block of a message of `words` data words takes. */
uint32_t msg_footprint_words(uint16_t words);

/* Words a queued message is charged, as counted in mbox_words (and in a
 * SendAfter creator's timer charge): its block plus the full length of each
 * distinct off-heap Bytes root it carries (spec M4 §9.5b). */
uint32_t msg_charge_words(const Msg *m);

/* Copies t out of src's heap into a new, unqueued message; NULL if out of
 * memory or the copy exceeds 65535 words. Used by msg_send and by spawn
 * (which moves a closure into the new actor the same way). src is not
 * const: the copy temporarily writes forward markers into src's heap and
 * restores it byte for byte before returning, on success and on failure.
 * Safe because the VM has a single scheduler thread. */
Msg *msg_build(Actor *src, term t);

/* Moves m's words into a's heap (relocating refs) and stores the message
 * term in *out; a then holds the off-heap Bytes it carries (bytes_adopt).
 * m is left untouched. May GC; false if a's heap cannot hold it or a
 * cannot hold that many handles (GOGO_BYTES_PER_ACTOR, after a GC). */
bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out);

void msg_free(Msg *m);

/* 0 delivered, or dropped by the receiver's quota; 1 dropped (dead or
 * stale pid, or receiver marked for exit); -1 out of memory (sender).
 * Quota (spec M2 §5.1.1): if queuing the message would take the receiver's
 * block + charged words (mailbox, pending timers) past its limit (counting
 * the holder list a first off-heap Bytes handle would make, bytes.h), or its mailbox counter past 65535 words,
 * the message is dropped and the receiver is marked to exit with
 * R_MAILBOX_OVERFLOW. Only the receiver's state counts; the sender cannot
 * tell. */
int msg_send(Vm *vm, Actor *from, term to_pid, term msg);

/* Queues m (which it now owns) on `to` and wakes it if waiting. With
 * `quota`, the receiver's limit applies as in msg_send; without (exit
 * notifications, spec M2 §5.3) the message is still counted in mbox_words.
 * Either way, if the 16-bit mailbox counter cannot hold it, m is freed, the
 * receiver is marked to exit with R_MAILBOX_OVERFLOW and false returned.
 * `to` must be alive and not marked for exit. Never allocates. */
bool msg_enqueue(Vm *vm, Actor *to, Msg *m, bool quota);

/* Dequeues the oldest message into a's heap and stores it in *out. Reply
 * blocks (MSG_REPLY) at the head are freed unseen. False
 * if the mailbox is empty, or if the heap cannot hold the message (it then
 * stays queued; check a->mbox_head to tell the two apart). May GC. */
bool msg_receive(Vm *vm, Actor *a, term *out);

/* Sends a reply to the Call that created `ref` (a K_REF term on from's
 * heap) with a copy of body. Like msg_send: 0 delivered or quota-dropped, 1
 * dropped (dead or stale pid, receiver marked for exit, or ref not a Ref),
 * -1 out of memory. A reply that overflows the caller's quota marks the
 * caller MailboxOverflow, as any message does. */
int msg_send_reply(Vm *vm, Actor *from, term to_pid, term ref, term body);

/* Takes the reply with this Ref serial out of a's mailbox into a's heap,
 * stores the body in *out and returns 1. Ordinary messages stay queued. Any
 * other reply block met on the way is stale (a Call blocks, so only one is
 * outstanding) and is freed. 0 if no such reply is queued; -1 if it is but
 * the heap cannot hold it (it stays queued). May GC. */
int msg_take_reply(Vm *vm, Actor *a, uint64_t serial, term *out);

/* Heap words a copy of t needs, or UINT32_MAX if it cannot be copied
 * (out of memory or more than 65535 words). Mutates and restores src's
 * heap like msg_build. */
uint32_t msg_copy_words(Actor *src, term t);

#endif
