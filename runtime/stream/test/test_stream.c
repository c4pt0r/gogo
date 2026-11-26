/* stream/ on a real VM: Chunk Seq/TimeMs/Dropped across a full mailbox,
 * the Out flood (exactly 2 accepted, the rest Done{-1}, one Done each),
 * malformed Outs, Done ordering and the head, close with Outs in flight
 * (no Done, Bytes released), Handoff, the timing check, and a stale
 * Bytes in an Out (the sender's badcode, through the interpreter). The
 * port is a fake "st" driver whose command_term is stream_out_accept. */
#include "test.h"
#include "test_host.h"
#include "stream.h"
#include "actor.h"
#include "signals.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "module.h"
#include "opcodes.h"
#include "port.h"
#include "evq.h"
#include "bytes.h"
#include <string.h>

extern void (*platform_unix_err_hook)(const char *, size_t);

static char err_buf[512];
static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}

/* ---- the image: f0 "main": Send(r2, r3); const "main" at 0 (images have
 * no const Bytes: a const in an Out is a string, refused as not Bytes) ---- */
#define IMG_WORDS 40
static uint32_t img_w[IMG_WORDS];
static Image img;
static Vm vm;

static void mini_image(void) {
    uint32_t *w = img_w;
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 3; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (4u << 16); w[10] = 0;          /* f0: code 0, 0 params, 4 regs, name 0 */
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_BUILTIN, 1, 2, 2); w[15] = B_SEND;
    w[16] = OP_MAKE(OP_RET, 1, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
}

/* ---- the fake driver ---- */
static StreamOut so;
static StreamCheck check;
static int last_rc, accepts, closes;

static int st_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)p; (void)index; (void)cfg; (void)n;
    memset(&so, 0, sizeof so);
    return 0;
}
static void st_close(struct Vm *vm, Port *p) { (void)vm; closes++; stream_out_close(NULL, p, &so); }
static bool st_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return true; }
static void st_command(Vm *v, const Actor *from, Port *p, term msg) {
    accepts++;
    last_rc = stream_out_accept(v, from, p, &so, msg, check);
}
static const PortDriver st_driver = {
    "st", TYPE_IR_TX, TYPE_DONE, st_open, NULL, st_close, st_can_event, NULL, st_command, NULL, NULL
};
static const PortDriver *const drivers[] = { &st_driver, NULL };

static Actor *owner;
static term sp;

static Port *port_at(term pid) { return &vm.ports[term_pid_slot(pid) - PORT_SLOT_BASE]; }

static void setup(void) {
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    mini_image();
    CHECK_EQ(test_vm_init(&vm, &img, 16), 0);
    port_set_drivers(&vm, drivers);
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
    sp = port_open_with(&vm, owner, &st_driver, NULL, 0);
    CHECK(port_is(sp));
    check = NULL;
    last_rc = 99;
    accepts = closes = 0;
}

static void teardown(void) {
    port_close_all(&vm);
}

/* ---- mailbox helpers ---- */

static Msg *pop(Actor *a) {
    Msg *m = a->mbox_head;
    if (!m) return NULL;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
    return m;
}

static int mbox_n(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

static int32_t word_int(const Msg *m, term t) {
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t));
    const uint32_t *o = &m->data[term_ref_off(t)];
    CHECK_EQ(o[0], hdr_make(2, K_INT));
    return (int32_t)o[1];
}

/* Pops the Dones in a's mailbox into seqs[] (each must be a Done). */
static int dones(Actor *a, int32_t *seqs, int cap) {
    int n = 0;
    Msg *m;
    while ((m = pop(a)) != NULL) {
        CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
        CHECK_EQ(m->data[1], TYPE_DONE);
        CHECK_EQ(m->data[2], sp);              /* From: the port */
        if (n < cap) seqs[n] = word_int(m, m->data[3]);
        n++;
        msg_free(&vm, m);
    }
    return n;
}

typedef struct { int32_t seq, time, dropped; uint32_t len; uint8_t data[64]; } Chunk;

static bool pop_chunk(Actor *a, Chunk *c) {
    Msg *m = pop(a);
    if (!m) return false;
    CHECK_EQ(m->data[0], hdr_make(7, K_STRUCT));
    CHECK_EQ(m->data[1], TYPE_CHUNK);
    CHECK_EQ(m->flags, 0u);                  /* inline Bytes: no handles */
    CHECK_EQ(m->data[2], sp);                /* From: the port */
    c->seq = word_int(m, m->data[3]);
    c->time = word_int(m, m->data[4]);
    c->dropped = word_int(m, m->data[5]);
    c->len = 0;
    if (m->data[6] != T_EMPTYBYTES) {
        CHECK(term_is_ref(m->data[6]));
        const uint32_t *b = &m->data[term_ref_off(m->data[6])];
        CHECK_EQ(hdr_kind(b[0]), K_BYTES);
        c->len = b[1];
        CHECK_EQ(hdr_words(b[0]), 2u + (c->len + 3u) / 4u);
        if (c->len <= sizeof c->data) memcpy(c->data, &b[2], c->len);
    }
    /* The block is exactly what it holds: no slack, no stray refs. */
    msg_free(&vm, m);
    return true;
}

/* A heap K_BYTES on a's heap. */
static term heap_bytes(Actor *a, const uint8_t *d, uint32_t len) {
    uint32_t words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_BYTES);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, d, len);
    return term_from_ref(off);
}

/* A heap struct {type, fields...} on a's heap. */
static term heap_struct(Actor *a, uint32_t type, uint32_t n, const term *f) {
    uint32_t off = heap_alloc(&vm, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term out_of(Actor *a, term data) { return heap_struct(a, TYPE_OUT, 1, &data); }

static const uint8_t T4[4] = { 100, 0, 200, 0 };   /* timings {100, 200} */

/* Sends Out{heap Bytes d} from a to the port, as B_SEND does. */
static int send_out(Actor *a, const uint8_t *d, uint32_t len) {
    a->hp = 0;
    port_command(&vm, a, sp, out_of(a, heap_bytes(a, d, len)));
    return last_rc;
}

/* ---- Chunk ---- */

TEST(chunk_fields_and_data) {
    setup();
    StreamIn s = { 0, 0 };
    const uint8_t d[5] = { 1, 2, 3, 4, 5 };
    CHECK(stream_deliver_chunk(&vm, port_at(sp), &s, 1234, d, 5));
    CHECK(stream_deliver_chunk(&vm, port_at(sp), &s, 0xFFFFFFFFu, NULL, 0));   /* boxed TimeMs, empty */
    Chunk c;
    CHECK(pop_chunk(owner, &c));
    CHECK_EQ(c.seq, 1); CHECK_EQ(c.time, 1234); CHECK_EQ(c.dropped, 0);
    CHECK_EQ(c.len, 5u); CHECK(memcmp(c.data, d, 5) == 0);
    CHECK(pop_chunk(owner, &c));
    CHECK_EQ(c.seq, 2); CHECK_EQ(c.time, 0x7FFFFFFF); CHECK_EQ(c.dropped, 0);
    CHECK_EQ(c.len, 0u);
    CHECK(!pop_chunk(owner, &c));
    CHECK_EQ(s.seq, 2u); CHECK_EQ(s.dropped_since, 0u);
    teardown();
}

/* Over-quota Chunks drop without killing the
 * owner; Seq keeps counting; the next delivered one carries Dropped. */
TEST(chunk_seq_dropped_full_mailbox) {
    setup();
    owner->max_words = 400;
    StreamIn s = { 0, 0 };
    uint8_t d[64];
    for (int i = 0; i < 64; i++) d[i] = (uint8_t)i;
    int delivered = 0, dropped = 0;
    for (int i = 0; i < 40; i++) {
        if (stream_deliver_chunk(&vm, port_at(sp), &s, (uint32_t)i, d, 64)) delivered++;
        else dropped++;
    }
    CHECK(delivered > 0 && dropped > 0);
    CHECK(!(owner->flags & AF_EXIT_PENDING));   /* never killed */
    CHECK_EQ(s.seq, 40u);
    CHECK_EQ(s.dropped_since, (uint32_t)dropped);
    Chunk c;
    int32_t want = 1;
    while (pop_chunk(owner, &c)) {               /* the first `delivered` in order, no gap */
        CHECK_EQ(c.seq, want); CHECK_EQ(c.dropped, 0); CHECK_EQ(c.time, want - 1);
        CHECK_EQ(c.len, 64u); CHECK(memcmp(c.data, d, 64) == 0);
        want++;
    }
    CHECK_EQ(want - 1, delivered);
    /* Room again: the next one says how many were lost, and its Seq shows the gap. */
    CHECK(stream_deliver_chunk(&vm, port_at(sp), &s, 99, d, 3));
    CHECK(pop_chunk(owner, &c));
    CHECK_EQ(c.seq, 41); CHECK_EQ(c.dropped, dropped);
    CHECK_EQ(s.dropped_since, 0u);
    /* A driver-side loss (a truncated frame) adds to the next Chunk's Dropped. */
    s.dropped_since++;
    CHECK(stream_deliver_chunk(&vm, port_at(sp), &s, 100, d, 1));
    CHECK(pop_chunk(owner, &c));
    CHECK_EQ(c.seq, 42); CHECK_EQ(c.dropped, 1);
    teardown();
}

TEST(chunk_owner_gone_counts) {
    setup();
    StreamIn s = { 0, 0 };
    owner->flags |= AF_EXIT_PENDING;
    CHECK(!stream_deliver_chunk(&vm, port_at(sp), &s, 0, T4, 4));
    CHECK_EQ(s.seq, 1u); CHECK_EQ(s.dropped_since, 1u);
    owner->flags &= (uint8_t)~AF_EXIT_PENDING;
    /* Too big for a message block: dropped and counted, nothing else. */
    static uint8_t big[0x40000];
    CHECK(!stream_deliver_chunk(&vm, port_at(sp), &s, 0, big, sizeof big));
    CHECK_EQ(s.seq, 2u); CHECK_EQ(s.dropped_since, 2u);
    CHECK_EQ(mbox_n(owner), 0);
    teardown();
}

TEST(chunk_seq_wraps_positive) {
    setup();
    StreamIn s = { 0x7FFFFFFFu, 0 };
    CHECK(stream_deliver_chunk(&vm, port_at(sp), &s, 0, T4, 4));
    Chunk c;
    CHECK(pop_chunk(owner, &c));
    CHECK_EQ(c.seq, 1);
    teardown();
}

/* ---- Out ---- */

/* Ten Outs at once: two accepted (head, queued), eight
 * Done{-1}; finishing both gives Done{1}, Done{2}; one Done per Out. */
TEST(out_flood) {
    setup();
    Actor *snd = actor_new(&vm, 256);
    int rcs[10];
    for (int i = 0; i < 10; i++) rcs[i] = send_out(snd, T4, 4);
    CHECK_EQ(rcs[0], 1); CHECK_EQ(rcs[1], 0);
    for (int i = 2; i < 10; i++) CHECK_EQ(rcs[i], -1);
    int32_t seqs[16];
    CHECK_EQ(dones(owner, seqs, 16), 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(seqs[i], -1);
    CHECK_EQ(so.n, 2u);
    CHECK(stream_out_done(&vm, port_at(sp), &so));       /* the queued one is the head now */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK(!stream_out_done(&vm, port_at(sp), &so));      /* nothing in flight: no Done */
    CHECK_EQ(dones(owner, seqs, 16), 2);
    CHECK_EQ(seqs[0], 1); CHECK_EQ(seqs[1], 2);
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) CHECK_EQ(port_at(sp)->held[k], 0u);
    CHECK_EQ(snd->mbox_head, NULL);                    /* Dones go to the owner, not the sender */
    teardown();
}

/* The head and the order: data of each Out in turn; Seq keeps counting
 * across idle periods; refusals do not use a Seq. */
TEST(out_done_ordering_and_head) {
    setup();
    const uint8_t a[2] = { 7, 0 }, b[4] = { 8, 0, 9, 0 }, c[2] = { 10, 0 };
    uint32_t len;
    CHECK(stream_out_head(&vm, &so, &len) == NULL); CHECK_EQ(len, 0u);
    CHECK_EQ(send_out(owner, a, 2), 1);
    CHECK_EQ(send_out(owner, b, 4), 0);
    const uint8_t *h = stream_out_head(&vm, &so, &len);
    CHECK(h && len == 2 && h[0] == 7);
    CHECK(stream_out_done(&vm, port_at(sp), &so));
    h = stream_out_head(&vm, &so, &len);
    CHECK(h && len == 4 && h[0] == 8 && h[2] == 9);
    CHECK_EQ(send_out(owner, c, 2), 0);                 /* queued behind b */
    CHECK_EQ(send_out(owner, c, 2), -1);                /* full: refused, no Seq */
    CHECK(stream_out_done(&vm, port_at(sp), &so));
    h = stream_out_head(&vm, &so, &len);
    CHECK(h && len == 2 && h[0] == 10);
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK_EQ(send_out(owner, a, 2), 1);                 /* idle again: starts at once */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    int32_t seqs[8];
    CHECK_EQ(dones(owner, seqs, 8), 5);
    CHECK_EQ(seqs[0], 1); CHECK_EQ(seqs[1], -1); CHECK_EQ(seqs[2], 2);
    CHECK_EQ(seqs[3], 3); CHECK_EQ(seqs[4], 4);
    teardown();
}

/* stream_out_at: every Out in flight by position, NULL past the end; the
 * queued one moves to 0 when the head finishes. */
TEST(out_at_positions) {
    setup();
    const uint8_t a[2] = { 7, 0 }, b[4] = { 8, 0, 9, 0 };
    uint32_t len;
    CHECK(stream_out_at(&vm, &so, 0, &len) == NULL); CHECK_EQ(len, 0u);
    CHECK_EQ(send_out(owner, a, 2), 1);
    CHECK(stream_out_at(&vm, &so, 1, &len) == NULL); CHECK_EQ(len, 0u);
    CHECK_EQ(send_out(owner, b, 4), 0);
    const uint8_t *d = stream_out_at(&vm, &so, 0, &len);
    CHECK(d && len == 2 && d[0] == 7);
    d = stream_out_at(&vm, &so, 1, &len);
    CHECK(d && len == 4 && d[0] == 8 && d[2] == 9);
    CHECK(stream_out_at(&vm, &so, 2, &len) == NULL);
    CHECK(stream_out_done(&vm, port_at(sp), &so));
    d = stream_out_at(&vm, &so, 0, &len);
    CHECK(d && len == 4 && d[0] == 8);
    CHECK(stream_out_at(&vm, &so, 1, &len) == NULL);
    teardown();
}

/* A head the driver could not send: Done{-1}, no Seq used, the queued one
 * becomes the head; Dones stay in order. */
TEST(out_fail_head) {
    setup();
    const uint8_t a[2] = { 7, 0 }, b[2] = { 8, 0 };
    uint32_t len;
    CHECK_EQ(send_out(owner, a, 2), 1);
    CHECK_EQ(send_out(owner, b, 2), 0);
    CHECK(stream_out_fail(&vm, port_at(sp), &so));
    const uint8_t *h = stream_out_head(&vm, &so, &len);
    CHECK(h && len == 2 && h[0] == 8);
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK(!stream_out_fail(&vm, port_at(sp), &so));    /* nothing in flight: no-op */
    CHECK_EQ(send_out(owner, a, 2), 1);
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    int32_t seqs[8];
    CHECK_EQ(dones(owner, seqs, 8), 3);
    CHECK_EQ(seqs[0], -1); CHECK_EQ(seqs[1], 1); CHECK_EQ(seqs[2], 2);
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) CHECK_EQ(port_at(sp)->held[k], 0u);
    teardown();
}

static bool is_free(term h) { return !bytes_valid(&vm, h); }

/* Malformed Outs: Done{-1} each, nothing held, the port still works. */
TEST(out_malformed) {
    setup();
    check = stream_timings_ok;
    Actor *a = owner;
    a->hp = 0;
    term one = term_from_int(1);
    term bad[] = {
        term_from_int(5),                                    /* not a struct */
        heap_struct(a, TYPE_DONE, 1, &one),                  /* another type */
        heap_struct(a, TYPE_OUT, 0, NULL),                   /* no field */
        out_of(a, term_from_int(3)),                         /* Data not Bytes */
        out_of(a, T_EMPTYSTR),
        out_of(a, T_EMPTYBYTES),                             /* empty */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        last_rc = 99;
        port_command(&vm, a, sp, bad[i]);
        CHECK_EQ(last_rc, -1);
    }
    term two[2] = { one, one };
    port_command(&vm, a, sp, heap_struct(a, TYPE_OUT, 2, two));   /* two fields */
    CHECK_EQ(last_rc, -1);
    const uint8_t odd[3] = { 1, 0, 2 }, zero[4] = { 1, 0, 0, 0 }, over[2] = { 0x00, 0x80 };
    CHECK_EQ(send_out(a, odd, 3), -1);
    CHECK_EQ(send_out(a, zero, 4), -1);
    CHECK_EQ(send_out(a, over, 2), -1);
    CHECK_EQ(so.n, 0u);
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) CHECK_EQ(port_at(sp)->held[k], 0u);
    int32_t seqs[16];
    CHECK_EQ(dones(owner, seqs, 16), 10);
    for (int i = 0; i < 10; i++) CHECK_EQ(seqs[i], -1);
    const uint8_t max[2] = { 0xFF, 0x7F };
    CHECK_EQ(send_out(a, max, 2), 1);                    /* 32767: fine */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK_EQ(dones(owner, seqs, 16), 1);
    CHECK_EQ(seqs[0], 1);
    teardown();
}

TEST(timings_ok_rules) {
    const uint8_t ok[6] = { 1, 0, 0xFF, 0x7F, 0x28, 0x23 };
    CHECK(stream_timings_ok(NULL, ok, 6));
    CHECK(stream_timings_ok(NULL, ok, 2));
    CHECK(!stream_timings_ok(NULL, ok, 0));
    CHECK(!stream_timings_ok(NULL, NULL, 0));
    CHECK(!stream_timings_ok(NULL, ok, 5));
    const uint8_t z[2] = { 0, 0 }, o[2] = { 0, 0x80 }, f[2] = { 0xFF, 0xFF };
    CHECK(!stream_timings_ok(NULL, z, 2));
    CHECK(!stream_timings_ok(NULL, o, 2));
    CHECK(!stream_timings_ok(NULL, f, 2));
}

/* A handle is pinned (no copy), a heap Bytes copied; close with both in
 * flight sends no Done and releases both; the sender's own handle lives
 * on until it lets go. */
TEST(out_close_in_flight) {
    setup();
    uint8_t *w;
    term h = bytes_new(&vm, owner, 4, &w);             /* the owner's own handle */
    CHECK(h != T_NIL);
    memcpy(w, T4, 4);
    port_command(&vm, owner, sp, out_of(owner, h));
    CHECK_EQ(last_rc, 1);
    CHECK_EQ(so.held[0], h);                           /* pinned, not copied */
    CHECK_EQ(send_out(owner, T4, 4), 0);
    term copy = so.held[1];
    CHECK(term_is_handle(copy) && copy != h);
    uint32_t len;
    CHECK(stream_out_head(&vm, &so, &len) == bytes_data(&vm, h, &len));
    stream_out_close(&vm, port_at(sp), &so);
    CHECK_EQ(so.n, 0u); CHECK_EQ(so.held[0], 0u); CHECK_EQ(so.held[1], 0u);
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) CHECK_EQ(port_at(sp)->held[k], 0u);
    CHECK(is_free(copy));
    CHECK(!is_free(h));                                /* still the owner's */
    bytes_release(&vm, owner, h);
    CHECK(is_free(h));
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK_EQ(mbox_n(owner), 0);
    teardown();
}

/* Through port_close (the driver's close has no vm): no Done, the port
 * releases what it held; then the owner exits cleanly. */
TEST(out_port_close_releases) {
    setup();
    CHECK_EQ(send_out(owner, T4, 4), 1);
    CHECK_EQ(send_out(owner, T4, 4), 0);
    term a = so.held[0], b = so.held[1];
    port_close(&vm, sp);
    CHECK_EQ(closes, 1);
    CHECK(is_free(a)); CHECK(is_free(b));
    CHECK_EQ(so.n, 0u);
    CHECK_EQ(mbox_n(owner), 0);
    actor_exit(&vm, owner, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* After Handoff the held data moves and Done goes to the new owner. */
TEST(out_handoff) {
    setup();
    Actor *to = actor_new(&vm, 256);
    to->state = A_WAITING;
    CHECK_EQ(send_out(owner, T4, 4), 1);
    CHECK(port_handoff(&vm, owner, sp, to->id));
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    int32_t seqs[4];
    CHECK_EQ(dones(owner, seqs, 4), 0);
    CHECK_EQ(dones(to, seqs, 4), 1);
    CHECK_EQ(seqs[0], 1);
    teardown();
}

/* No room for any message (a limit below what the owner uses): Dones are
 * owed instead; the owner is not waiting, so there is no slack to reclaim.
 * Outs then come from another actor `snd` as handles the owner already
 * holds (pinning is not limit-checked; a copy would be). */
static Actor *snd;
static term hh[4];
static void owner_full(void) { owner->state = A_RUNNABLE; owner->max_words = 1; }
static void owner_room(void) { owner->max_words = 0; }

static void make_handles(void) {
    snd = actor_new(&vm, 256);
    for (int i = 0; i < 4; i++) {
        uint8_t *w;
        hh[i] = bytes_new(&vm, owner, 4, &w);
        CHECK(hh[i] != T_NIL);
        memcpy(w, T4, 4);
        w[0] = (uint8_t)(i + 1);
    }
}
static int send_h(int i) {
    snd->hp = 0;
    port_command(&vm, snd, sp, out_of(snd, hh[i]));
    return last_rc;
}

/* The owner's mailbox is full while Outs finish and one is refused: the
 * Dones are owed, an Out whose Done is owed still counts as in flight, and
 * once there is room every Done arrives exactly once, in order. */
TEST(out_dones_owed_until_room) {
    setup();
    make_handles();
    CHECK_EQ(send_h(0), 1);
    CHECK_EQ(send_h(1), 0);
    owner_full();
    CHECK(stream_out_done(&vm, port_at(sp), &so));      /* Done{1} owed */
    CHECK_EQ(mbox_n(owner), 0);
    CHECK_EQ(send_h(2), -1);                            /* 1 queued + 1 owed: full; Done{-1} owed */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));     /* Done{2} owed */
    CHECK_EQ(so.owed_n, 3u);
    CHECK_EQ(send_h(2), -1);                            /* both Dones still owed: refused */
    CHECK(!stream_out_flush(&vm, port_at(sp), &so));
    CHECK_EQ(mbox_n(owner), 0);
    for (uint32_t k = 0; k < PORT_MAX_HELD; k++) CHECK_EQ(port_at(sp)->held[k], 0u);   /* released at done */
    owner_room();
    CHECK(stream_out_flush(&vm, port_at(sp), &so));
    CHECK(stream_out_flush(&vm, port_at(sp), &so));     /* nothing left: no repeats */
    int32_t seqs[8];
    CHECK_EQ(dones(owner, seqs, 8), 4);
    CHECK_EQ(seqs[0], 1); CHECK_EQ(seqs[1], -1); CHECK_EQ(seqs[2], 2); CHECK_EQ(seqs[3], -1);
    CHECK_EQ(send_h(3), 1);                             /* acknowledged: room again */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));
    CHECK_EQ(dones(owner, seqs, 8), 1);
    CHECK_EQ(seqs[0], 3);
    CHECK(!(owner->flags & AF_EXIT_PENDING));
    CHECK_EQ(snd->mbox_head, NULL);
    teardown();
}

/* A Done sent while an older one is owed goes behind it: done flushes
 * first, and a newer Done never overtakes. */
TEST(out_owed_partial_flush_keeps_order) {
    setup();
    make_handles();
    CHECK_EQ(send_h(0), 1);
    owner_full();
    CHECK(!stream_out_done(&vm, port_at(sp), &so));     /* Done{1} owed */
    CHECK_EQ(send_h(1), 1);                             /* 0 running + 1 owed < 2: accepted */
    /* room for exactly one Done (a 4-word block) */
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner) + msg_footprint_words(4));
    CHECK(!stream_out_done(&vm, port_at(sp), &so));     /* sends Done{1}, owes Done{2} */
    CHECK_EQ(so.owed_n, 1u);
    int32_t seqs[4];
    CHECK_EQ(dones(owner, seqs, 4), 1);
    CHECK_EQ(seqs[0], 1);
    owner_room();
    CHECK(stream_out_flush(&vm, port_at(sp), &so));
    CHECK_EQ(dones(owner, seqs, 4), 1);
    CHECK_EQ(seqs[0], 2);
    teardown();
}

/* A flood of refusals at an owner that reads nothing: at most
 * STREAM_OWED_MAX owed, room kept for the in-flight Out's own Done. */
TEST(out_owed_refusals_bounded) {
    setup();
    make_handles();
    CHECK_EQ(send_h(0), 1);
    owner_full();
    for (int i = 0; i < 40; i++) CHECK_EQ(send_h(1 + i % 3) == -1 || so.n == 2, true);
    CHECK_EQ(so.n, 2u);                                 /* the first one past the head was queued */
    CHECK_EQ(so.owed_n, STREAM_OWED_MAX - 2u);
    CHECK(stream_out_done(&vm, port_at(sp), &so));      /* Done{1} owed, never dropped */
    CHECK(!stream_out_done(&vm, port_at(sp), &so));     /* Done{2} owed */
    CHECK_EQ(so.owed_n, STREAM_OWED_MAX);
    owner_room();
    CHECK(stream_out_flush(&vm, port_at(sp), &so));
    int32_t seqs[32];
    CHECK_EQ(dones(owner, seqs, 32), (int)STREAM_OWED_MAX);
    for (uint32_t i = 0; i + 2 < STREAM_OWED_MAX; i++) CHECK_EQ(seqs[i], -1);
    CHECK_EQ(seqs[STREAM_OWED_MAX - 2u], 1);
    CHECK_EQ(seqs[STREAM_OWED_MAX - 1u], 2);
    teardown();
}

/* Close while Dones are owed (and an Out is in flight): all discarded,
 * nothing delivered later, the data released. */
TEST(out_close_with_owed_dones) {
    setup();
    CHECK_EQ(send_out(owner, T4, 4), 1);
    CHECK_EQ(send_out(owner, T4, 4), 0);
    term running = so.held[1];
    owner_full();
    CHECK(stream_out_done(&vm, port_at(sp), &so));      /* Done{1} owed, the second running */
    CHECK_EQ(so.held[0], running);
    CHECK_EQ(send_out(owner, T4, 4), -1);               /* Done{-1} owed */
    CHECK_EQ(so.owed_n, 2u);
    CHECK(!is_free(running));
    port_close(&vm, sp);
    CHECK(is_free(running));
    CHECK_EQ(so.owed_n, 0u); CHECK_EQ(so.n, 0u); CHECK_EQ(so.next_seq, 0u);
    owner_room();
    CHECK_EQ(mbox_n(owner), 0);
    teardown();
}

/* An actor about to run f0 (Send(r2, r3)) with r2 = the port, r3 = Out{data}. */
static Actor *sender(term data_on_heap(Actor *)) {
    Actor *a = actor_new(&vm, 64);
    CHECK(heap_push_frame(&vm, a, 4, -1));
    a->pc = img.code;
    a->mem[a->fp + 2] = sp;
    a->mem[a->fp + 3] = out_of(a, data_on_heap(a));
    return a;
}
static term timings_on(Actor *a) { const uint8_t d[4] = { 16, 0, 32, 0 }; return heap_bytes(a, d, 4); }
static term live_const(Actor *a) { (void)a; return term_from_const(0); }
static term stale_const(Actor *a) { (void)a; return term_from_const(0) | mod_tag_make(6, 3); }

/* Through the interpreter (B_SEND to a port): Out{Bytes} is accepted and
 * copied off the sender's heap; Out{a live const} reaches the driver and
 * is refused (not Bytes); Out{a stale const} is the sender's badcode before
 * the driver sees it (the port-send gate). */
TEST(out_via_interp_and_stale_badcode) {
    setup();
    check = stream_timings_ok;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, sender(timings_on)), RUN_EXIT);
    CHECK_EQ(accepts, 1);
    CHECK_EQ(last_rc, 1);
    uint32_t len;
    const uint8_t *hd = stream_out_head(&vm, &so, &len);
    CHECK(hd && len == 4 && hd[0] == 16 && hd[2] == 32);
    CHECK(term_is_handle(so.held[0]));                 /* a port-held copy */
    CHECK_EQ(interp_run(&vm, sender(live_const)), RUN_EXIT);
    CHECK_EQ(accepts, 2);
    CHECK_EQ(last_rc, -1);
    CHECK_EQ(err_len, 0u);                             /* neither crashed */
    CHECK_EQ(interp_run(&vm, sender(stale_const)), RUN_EXIT);
    platform_unix_err_hook = NULL;
    CHECK(strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(accepts, 2);                              /* the driver never saw it */
    CHECK_EQ(so.n, 1u);
    int32_t seqs[4];
    CHECK_EQ(dones(owner, seqs, 4), 1);                /* the live const's Done{-1} */
    CHECK_EQ(seqs[0], -1);
    teardown();
}

/* A failure while Dones are owed is owed too, in order, never dropped. */
TEST(out_fail_owed_in_order) {
    setup();
    make_handles();
    owner_full();
    CHECK_EQ(send_h(0), 1);
    CHECK_EQ(send_h(1), 0);
    CHECK(stream_out_done(&vm, port_at(sp), &so));       /* Done{1} owed */
    CHECK(!stream_out_fail(&vm, port_at(sp), &so));      /* Done{-1} owed */
    CHECK_EQ(so.owed_n, 2u);
    owner_room();
    CHECK(stream_out_flush(&vm, port_at(sp), &so));
    int32_t seqs[4];
    CHECK_EQ(dones(owner, seqs, 4), 2);
    CHECK_EQ(seqs[0], 1); CHECK_EQ(seqs[1], -1);
    teardown();
}
