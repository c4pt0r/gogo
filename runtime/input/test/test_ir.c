/* The ir_tx and ir_rx ports against the fake devices (fakehw.c) on a real VM: Config
 * validation and defaults, one owner, Out -> start -> busy -> Done, the
 * queue and the third Out, bad timings, waiting for an idle transmitter,
 * a failed start, close with a frame in flight, the busy-poll deadline
 * (also with dead hardware); frames to Chunks, the 256-value cut counted in
 * Dropped, value clamping, read errors, the charged frame buffer. Time is
 * the fake clock; each step runs port_drain, as the scheduler does. */
#include "test.h"
#include "test_host.h"
#include "fakehw.h"
#include "actor.h"
#include "signals.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "evq.h"
#include "bytes.h"
#include "stream.h"
#include <string.h>


static uint64_t now;
static uint64_t clock_fn(void) { return now; }

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm;
static Actor *owner;

static void mini_image(void) {
    uint32_t *w = img_w;
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
}

static const PortDriver *const drivers[] = {
    &input_kbd_driver, &input_power_driver, &input_imu_driver, &input_ir_tx_driver, &input_ir_rx_driver, NULL
};

static void setup(void) {
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    fake_reset();
    now = 0;
    test_host_set_clock(clock_fn);
    mini_image();
    CHECK_EQ(test_vm_init(&vm, &img, 16), 0);
    port_set_drivers(&vm, drivers);
    fake_install(&vm);
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
}

static void teardown(void) {
    port_close_all(&vm);
    test_host_set_clock(NULL);
}

static term heap_struct(Actor *a, uint32_t type, uint32_t n, const term *f) {
    uint32_t off = heap_alloc(&vm, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term heap_str(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

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

static term open_tx(Actor *a, int carrier, int duty) {
    a->hp = 0;
    term name = heap_str(a, "ir_tx");
    term f[2] = { term_from_int(carrier), term_from_int(duty) };
    return port_open(&vm, a, name, heap_struct(a, TYPE_IR_TX, 2, f));
}

static term open_rx(Actor *a, int gap) {
    a->hp = 0;
    term name = heap_str(a, "ir_rx");
    term f[1] = { term_from_int(gap) };
    return port_open(&vm, a, name, heap_struct(a, TYPE_IR_RX, 1, f));
}

/* Sends Out{timings v[0..n)} (uint16 LE) from a to port tx. */
static void send_out(Actor *a, term tx, const uint16_t *v, uint32_t n) {
    static uint8_t b[600];
    for (uint32_t i = 0; i < n; i++) { b[2 * i] = (uint8_t)(v[i] & 0xFF); b[2 * i + 1] = (uint8_t)(v[i] >> 8); }
    a->hp = 0;
    term d = heap_bytes(a, b, n * 2u);
    port_command(&vm, a, tx, heap_struct(a, TYPE_OUT, 1, &d));
}

static void step(uint64_t t) { now = t; port_drain(&vm, t); }

static int32_t word_int(const Msg *m, term t) {
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t));
    const uint32_t *o = &m->data[term_ref_off(t)];
    CHECK_EQ(o[0], hdr_make(2, K_INT));
    return (int32_t)o[1];
}

/* Everything a received, in order: a Done (len -1, seq) or a Chunk. */
typedef struct { int32_t seq, time, dropped; int len; uint8_t data[600]; } Ev;
static Ev got[16];
static int ngot;

static void collect(Actor *a) {
    ngot = 0;
    for (Msg *m = a->mbox_head; m; m = a->mbox_head) {
        Ev *e = &got[ngot < 16 ? ngot : 15];
        memset(e, 0, sizeof *e);
        if (m->data[1] == TYPE_DONE) {
            CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
            CHECK(term_is_pid(m->data[2]));   /* From */
            e->len = -1;
            e->seq = word_int(m, m->data[3]);
        } else {
            CHECK_EQ(m->data[0], hdr_make(7, K_STRUCT));
            CHECK_EQ(m->data[1], TYPE_CHUNK);
            CHECK(term_is_pid(m->data[2]));   /* From */
            e->seq = word_int(m, m->data[3]);
            e->time = word_int(m, m->data[4]);
            e->dropped = word_int(m, m->data[5]);
            if (m->data[6] != T_EMPTYBYTES) {
                const uint32_t *b = &m->data[term_ref_off(m->data[6])];
                e->len = (int)b[1];
                memcpy(e->data, &b[2], b[1] < sizeof e->data ? b[1] : sizeof e->data);
            }
        }
        if (ngot < 16) ngot++;
        a->mbox_head = m->next;
        if (!a->mbox_head) a->mbox_tail = NULL;
        a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
        msg_free(&vm, m);
    }
}

static int le16(const Ev *e, int i) { return e->data[2 * i] | e->data[2 * i + 1] << 8; }

static const uint16_t F3[3] = { 9000, 4500, 560 };

/* ---- ir_tx ---- */

TEST(ir_open_without_hw_fails) {
    setup();
    fake_no_ir();
    CHECK(!test_port_is(open_tx(owner, 38000, 33)));
    CHECK(!test_port_is(open_rx(owner, 10000)));
    teardown();
}

TEST(ir_tx_config) {
    setup();
    CHECK(!test_port_is(open_tx(owner, 5000, 33)));          /* carrier 10000..500000 */
    CHECK(!test_port_is(open_tx(owner, 600000, 33)));
    CHECK(!test_port_is(open_tx(owner, 38000, 100)));        /* duty 1..99 */
    term tx = open_tx(owner, 0, 0);                     /* defaults 38000 / 33 */
    CHECK(test_port_is(tx));
    send_out(owner, tx, F3, 3);
    step(0);
    CHECK_EQ(fake_ir_tx_starts(), 1);
    CHECK_EQ(fake_ir_tx_carrier(), 38000u);
    CHECK_EQ(fake_ir_tx_duty(), 33);
    port_close(&vm, tx);
    tx = open_tx(owner, 40000, 50);
    CHECK(test_port_is(tx));
    send_out(owner, tx, F3, 3);
    step(100);
    CHECK_EQ(fake_ir_tx_carrier(), 40000u);
    CHECK_EQ(fake_ir_tx_duty(), 50);
    teardown();
}

TEST(ir_tx_one_owner) {
    setup();
    Actor *b = actor_new(&vm, 256);
    CHECK(b != NULL);
    term tx = open_tx(owner, 38000, 33);
    CHECK(test_port_is(tx));
    CHECK(!test_port_is(open_tx(b, 38000, 33)));
    port_close(&vm, tx);
    CHECK(test_port_is(open_tx(b, 38000, 33)));
    teardown();
}

/* Out -> ir_tx_start (values copied) -> busy until done -> Done{1}; the
 * second is queued and starts when the first finishes; a third is refused. */
TEST(ir_tx_out_done_queue) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    CHECK(test_port_is(tx));
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));                /* idle: no poll */
    CHECK(!input_ir_tx_driver.can_event(&vm, port_at(&vm, term_pid_slot(tx) - PORT_SLOT_BASE)));
    fake_ir_tx_set_busy(true);
    send_out(owner, tx, F3, 3);
    static const uint16_t G[2] = { 1000, 1000 };
    send_out(owner, tx, G, 2);
    send_out(owner, tx, G, 2);                          /* third in flight: Done{-1} */
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, -1);
    CHECK_EQ(got[0].seq, -1);
    CHECK(input_ir_tx_driver.can_event(&vm, port_at(&vm, term_pid_slot(tx) - PORT_SLOT_BASE)));
    step(0);
    CHECK_EQ(fake_ir_tx_starts(), 0);                   /* busy: waits (a frame of before) */
    fake_ir_tx_set_busy(false);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 5u);
    step(5);
    CHECK_EQ(fake_ir_tx_starts(), 1);
    CHECK(!fake_ir_tx_started_while_busy());
    CHECK_EQ(fake_ir_tx_n(), 3);
    CHECK_EQ(fake_ir_tx_value(0), 9000);
    CHECK_EQ(fake_ir_tx_value(2), 560);
    fake_ir_tx_set_busy(true);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 5u + 15u);                              /* 14060 us: polled when it should end */
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 0);                                  /* still busy */
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 25u);                                   /* then every 5 ms */
    fake_ir_tx_set_busy(false);
    step(25);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK_EQ(fake_ir_tx_starts(), 2);                   /* the queued one started at once */
    CHECK_EQ(fake_ir_tx_n(), 2);
    step(27);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 2);
    CHECK(!port_next_deadline(&vm, &d));                /* nothing in flight */
    CHECK(!input_ir_tx_driver.can_event(&vm, port_at(&vm, term_pid_slot(tx) - PORT_SLOT_BASE)));
    teardown();
}

TEST(ir_tx_bad_timings_refused) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    static const uint16_t zero[2] = { 560, 0 }, big[2] = { 32768, 560 };
    send_out(owner, tx, zero, 2);
    send_out(owner, tx, big, 2);
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, -1);
    CHECK_EQ(got[1].seq, -1);
    CHECK_EQ(fake_ir_tx_starts(), 0);
    send_out(owner, tx, F3, 3);                         /* Seq counts accepted Outs only */
    step(1);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    teardown();
}

TEST(ir_tx_start_failure_is_done_minus_1) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    fake_ir_tx_fail(true);
    send_out(owner, tx, F3, 3);
    send_out(owner, tx, F3, 3);
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, -1);                           /* not sent: told apart from a sent one */
    CHECK_EQ(got[1].seq, -1);
    CHECK_EQ(fake_ir_tx_fails(), 2);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    fake_ir_tx_fail(false);
    send_out(owner, tx, F3, 3);                         /* failures use no Seq */
    step(1);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    teardown();
}

/* At most IR_FRAME_MAX (256) values per Out, the receiver's limit: a
 * received frame can always be sent back. */
TEST(ir_tx_frame_cap_256) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    static uint16_t v[257];
    for (int i = 0; i < 257; i++) v[i] = 100;
    send_out(owner, tx, v, 257);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, -1);
    send_out(owner, tx, v, 256);
    step(0);
    CHECK_EQ(fake_ir_tx_starts(), 1);
    CHECK_EQ(fake_ir_tx_n(), 256);
    step(26);                                           /* 25600 us */
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    teardown();
}

/* Close with Outs in flight: no Done, the held data goes; the next owner's
 * frame waits until the hardware finished the old one. */
TEST(ir_tx_close_in_flight) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    send_out(owner, tx, F3, 3);
    send_out(owner, tx, F3, 3);
    step(0);
    CHECK_EQ(fake_ir_tx_starts(), 1);
    fake_ir_tx_set_busy(true);
    CHECK_EQ(bytes_held_count(owner), 2u);
    port_close(&vm, tx);
    CHECK_EQ(bytes_held_count(owner), 0u);
    collect(owner);
    CHECK_EQ(ngot, 0);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    tx = open_tx(owner, 38000, 33);
    CHECK(test_port_is(tx));
    send_out(owner, tx, F3, 3);
    step(10);
    CHECK_EQ(fake_ir_tx_starts(), 1);                   /* still sending the old frame */
    fake_ir_tx_set_busy(false);
    step(15);
    CHECK_EQ(fake_ir_tx_starts(), 2);
    CHECK(!fake_ir_tx_started_while_busy());
    step(30);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);                            /* Seq starts again */
    teardown();
}

/* A frame being sent finishes even when the hardware is otherwise dead
 * (the unix script hit EOF): its deadline is still given. */
TEST(ir_tx_done_with_dead_hardware) {
    setup();
    fake_live_hook(false);
    term tx = open_tx(owner, 38000, 33);
    send_out(owner, tx, F3, 3);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    step(0);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 15u);
    step(15);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    teardown();
}

/* A Done the owner has no room for is owed, then sent by a later poll. */
TEST(ir_tx_owed_done_retried) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    send_out(owner, tx, F3, 3);
    step(0);
    owner->state = A_RUNNABLE;                          /* no slack to reclaim */
    owner->max_words = 1;                               /* the mailbox is full */
    step(15);
    CHECK(owner->mbox_head == NULL);                    /* owed */
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 35u);
    owner->max_words = 0;
    step(35);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK(!port_next_deadline(&vm, &d));
    teardown();
}

/* ---- ir_rx ---- */

TEST(ir_rx_config_and_frame) {
    setup();
    term rx = open_rx(owner, 0);
    CHECK(test_port_is(rx));
    CHECK_EQ(fake_ir_rx_gap(), 10000u);                 /* default */
    CHECK(!test_port_is(open_rx(owner, 5000)));              /* one owner */
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 20u);
    fake_ir_rx_push(F3, 3);
    step(10);
    collect(owner);
    CHECK_EQ(ngot, 0);                                  /* polled every 20 ms */
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK_EQ(got[0].time, 20);
    CHECK_EQ(got[0].dropped, 0);
    CHECK_EQ(got[0].len, 6);
    CHECK_EQ(le16(&got[0], 0), 9000);
    CHECK_EQ(le16(&got[0], 1), 4500);
    CHECK_EQ(le16(&got[0], 2), 560);
    CHECK(stream_timings_ok(NULL, got[0].data, 6));     /* can be sent back as it is */
    port_close(&vm, rx);
    CHECK_EQ(fake_ir_rx_closes(), 1);
    CHECK(test_port_is(open_rx(owner, 40000)));
    CHECK_EQ(fake_ir_rx_gap(), 32767u);                 /* clamped */
    teardown();
}

TEST(ir_rx_cut_at_256_counts_dropped) {
    setup();
    term rx = open_rx(owner, 10000);
    CHECK(test_port_is(rx));
    static uint16_t v[300];
    for (int i = 0; i < 300; i++) v[i] = (uint16_t)(100 + i);
    fake_ir_rx_push(v, 300);
    fake_ir_rx_push(v, 256);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].len, 512);
    CHECK_EQ(got[0].dropped, 1);
    CHECK_EQ(le16(&got[0], 255), 355);
    CHECK_EQ(got[1].len, 512);
    CHECK_EQ(got[1].dropped, 0);
    CHECK_EQ(got[1].seq, 2);
    teardown();
}

TEST(ir_rx_values_clamped) {
    setup();
    CHECK(test_port_is(open_rx(owner, 10000)));
    static const uint16_t v[3] = { 0, 40000, 32767 };
    fake_ir_rx_push(v, 3);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(le16(&got[0], 0), 1);
    CHECK_EQ(le16(&got[0], 1), 32767);
    CHECK_EQ(le16(&got[0], 2), 32767);
    teardown();
}

TEST(ir_rx_frames_per_poll_and_errors) {
    setup();
    CHECK(test_port_is(open_rx(owner, 10000)));
    fake_ir_rx_fail(true);
    fake_ir_rx_push(F3, 3);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_ir_rx_fail(false);
    fake_ir_rx_push(F3, 2);
    fake_ir_rx_push(F3, 1);
    step(40);                                           /* retried; every waiting frame */
    collect(owner);
    CHECK_EQ(ngot, 3);
    CHECK_EQ(got[0].len, 6);
    CHECK_EQ(got[1].len, 4);
    CHECK_EQ(got[2].len, 2);
    teardown();
}

TEST(ir_rx_buffer_charged_and_freed) {
    setup();
    uint32_t charged = actor_charged_words(owner);
    size_t used0 = va_used_bytes(&vm);
    term rx = open_rx(owner, 10000);
    CHECK(test_port_is(rx));
    CHECK_EQ(actor_charged_words(owner), charged + 128u);
    CHECK(va_used_bytes(&vm) >= used0 + 512u);
    size_t used = va_used_bytes(&vm);
    port_close_owned(&vm, owner->id);                   /* the owner exits */
    CHECK(va_used_bytes(&vm) + 512u <= used);
    CHECK_EQ(actor_charged_words(owner), charged);
    CHECK_EQ(fake_ir_rx_closes(), 1);
    /* Refused hardware: nothing left behind. */
    fake_ir_rx_open_fail(true);
    size_t before = va_used_bytes(&vm);
    CHECK(!test_port_is(open_rx(owner, 10000)));
    CHECK(va_used_bytes(&vm) <= before + 64);
    fake_ir_rx_open_fail(false);
    fake_no_ir_rx_close();                              /* optional */
    rx = open_rx(owner, 10000);
    CHECK(test_port_is(rx));
    port_close(&vm, rx);
    teardown();
}

TEST(ir_rx_can_event_follows_live) {
    setup();
    fake_live_hook(true);
    Port p;
    memset(&p, 0, sizeof p);
    p.drv = &input_ir_rx_driver;
    CHECK(input_ir_rx_driver.can_event(&vm, &p));
    fake_set_live(false);
    CHECK(!input_ir_rx_driver.can_event(&vm, &p));
    term rx = open_rx(owner, 10000);
    CHECK(test_port_is(rx));
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));                /* dead: no receive poll */
    teardown();
}

/* Owed Dones are forgotten on close: none reaches the next owner. */
TEST(ir_tx_close_with_owed_dones) {
    setup();
    term tx = open_tx(owner, 38000, 33);
    send_out(owner, tx, F3, 3);
    step(0);
    owner->state = A_RUNNABLE;
    owner->max_words = 1;
    step(15);                                           /* Done{1} owed */
    CHECK(owner->mbox_head == NULL);
    port_close(&vm, tx);
    owner->max_words = 0;
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    tx = open_tx(owner, 38000, 33);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

/* An Out from another actor: the owner gets its Done. Handoff while a
 * frame runs: its Done goes to the new owner. */
TEST(ir_tx_non_owner_and_handoff) {
    setup();
    Actor *b = actor_new(&vm, 256);
    CHECK(b != NULL);
    b->state = A_WAITING;
    term tx = open_tx(owner, 38000, 33);
    send_out(b, tx, F3, 3);
    step(0);
    step(15);
    collect(b);
    CHECK_EQ(ngot, 0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    send_out(owner, tx, F3, 3);
    step(20);
    CHECK_EQ(fake_ir_tx_starts(), 2);
    CHECK(port_handoff(&vm, owner, tx, b->id));
    step(35);
    collect(owner);
    CHECK_EQ(ngot, 0);
    collect(b);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 2);
    teardown();
}

/* A hook that knows when the frame began gives TimeMs. */
TEST(ir_rx_time_from_hook) {
    setup();
    CHECK(test_port_is(open_rx(owner, 10000)));
    fake_ir_rx_start(7);
    fake_ir_rx_push(F3, 3);
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].time, 7);
    teardown();
}

/* A host clock ahead of VM time (t0 != 0): the poll schedule is VM time and
 * the hook's host-clock frame start gets -t0 exactly once. */
TEST(ir_rx_time_with_t0) {
    setup();
    vm.t0 = 1000;
    now = 1000;                                 /* host 1000 = VM 0 */
    CHECK(test_port_is(open_rx(owner, 10000)));      /* first poll at VM 20 */
    fake_ir_rx_start(1007);                     /* host clock */
    fake_ir_rx_push(F3, 3);
    now = 1019;
    port_drain(&vm, 19);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(fake_ir_rx_reads(), 0);
    now = 1020;
    port_drain(&vm, 20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].time, 7);
    teardown();
}

/* The optional ir_tx_open / ir_tx_close hooks bracket the port: open (with
 * the port's carrier and duty, defaults applied) before any start, close
 * after the last; once per port, also when the owner exits. */
TEST(ir_tx_open_close_hooks_order) {
    setup();
    term tx = open_tx(owner, 0, 0);
    CHECK(test_port_is(tx));
    CHECK(strcmp(fake_ir_tx_log(), "O") == 0);
    CHECK_EQ(fake_ir_tx_open_carrier(), 38000u);
    CHECK_EQ(fake_ir_tx_open_duty(), 33);
    send_out(owner, tx, F3, 3);
    step(0);
    step(15);
    port_close(&vm, tx);
    CHECK(strcmp(fake_ir_tx_log(), "OSC") == 0);
    port_close(&vm, tx);                                /* stale: no second close */
    CHECK(strcmp(fake_ir_tx_log(), "OSC") == 0);
    tx = open_tx(owner, 40000, 50);
    CHECK(test_port_is(tx));
    CHECK_EQ(fake_ir_tx_open_carrier(), 40000u);
    CHECK_EQ(fake_ir_tx_open_duty(), 50);
    port_close_owned(&vm, owner->id);                   /* the owner exits */
    CHECK(strcmp(fake_ir_tx_log(), "OSCOC") == 0);
    teardown();
}

/* A failing ir_tx_open makes the open fail, with no close and no start; a
 * bad config never reaches the hook. */
TEST(ir_tx_open_hook_failure) {
    setup();
    CHECK(!test_port_is(open_tx(owner, 5000, 33)));          /* refused before the hook */
    CHECK(strcmp(fake_ir_tx_log(), "") == 0);
    fake_ir_tx_open_fail(true);
    CHECK(!test_port_is(open_tx(owner, 38000, 33)));
    CHECK(strcmp(fake_ir_tx_log(), "") == 0);
    fake_ir_tx_open_fail(false);
    term tx = open_tx(owner, 38000, 33);
    CHECK(test_port_is(tx));
    send_out(owner, tx, F3, 3);
    step(0);
    port_close(&vm, tx);
    CHECK(strcmp(fake_ir_tx_log(), "OSC") == 0);
    teardown();
}

/* Without the optional hooks ir_tx works as before. */
TEST(ir_tx_without_open_close_hooks) {
    setup();
    fake_no_ir_tx_open_close();
    term tx = open_tx(owner, 38000, 33);
    CHECK(test_port_is(tx));
    send_out(owner, tx, F3, 3);
    step(0);
    CHECK_EQ(fake_ir_tx_starts(), 1);
    CHECK(strcmp(fake_ir_tx_log(), "S") == 0);
    port_close(&vm, tx);
    teardown();
}
