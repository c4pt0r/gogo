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

/* Msg.flags bit: a reply to a Call. Its data starts with
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
 * distinct off-heap Bytes root it carries. */
uint32_t msg_charge_words(const Msg *m);

/* Copies t out of src's heap into a new, unqueued message for an actor of
 * module slot dst_mod; NULL if out of memory, the copy exceeds 65535
 * words, or the cross-module rules refuse it (docs/design.md §5.4.4; the same rules
 * for every module, the main program included): a value naming a module
 * (const ref, closure, struct of a module's own type) enters only that
 * module's actors as it is; into another module a const string, int or
 * int64 goes as a heap copy (so the receiver never pins or reads that
 * module's code), anything else of a module is refused with R_BAD_MSG,
 * and a value of a module that is gone with R_BAD_CODE (also within one
 * module). *why (if why is not NULL) is the sender's exit reason on NULL:
 * R_BAD_MSG, R_BAD_CODE or R_OUT_OF_MEMORY. Reserved structs, Bytes,
 * PIDs, Refs and immediates always pass. Used by msg_send, SendAfter and
 * spawn (which moves a closure or code.Spawn's arg into the new actor the
 * same way). src is not const: the copy temporarily writes forward markers
 * into src's heap and restores it byte for byte before returning, on
 * success and on failure. Safe because the VM has a single scheduler
 * thread. */
Msg *msg_build(const Vm *vm, Actor *src, term t, uint32_t dst_mod, uint32_t *why);

/* Moves m's words into a's heap (relocating refs) and stores the message
 * term in *out; a then holds the off-heap Bytes it carries (bytes_adopt).
 * m is left untouched. May GC; false if a's heap cannot hold it or a
 * cannot hold that many handles (GOGO_BYTES_PER_ACTOR, after a GC). */
bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out);

void msg_free(Msg *m);

/* 0 delivered, or dropped by the receiver's quota; 1 dropped (dead or
 * stale pid, or receiver marked for exit); or the sender's exit reason,
 * negated: -R_OUT_OF_MEMORY, -R_BAD_MSG, -R_BAD_CODE (msg_build). A
 * dropped message is still checked, against from's own module (as
 * SendAfter to a dead target): a stale value is -R_BAD_CODE either way.
 * Quota: if queuing the message would take the receiver's
 * block + charged words (mailbox, pending timers) past its limit (counting
 * the holder list a first off-heap Bytes handle would make, bytes.h), or its mailbox counter past 65535 words,
 * the message is dropped and the receiver is marked to exit with
 * R_MAILBOX_OVERFLOW. Only the receiver's state counts; the sender cannot
 * tell. */
int msg_send(Vm *vm, Actor *from, term to_pid, term msg);

/* Queues m (which it now owns) on `to` and wakes it if waiting. With
 * `quota`, the receiver's limit applies as in msg_send; without (exit
 * notifications) the message is still counted in mbox_words.
 * Either way, if the 16-bit mailbox counter cannot hold it, m is freed, the
 * receiver is marked to exit with R_MAILBOX_OVERFLOW and false returned.
 * `to` must be alive and not marked for exit. Never allocates. */
bool msg_enqueue(Vm *vm, Actor *to, Msg *m, bool quota);

/* Non-fatal delivery: queues m on `to`
 * like msg_enqueue with quota and returns true; but if `to` is exited or
 * marked to exit, or m would take it past its limit or its mailbox
 * counter, returns false and leaves m unqueued and still the caller's (to
 * free or retry). The receiver is never marked. Never allocates. */
bool msg_try_enqueue(Vm *vm, Actor *to, Msg *m);

/* Dequeues the oldest message into a's heap and stores it in *out. Reply
 * blocks (MSG_REPLY) at the head are freed unseen. False
 * if the mailbox is empty, or if the heap cannot hold the message (it then
 * stays queued; check a->mbox_head to tell the two apart). May GC. */
bool msg_receive(Vm *vm, Actor *a, term *out);

/* Sends a reply to the Call that created `ref` (a K_REF term on from's
 * heap) with a copy of body. Like msg_send: 0 delivered or quota-dropped, 1
 * dropped (dead or stale pid, receiver marked for exit, or ref not a Ref),
 * or a negated exit reason as msg_send (a dropped body is checked the same
 * way). A reply that overflows the caller's quota marks the
 * caller MailboxOverflow, as any message does. */
int msg_send_reply(Vm *vm, Actor *from, term to_pid, term ref, term body);

/* Takes the reply with this Ref serial out of a's mailbox into a's heap,
 * stores the body in *out and returns 1. Ordinary messages stay queued. Any
 * other reply block met on the way is stale (a Call blocks, so only one is
 * outstanding) and is freed. 0 if no such reply is queued; -1 if it is but
 * the heap cannot hold it (it stays queued). May GC. */
int msg_take_reply(Vm *vm, Actor *a, uint64_t serial, term *out);

#endif
