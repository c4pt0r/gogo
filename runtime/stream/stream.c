/* Unified data streams: Chunk delivery and Out bookkeeping (stream.h). */
#include "stream.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "bytes.h"
#include "opcodes.h"
#include <string.h>

/* A driver may keep its StreamOut in Port.state. */
typedef char stream_out_fits_port_state[sizeof(StreamOut) <= sizeof(((Port *)0)->state) ? 1 : -1];

/* Seq and TimeMs stay non-negative Go ints. */
#define INT31_MAX 0x7FFFFFFFu

/* ---- Chunk ---- */

/* Words an int field needs beyond itself: a K_INT box if it is too big
 * for an immediate. */
static uint32_t box_words(int32_t v) { return term_int_fits(v) ? 0u : 2u; }

static term put_int(uint32_t *d, uint32_t *at, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    d[*at] = hdr_make(2, K_INT);
    d[*at + 1u] = (uint32_t)v;
    term t = term_from_ref(*at);
    *at += 2u;
    return t;
}

Msg *stream_chunk_new(struct Vm *vm, term from, uint32_t seq, uint32_t time_ms, uint32_t dropped,
                      uint32_t len, uint8_t **data) {
    int32_t t = (int32_t)(time_ms & INT31_MAX);
    uint64_t words = 7u + box_words((int32_t)seq) + box_words(t) + box_words((int32_t)dropped) +
                     (len ? 2u + ((uint64_t)len + 3u) / 4u : 0u);
    if (words > 0xFFFFu) return NULL;
    Msg *m = msg_new(vm, words);         /* inline Bytes: no handles */
    if (!m) return NULL;
    uint32_t *d = m->data, at = 7;
    d[0] = hdr_make(7, K_STRUCT);
    d[1] = TYPE_CHUNK;
    d[2] = from;
    d[3] = put_int(d, &at, (int32_t)seq);
    d[4] = put_int(d, &at, t);
    d[5] = put_int(d, &at, (int32_t)dropped);
    *data = NULL;
    if (len) {
        uint32_t bw = 2u + (len + 3u) / 4u;
        d[at] = hdr_make(bw, K_BYTES);
        d[at + 1u] = len;
        d[at + bw - 1u] = 0;            /* padding */
        *data = (uint8_t *)&d[at + 2u];
        d[6] = term_from_ref(at);
    } else {
        d[6] = T_EMPTYBYTES;
    }
    return m;
}

/* Queues m on o without ever marking it; once more after collecting a
 * waiting o's heap slack (as the net stack does). */
static bool try_deliver(struct Vm *vm, Actor *o, Msg *m) {
    for (int k = 0; k < 2; k++) {
        if (msg_try_enqueue(vm, o, m)) return true;
        if (k || !heap_reclaim_waiting(vm, o)) break;
    }
    return false;
}

bool stream_deliver_chunk(struct Vm *vm, const Port *p, StreamIn *s, uint32_t time_ms,
                          const uint8_t *data, uint32_t len) {
    s->seq = s->seq >= INT31_MAX ? 1u : s->seq + 1u;
    uint32_t dropped = s->dropped_since > INT31_MAX ? INT31_MAX : s->dropped_since;
    Actor *o = port_owner(vm, p);
    uint8_t *w;
    Msg *m = o ? stream_chunk_new(vm, p->id, s->seq, time_ms, dropped, len, &w) : NULL;
    if (m) {
        if (len) memcpy(w, data, len);
        if (try_deliver(vm, o, m)) {
            s->dropped_since = 0;
            return true;
        }
        msg_free(vm, m);
    }
    if (s->dropped_since < UINT32_MAX) s->dropped_since++;
    return false;
}

/* ---- Out / Done ---- */

/* Sends Done{From: p, Seq: seq} to p's owner without ever marking it;
 * once more after collecting a waiting owner's heap slack (as Chunks). */
static bool send_done(struct Vm *vm, const Port *p, int32_t seq) {
    int32_t v[2] = { (int32_t)p->id, seq };
    if (port_deliver_ints(vm, p, TYPE_DONE, v, 2, PORT_RAW(0))) return true;
    Actor *o = port_owner(vm, p);
    return o && heap_reclaim_waiting(vm, o) && port_deliver_ints(vm, p, TYPE_DONE, v, 2, PORT_RAW(0));
}

/* Accepted Dones owed: their Seqs run up to next_seq. */
static uint32_t owed_ok(const StreamOut *o) {
    uint32_t c = 0;
    for (uint32_t k = 0; k < o->owed_n; k++) c += (uint32_t)(o->owed_bits >> k) & 1u;
    return c;
}

static uint32_t seq_back(uint32_t seq, uint32_t k) {   /* seq - k in 1..INT31_MAX */
    while (k--) seq = seq <= 1u ? INT31_MAX : seq - 1u;
    return seq;
}

bool stream_out_flush(struct Vm *vm, Port *p, StreamOut *o) {
    while (o->owed_n) {
        bool ok = o->owed_bits & 1u;
        int32_t seq = ok ? (int32_t)seq_back(o->next_seq, owed_ok(o) - 1u) : -1;
        if (!send_done(vm, p, seq)) return false;
        o->owed_bits = (uint8_t)(o->owed_bits >> 1);
        o->owed_n--;
    }
    return true;
}

/* The next Done in order: an accepted Out's (ok: Seq next_seq + 1) or a
 * refusal. Sent now if nothing older is owed (those go first) and the
 * driver holds no events of its own (user), else owed. */
static void emit(struct Vm *vm, Port *p, StreamOut *o, bool ok) {
    bool clear = !o->user && stream_out_flush(vm, p, o);   /* before next_seq moves: owed Seqs end at it */
    if (ok) o->next_seq = o->next_seq >= INT31_MAX ? 1u : o->next_seq + 1u;
    if (clear && send_done(vm, p, ok ? (int32_t)o->next_seq : -1)) return;
    /* Room stays for the Dones of the Outs in flight (owed_n + n never
     * exceeds STREAM_OWED_MAX), so only a refusal is ever lost here. */
    if (!ok && o->owed_n + o->n >= STREAM_OWED_MAX) return;
    if (ok) o->owed_bits = (uint8_t)(o->owed_bits | 1u << o->owed_n);
    o->owed_n++;
}

/* The Data of an Out{Data} command and its bytes as `from` sees them;
 * false if msg is anything else. */
static bool out_data(struct Vm *vm, const Actor *from, term msg, term *data, const uint8_t **d,
                     uint32_t *len) {
    const uint32_t *o = obj_of(vm, from, msg);
    if (!o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_OUT || hdr_words(o[0]) != 3u) return false;
    *data = o[2];
    return bytes_view(vm, from, o[2], d, len);
}

int stream_out_accept_data(struct Vm *vm, Port *p, StreamOut *o, term data, const uint8_t *d, uint32_t len) {
    term h = T_NIL;
    if (o->n + owed_ok(o) < STREAM_OUT_MAX && o->owed_n + o->n < STREAM_OWED_MAX && d && len) {
        if (term_is_handle(data)) {
            if (port_bytes_retain(vm, p, data)) h = data;
        } else {
            /* A heap or const K_BYTES: copied, so it outlives the sender's
             * next GC and never pins a module. d stays valid: bytes_new
             * does not collect. */
            uint8_t *w;
            h = port_bytes_new(vm, p, len, &w);
            if (h != T_NIL) memcpy(w, d, len);
        }
    }
    if (h == T_NIL) {
        emit(vm, p, o, false);
        return -1;
    }
    o->held[o->n++] = h;
    return o->n == 1u ? 1 : 0;
}

int stream_out_accept(struct Vm *vm, const Actor *from, Port *p, StreamOut *o, term msg,
                      StreamCheck check) {
    term data = T_NIL;
    const uint8_t *d = NULL;
    uint32_t len = 0;
    if (!out_data(vm, from, msg, &data, &d, &len) || (len && check && !check(p, d, len))) d = NULL;
    return stream_out_accept_data(vm, p, o, data, d, len);
}

const uint8_t *stream_out_at(const struct Vm *vm, const StreamOut *o, uint32_t k, uint32_t *len) {
    *len = 0;
    return k < o->n ? bytes_data(vm, o->held[k], len) : NULL;
}

const uint8_t *stream_out_head(const struct Vm *vm, const StreamOut *o, uint32_t *len) { return stream_out_at(vm, o, 0, len); }

static bool finish(struct Vm *vm, Port *p, StreamOut *o, bool ok) {
    if (!o->n) return false;
    port_bytes_release(vm, p, o->held[0]);
    for (uint32_t k = 1; k < STREAM_OUT_MAX; k++) o->held[k - 1u] = o->held[k];
    o->held[STREAM_OUT_MAX - 1u] = 0;
    o->n--;
    /* n just went down, so owed_n + n < STREAM_OWED_MAX: a failure's
     * Done{-1} is never the refusal that emit drops. */
    emit(vm, p, o, ok);
    return o->n != 0;
}

bool stream_out_done(struct Vm *vm, Port *p, StreamOut *o) { return finish(vm, p, o, true); }

bool stream_out_fail(struct Vm *vm, Port *p, StreamOut *o) { return finish(vm, p, o, false); }

void stream_out_close(struct Vm *vm, Port *p, StreamOut *o) {
    if (vm) {
        for (uint32_t k = 0; k < o->n; k++) port_bytes_release(vm, p, o->held[k]);
    }
    memset(o, 0, sizeof *o);
}

bool stream_timings_ok(const Port *p, const uint8_t *d, uint32_t len) {
    (void)p;
    if (!d || !len || (len & 1u)) return false;
    for (uint32_t i = 0; i < len; i += 2u) {
        uint32_t v = (uint32_t)d[i] | (uint32_t)d[i + 1u] << 8;
        if (v == 0 || v > 32767u) return false;
    }
    return true;
}
