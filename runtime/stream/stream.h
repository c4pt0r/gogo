/* Unified data streams (spec input2 §1): the one place every streaming
 * driver (IMU samples, IR frames, later audio) builds its messages.
 *
 *  - Input: port.Chunk{Seq, TimeMs, Dropped int; Data bytes.Bytes} to the
 *    port's owner, Data an inline heap K_BYTES in the message block (as
 *    net/udp.c's Datagram). Delivery is non-fatal: a Chunk over the owner's
 *    quota (or to an owner that is gone) is dropped and counted, never
 *    killing anyone. Seq counts every Chunk made, from 1, delivered or not;
 *    Dropped is how many were lost since the last delivered one.
 *  - Output: port.Out{Data bytes.Bytes} commands, at most STREAM_OUT_MAX in
 *    flight per port (the head is running, one more is queued). Each
 *    accepted Out gets exactly one port.Done{Seq} when it finishes, Seq
 *    counting the Outs finished successfully from 1; a refused one (a
 *    third in flight, malformed, empty, or data the driver's check
 *    rejects) and one the driver could not send (stream_out_fail) get
 *    Done{Seq: -1} and are not counted. Dones come in the Outs' order.
 *  - Dones are never lost to a full mailbox: one the owner has no room for
 *    is owed (StreamOut.owed_*) and sent, in order, by a later
 *    stream_out_flush (the driver calls it from its poll), accept or done;
 *    a newer Done never overtakes an owed one. "In flight" counts an Out
 *    until its Done is delivered: an Out whose Done is still owed keeps
 *    its place, so a program that cannot read its Dones cannot have more
 *    than STREAM_OUT_MAX Outs outstanding. At most STREAM_OWED_MAX Dones
 *    are owed, room for the in-flight Outs' own always kept; a refusal
 *    beyond that (a sender flooding Outs at an owner that reads nothing)
 *    is dropped. Close discards what is owed.
 *
 * The Out's Data is held for the port: a handle is pinned
 * (port_bytes_retain), a heap or const K_BYTES is copied into a new
 * port-held root (port_bytes_new). A StreamOut therefore uses the port's
 * PORT_MAX_HELD held slots; a driver using it holds no other Bytes. Handoff
 * moves them with the port (the terms stay the same) and Done then goes to
 * the new owner. Any actor may send a port commands (interp B_SEND), so a
 * non-owner's Out is held, and charged, for the owner (bytes_retain does
 * not check its limit), as for net/ writes; it is bounded by
 * STREAM_OUT_MAX. A stale value in an Out (a const of a module that is gone)
 * never reaches the driver: the interpreter's port-send gate (B_SEND,
 * deep_live) crashes the sender with badcode first.
 *
 * Scheduler thread only, like every port callback. */
#ifndef GOGO_STREAM_H
#define GOGO_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "term.h"
#include "port.h"

struct Vm;
struct Actor;

/* ---- input: Chunk ---- */

typedef struct {
    uint32_t seq;             /* Seq of the last Chunk made (0: none yet) */
    uint32_t dropped_since;   /* Chunks lost since the last delivered one */
} StreamIn;

/* Makes the next Chunk of s (Seq + 1) with TimeMs = the low 31 bits of
 * time_ms and a copy of data[0..len) (len 0: the empty Bytes), and queues
 * it on p's owner without killing it. True if delivered (dropped_since
 * starts again from 0); false if dropped (owner gone or exiting, out of
 * memory, over its quota, or too big for a message): dropped_since + 1. A
 * driver that loses data on its own side (a truncated IR frame) adds to
 * dropped_since before the call. Seq wraps from 2^31 - 1 to 1. */
bool stream_deliver_chunk(struct Vm *vm, const Port *p, StreamIn *s, uint32_t time_ms,
                          const uint8_t *data, uint32_t len);

/* ---- output: Out / Done ---- */

#define STREAM_OUT_MAX 2u
#define STREAM_OWED_MAX 16u      /* bits in owed_bits */

/* 16 bytes (it fits Port.state). Zero it in open; stream_out_close zeroes
 * it again. */
typedef struct {
    term held[STREAM_OUT_MAX];   /* [0] the head (running), [1] queued; 0 = none */
    uint32_t next_seq;           /* Seq of the last finished Out (its Done sent or owed) */
    uint8_t n;                   /* Outs running or queued */
    uint8_t owed_n;              /* Dones owed, oldest first */
    uint16_t owed_bits;          /* bit k: owed Done k is an accepted Out's (else Done{-1});
                                  * the accepted ones are the last Seqs up to next_seq */
} StreamOut;

/* A data check for stream_out_accept (NULL: any non-empty data); p for
 * checks that depend on the port's config (Port.state). */
typedef bool (*StreamCheck)(const Port *p, const uint8_t *d, uint32_t len);

/* Takes the command msg (read in place on from's heap, as a driver's
 * command_term gets it) as an Out{Data}. Returns 1 if it became the head
 * (the driver starts it now), 0 if queued behind the head, -1 if refused
 * (msg not an Out{Data} with non-empty Bytes data, `check` said no,
 * STREAM_OUT_MAX already in flight, or the port cannot hold the data):
 * the owner is sent (or owed) Done{-1}. Every message counts as an Out: a
 * driver that takes other commands too dispatches on the struct type
 * (obj_of(vm, from, msg)[1]) first. */
int stream_out_accept(struct Vm *vm, const struct Actor *from, Port *p, StreamOut *o,
                      term msg, StreamCheck check);

/* The head's data and length, or NULL (*len 0) when nothing is in flight. */
const uint8_t *stream_out_head(const struct Vm *vm, const StreamOut *o, uint32_t *len);

/* Out k in flight (0 the head, 1 the queued one), or NULL (*len 0) if
 * k >= o->n: for a driver that hands the queued Out to the hardware while
 * the head is still running (the speaker, to play without a gap). */
const uint8_t *stream_out_at(const struct Vm *vm, const StreamOut *o, uint32_t k, uint32_t *len);

/* The head finished: releases its data, promotes the queued one and sends
 * (or owes) Done{Seq}. True if there is a new head (the driver starts it
 * now). Nothing in flight: no-op, false. */
bool stream_out_done(struct Vm *vm, Port *p, StreamOut *o);

/* The head failed (the hardware could not send it): as stream_out_done, but
 * the owner gets Done{-1}, as for a refused Out, and no Seq is used, so a
 * program can tell "not sent" from "sent". */
bool stream_out_fail(struct Vm *vm, Port *p, StreamOut *o);

/* Sends the owed Dones, oldest first, until one does not fit. True when
 * none is owed any more. Cheap when none is: call it on every poll. */
bool stream_out_flush(struct Vm *vm, Port *p, StreamOut *o);

/* Forgets every Out in flight and every owed Done (the port is closing)
 * and releases their data; o is zeroed. vm may be NULL (a PortDriver.close
 * has none): then only o is cleared, and port_close, which runs right
 * after the driver's close, releases the port-held data itself. */
void stream_out_close(struct Vm *vm, Port *p, StreamOut *o);

/* IR timings (spec input2 §3): uint16 LE microseconds, mark first; valid
 * when len is even and non-zero and every value is 1..32767. A StreamCheck
 * (p unused). */
bool stream_timings_ok(const Port *p, const uint8_t *d, uint32_t len);

#endif
