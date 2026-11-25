/* The imu port against a fake InputHw on a real VM: Config validation,
 * batching (size and timing), the gesture mask and values, RateHz 0 (no
 * FIFO reads), a slow consumer, one owner, close and reopen. Time is the
 * fake clock; each step runs port_drain, as the scheduler does. */
#include "test.h"
#include "fakehw.h"
#include "actor.h"
#include "signals.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "alloc.h"
#include "evq.h"
#include "bytes.h"
#include <string.h>

void platform_unix_set_clock(uint64_t (*fn)(void));
void platform_unix_arena_forget(void);

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

static const PortDriver *const drivers[] = { &input_kbd_driver, &input_power_driver, &input_imu_driver, NULL };

static void setup(void) {
    platform_unix_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    fake_reset();
    now = 0;
    platform_unix_set_clock(clock_fn);
    mini_image();
    CHECK_EQ(vm_init(&vm, &img, 16), 0);
    port_set_drivers(&vm, drivers);
    fake_install(&vm);
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
}

static void teardown(void) {
    port_close_all(&vm);
    platform_unix_set_clock(NULL);
}


static term heap_cfg(Actor *a, int rate, int mask, int batch) {
    uint32_t off = heap_alloc(&vm, a, 5, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(5, K_STRUCT);
    a->mem[off + 1] = TYPE_IMU_CONFIG;
    a->mem[off + 2] = term_from_int(rate);
    a->mem[off + 3] = term_from_int(mask);
    a->mem[off + 4] = term_from_int(batch);
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

static term open_imu(Actor *a, int rate, int mask, int batch) {
    a->hp = 0;
    term name = heap_str(a, "imu");
    return port_open(&vm, a, name, heap_cfg(a, rate, mask, batch));
}

static void step(uint64_t t) { now = t; port_drain(&vm, t); }

static int32_t word_int(const Msg *m, term t) {
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t));
    const uint32_t *o = &m->data[term_ref_off(t)];
    CHECK_EQ(o[0], hdr_make(2, K_INT));
    return (int32_t)o[1];
}

/* Everything the owner received, in order: a Chunk (kind 0) or a Gesture. */
typedef struct { int kind, value; int32_t seq, time, dropped; uint32_t len; uint8_t first[12]; } Ev;
static Ev got[64];
static int ngot;

static void collect(Actor *a) {
    ngot = 0;
    for (Msg *m = a->mbox_head; m; m = a->mbox_head) {
        Ev e;
        memset(&e, 0, sizeof e);
        if (m->data[1] == TYPE_CHUNK) {
            CHECK_EQ(m->data[0], hdr_make(6, K_STRUCT));
            e.seq = word_int(m, m->data[2]);
            e.time = word_int(m, m->data[3]);
            e.dropped = word_int(m, m->data[4]);
            if (m->data[5] != T_EMPTYBYTES) {
                const uint32_t *b = &m->data[term_ref_off(m->data[5])];
                e.len = b[1];
                memcpy(e.first, &b[2], e.len < 12 ? e.len : 12);
            }
        } else {
            CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
            CHECK_EQ(m->data[1], TYPE_IMU_GESTURE);
            e.kind = term_to_int(m->data[2]);
            e.value = term_to_int(m->data[3]);
        }
        if (ngot < 64) got[ngot++] = e;
        a->mbox_head = m->next;
        if (!a->mbox_head) a->mbox_tail = NULL;
        a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
        msg_free(&vm, m);
    }
}

TEST(imu_open_without_hw_fails) {
    setup();
    fake_no_imu();
    CHECK(!port_is(open_imu(owner, 100, 0, 200)));
    teardown();
}

TEST(imu_open_hw_refuses) {
    setup();
    fake_imu_open_fail(true);
    CHECK(!port_is(open_imu(owner, 100, 1, 200)));
    fake_imu_open_fail(false);
    CHECK(port_is(open_imu(owner, 100, 1, 200)));      /* the failed open left nothing behind */
    teardown();
}

TEST(imu_invalid_config) {
    setup();
    CHECK(!port_is(open_imu(owner, 10, 0, 200)));      /* rate not in {0,25,50,100,200} */
    CHECK(!port_is(open_imu(owner, -25, 0, 200)));
    CHECK(!port_is(open_imu(owner, 400, 0, 200)));
    CHECK(!port_is(open_imu(owner, 100, 256, 200)));   /* unknown gesture bit */
    CHECK(!port_is(open_imu(owner, 100, -1, 200)));
    CHECK_EQ(fake_imu_opens(), 0);                     /* the hardware was never touched */
    int rates[] = { 0, 25, 50, 100, 200 };
    for (int i = 0; i < 5; i++) {
        term p = open_imu(owner, rates[i], 255, 50);
        CHECK(port_is(p));
        port_close(&vm, p);
    }
    CHECK(port_is(open_imu(owner, 100, 0, 0)));        /* default batch */
    CHECK_EQ(fake_imu_rate(), 100);
    teardown();
}

/* BatchMs is clamped to 50..1000; 0 or less is 200. */
TEST(imu_batch_ms_clamped) {
    setup();
    uint64_t d;
    term p = open_imu(owner, 100, 0, 10);              /* -> 50: polls every 50 ms, a Chunk per poll */
    CHECK(port_is(p));
    fake_imu_push(5, 0);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    port_close(&vm, p);
    p = open_imu(owner, 25, 0, 5000);                  /* -> 1000: 300 B cap */
    CHECK(port_is(p));
    fake_imu_push(30, 0);
    step(1050);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 300u);
    port_close(&vm, p);
    p = open_imu(owner, 100, 0, -3);                   /* -> 200 */
    CHECK(port_is(p));
    fake_imu_push(5, 0);
    step(1050 + 150);
    collect(owner);
    CHECK_EQ(ngot, 0);
    step(1050 + 200);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK(port_next_deadline(&vm, &d));
    teardown();
}

TEST(imu_second_owner_fails_and_reopen) {
    setup();
    Actor *b = actor_new(&vm, 256);
    b->state = A_WAITING;
    term pp = open_imu(owner, 100, 0, 200);
    CHECK(port_is(pp));
    CHECK(!port_is(open_imu(b, 100, 0, 200)));
    port_close(&vm, pp);
    CHECK_EQ(fake_imu_closes(), 1);
    CHECK(port_is(open_imu(b, 50, 3, 100)));
    CHECK_EQ(fake_imu_opens(), 2);
    CHECK_EQ(fake_imu_rate(), 50);
    CHECK_EQ(fake_imu_mask(), 3);
    teardown();
}

TEST(imu_batching_timing) {
    setup();
    CHECK(port_is(open_imu(owner, 100, 0, 200)));      /* batch 200 ms: 240 B max, polled every 50 ms */
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 50);
    fake_imu_push(5, 10);
    step(49);
    CHECK_EQ(fake_imu_fifo_reads(), 0);                /* not due */
    step(50);
    CHECK_EQ(fake_imu_fifo_reads(), 1);
    CHECK_EQ(fake_imu_fifo_len(), 0);
    fake_imu_push(5, 100);
    step(100); step(150);
    collect(owner);
    CHECK_EQ(ngot, 0);                                 /* 200 ms not yet passed */
    fake_imu_push(2, 200);
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK_EQ(got[0].time, 0);                          /* when the batch began (open) */
    CHECK_EQ(got[0].len, 12u * 12u);
    CHECK_EQ(got[0].dropped, 0);
    CHECK_EQ(got[0].first[0], 10);                     /* first sample first, little-endian */
    CHECK_EQ(got[0].first[1], 0);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 250);
    step(250); step(300); step(350);
    collect(owner);
    CHECK_EQ(ngot, 0);                                 /* an empty batch is not sent */
    fake_imu_push(1, 7);
    step(400);                                         /* 200 ms after the last send, at 200 */
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 2);
    CHECK_EQ(got[0].time, 200);
    CHECK_EQ(got[0].len, 12u);
    teardown();
}

TEST(imu_batch_full_sends_early) {
    setup();
    CHECK(port_is(open_imu(owner, 25, 0, 1000)));      /* 25 * 12 = 300 B per second: poll at 50 ms */
    fake_imu_push(30, 0);                              /* more than one batch (25 samples) */
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 300u);
    CHECK_EQ(fake_imu_fifo_len(), 0);                  /* the rest was read into the next batch */
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    step(1050);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 60u);
    CHECK_EQ(got[0].seq, 2);
    teardown();
}

TEST(imu_batch_cap_2400) {
    setup();
    CHECK(port_is(open_imu(owner, 200, 0, 1000)));     /* 200 * 12 = 2400 B exactly */
    fake_imu_push(250, 0);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 2400u);
    CHECK_EQ(fake_imu_fifo_len(), 0);
    step(1050);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 600u);                        /* the 50 samples that did not fit */
    CHECK_EQ(got[0].seq, 2);
    teardown();
}

TEST(imu_poll_period_follows_batch) {
    setup();
    CHECK(port_is(open_imu(owner, 100, 0, 50)));
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 50);
    fake_imu_push(5, 0);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);                                 /* BatchMs 50: one Chunk per poll */
    CHECK_EQ(got[0].len, 60u);
    teardown();
}

TEST(imu_rate0_never_reads_fifo) {
    setup();
    fake_imu_push(3, 0);
    CHECK(port_is(open_imu(owner, 0, 1, 200)));
    for (uint64_t t = 50; t <= 1000; t += 50) step(t);
    CHECK_EQ(fake_imu_fifo_reads(), 0);
    CHECK(fake_imu_gesture_reads() > 0);
    fake_imu_gesture(1, 0, 0);
    step(1050);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].kind, 1);
    teardown();
}

TEST(imu_gesture_mask_and_values) {
    setup();
    CHECK(port_is(open_imu(owner, 0, 2 | 8 | 128, 200)));   /* DoubleTap, Orientation, Step */
    fake_imu_gesture(1 | 2 | 8 | 16 | 128, 5, 4242);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 3);                                 /* Tap and Flat filtered out */
    CHECK_EQ(got[0].kind, 2);   CHECK_EQ(got[0].value, 0);
    CHECK_EQ(got[1].kind, 8);   CHECK_EQ(got[1].value, 5);
    CHECK_EQ(got[2].kind, 128); CHECK_EQ(got[2].value, 4242);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);                                 /* cleared by the read */
    teardown();
}

TEST(imu_no_mask_never_reads_gestures) {
    setup();
    CHECK(port_is(open_imu(owner, 100, 0, 50)));
    fake_imu_gesture(255, 1, 1);
    step(50); step(100);
    CHECK_EQ(fake_imu_gesture_reads(), 0);
    teardown();
}

TEST(imu_fifo_error_retries) {
    setup();
    CHECK(port_is(open_imu(owner, 100, 0, 50)));
    fake_imu_push(2, 3);
    fake_imu_fifo_fail(true);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_imu_fifo_fail(false);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, 24u);
    teardown();
}

/* A consumer that reads nothing is not killed; its Chunks
 * drop and the Seq gap and Dropped show it. */
TEST(imu_slow_consumer) {
    setup();
    owner->max_words = 400;
    CHECK(port_is(open_imu(owner, 100, 0, 50)));
    int polls = 40;
    for (int i = 1; i <= polls; i++) {
        fake_imu_push(5, (int16_t)i);
        step((uint64_t)i * 50);
    }
    CHECK(!(owner->flags & AF_EXIT_PENDING));
    collect(owner);
    CHECK(ngot > 0 && ngot < polls);
    int32_t last = 0;
    for (int i = 0; i < ngot; i++) {
        CHECK(got[i].seq > last);
        CHECK_EQ(got[i].dropped, got[i].seq - last - 1);   /* the gap is exactly what Dropped says */
        last = got[i].seq;
    }
    /* Room again: the next one carries the loss. */
    fake_imu_push(5, 99);
    step((uint64_t)(polls + 1) * 50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, polls + 1);
    CHECK_EQ(got[0].dropped, polls - last);
    teardown();
}

TEST(imu_close_stops_polling_and_reopen_resets) {
    setup();
    term pp = open_imu(owner, 100, 0, 50);
    CHECK(port_is(pp));
    fake_imu_push(2, 0);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    port_close(&vm, pp);
    CHECK_EQ(fake_imu_closes(), 1);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(!port_work_pending(&vm));
    fake_imu_push(2, 0);
    step(1000);
    CHECK_EQ(fake_imu_fifo_len(), 2);                  /* nobody reads it */
    pp = open_imu(owner, 100, 0, 50);
    CHECK(port_is(pp));
    step(1050);
    collect(owner);
    CHECK_EQ(ngot, 0);                                 /* the fake flushed its FIFO in open */
    fake_imu_push(1, 0);
    step(1100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);                           /* Seq starts again */
    CHECK_EQ(got[0].time, 1050);
    teardown();
}

TEST(imu_can_event_follows_live) {
    setup();
    fake_live_hook(true);
    Port p;
    memset(&p, 0, sizeof p);
    CHECK(input_imu_driver.can_event(&vm, &p));
    fake_set_live(false);
    CHECK(!input_imu_driver.can_event(&vm, &p));
    teardown();
}

/* The buffer is VM memory, charged to the owner, and goes back on owner exit. */
TEST(imu_buffer_charged_and_freed) {
    setup();
    Actor *big = actor_new(&vm, 2048);
    CHECK(big != NULL);
    big->state = A_WAITING;
    uint32_t charged = actor_charged_words(big);
    CHECK(port_is(open_imu(big, 200, 0, 1000)));       /* 2400 B = 600 words */
    size_t used = va_used_bytes(&vm);
    CHECK_EQ(actor_charged_words(big), charged + 600u);
    port_close_owned(&vm, big->id);                    /* the owner exits */
    CHECK(va_used_bytes(&vm) + 2400 <= used);             /* the buffer went back */
    CHECK_EQ(actor_charged_words(big), charged);
    CHECK_EQ(fake_imu_closes(), 1);
    /* An owner that cannot afford it is refused, and nothing is left behind. */
    owner->max_words = 400;
    size_t before = va_used_bytes(&vm);
    CHECK(!port_is(open_imu(owner, 200, 0, 1000)));    /* owner: 256 words */
    CHECK(va_used_bytes(&vm) <= before + 64);            /* only the Config struct on the heap */
    CHECK_EQ(actor_charged_words(owner), 0u);
    CHECK_EQ(fake_imu_closes(), 2);                    /* the hardware was stopped again */
    CHECK(port_is(open_imu(owner, 0, 1, 200)));        /* RateHz 0: no buffer, no charge */
    CHECK_EQ(actor_charged_words(owner), 0u);
    teardown();
}

/* Hooks the board lacks refuse only the Config that needs them. */
TEST(imu_partial_hooks) {
    setup();
    fake_imu_no_fifo();
    CHECK(!port_is(open_imu(owner, 100, 0, 200)));
    CHECK(port_is(open_imu(owner, 0, 1, 200)));
    teardown();
}
