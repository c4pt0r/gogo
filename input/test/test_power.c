/* The power port against a fake InputHw on a real VM: open rules, the
 * period, percent and smoothing, the report rules (open, Read, state
 * change, 2-point threshold), failed reads, and power_pending. Time is the
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

static const PortDriver *const drivers[] = { &input_kbd_driver, &input_power_driver, NULL };

static void setup(void) {
    platform_unix_arena_forget();
    va_reset();
    evq_reset();
    bytes_reset();
    fake_reset();
    now = 0;
    platform_unix_set_clock(clock_fn);
    port_set_drivers(drivers);
    fake_install_once();
    mini_image();
    CHECK_EQ(vm_init(&vm, &img, 16), 0);
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
}

static void teardown(void) {
    port_close_all(&vm);
    platform_unix_set_clock(NULL);
    port_set_drivers(NULL);
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

static term heap_cfg(Actor *a, int period) {
    uint32_t off = heap_alloc(&vm, a, 3, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(3, K_STRUCT);
    a->mem[off + 1] = TYPE_POWER_CONFIG;
    a->mem[off + 2] = term_from_int(period);
    return term_from_ref(off);
}

static term open_power(Actor *a, int period) {
    a->hp = 0;
    return port_open(&vm, a, heap_str(a, "power"), heap_cfg(a, period));
}

static void send_read(Actor *a, term pp) {
    a->hp = 0;
    uint32_t off = heap_alloc(&vm, a, 2, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2, K_STRUCT);
    a->mem[off + 1] = TYPE_POWER_READ;
    port_command(&vm, a, pp, term_from_ref(off));
}

static void step(uint64_t t) { now = t; port_drain(&vm, t); }

typedef struct { int32_t mv, pct, charging, external; } Bat;
static Bat got[64];
static int ngot;

static void collect(Actor *a) {
    ngot = 0;
    for (Msg *m = a->mbox_head; m; m = a->mbox_head) {
        CHECK_EQ(m->data[0], hdr_make(6, K_STRUCT));
        CHECK_EQ(m->data[1], TYPE_POWER_BATTERY);
        if (ngot < 64) {
            got[ngot].mv = term_to_int(m->data[2]);
            got[ngot].pct = term_to_int(m->data[3]);
            got[ngot].charging = term_to_int(m->data[4]);
            got[ngot].external = term_to_int(m->data[5]);
            ngot++;
        }
        a->mbox_head = m->next;
        if (!a->mbox_head) a->mbox_tail = NULL;
        a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(m));
        msg_free(m);
    }
}

static void is(int i, int mv, int pct, int ch, int ext) {
    CHECK(i < ngot);
    if (i >= ngot) return;
    CHECK_EQ(got[i].mv, mv);
    CHECK_EQ(got[i].pct, pct);
    CHECK_EQ(got[i].charging, ch);
    CHECK_EQ(got[i].external, ext);
}

/* Runs `n` polls, 10 s apart from `t`, returns the next time. */
static uint64_t polls(uint64_t t, int n) {
    for (int i = 0; i < n; i++, t += 10000) step(t);
    return t;
}

TEST(power_open_without_hw_fails) {
    setup();
    fake_no_power();
    CHECK(!port_is(open_power(owner, 0)));
    teardown();
}

TEST(power_second_owner_fails) {
    setup();
    Actor *b = actor_new(&vm, 256);
    b->state = A_WAITING;
    term pp = open_power(owner, 0);
    CHECK(port_is(pp));
    CHECK(!port_is(open_power(b, 0)));
    port_close(&vm, pp);
    CHECK(port_is(open_power(b, 0)));
    teardown();
}

TEST(power_period_clamp) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    CHECK_EQ(fake_power_reads(), 1);
    step(9999);
    CHECK_EQ(fake_power_reads(), 1);
    step(10000);
    CHECK_EQ(fake_power_reads(), 2);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 20000);
    port_close_all(&vm);
    fake_reset();
    CHECK(port_is(open_power(owner, 200)));
    step(100000);
    CHECK_EQ(fake_power_reads(), 1);
    step(100999);
    CHECK_EQ(fake_power_reads(), 1);
    step(101000);
    CHECK_EQ(fake_power_reads(), 2);
    teardown();
}

TEST(power_open_reports_now) {
    setup();
    fake_power_set(3700, 1, 1);
    CHECK(port_is(open_power(owner, 0)));
    CHECK(port_work_pending(&vm));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 1, 1);
    teardown();
}

TEST(power_percent_clamp) {
    setup();
    fake_power_set(3200, 0, 0);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3200, 0, 0, 0);
    fake_power_set(4200, 0, 0);
    polls(10000, 4);                       /* the average catches up */
    collect(owner);
    CHECK(ngot > 0);
    is(ngot - 1, 4200, 100, 0, 0);
    teardown();
}

TEST(power_smoothing) {
    setup();
    term pp = open_power(owner, 0);
    CHECK(port_is(pp));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 0);
    step(10000); step(20000);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_power_set(3740, 0, 0);
    send_read(owner, pp);                  /* Read: a sample and a report (3710 is only 51%) */
    step(30000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3710, 51, 0, 0);
    teardown();
}

TEST(power_threshold) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    fake_power_set(3708, 0, 0);            /* 51% */
    uint64_t t = polls(10000, 8);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_power_set(3716, 0, 0);            /* 52% */
    polls(t, 8);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3716, 52, 0, 0);
    teardown();
}

TEST(power_flap_no_storm) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    uint64_t t = 0;
    for (int i = 0; i < 100; i++, t += 10000) {
        fake_power_set(i % 2 ? 3701 : 3699, 0, 0);
        step(t);
    }
    collect(owner);
    CHECK_EQ(ngot, 1);                     /* the open event, nothing after */
    teardown();
}

TEST(power_state_change_immediate) {
    setup();
    fake_power_set(3700, 1, 1);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    step(10000);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_power_set(3700, 1, 0);
    step(20000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 1, 0);
    fake_power_set(3700, 0, 0);
    step(30000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 0);
    teardown();
}

TEST(power_read_command) {
    setup();
    term pp = open_power(owner, 0);
    CHECK(port_is(pp));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    send_read(owner, pp);
    CHECK(port_work_pending(&vm));
    step(100);                             /* long before the period */
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 0);
    step(101);
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

TEST(power_unknown_states) {
    setup();
    fake_power_set(3700, -1, -1);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, -1, -1);
    teardown();
}

/* R1: a failed read is no sample: no event, retry at the next period; the
 * first good sample fills the average and reports. */
TEST(power_failed_read_is_no_sample) {
    setup();
    fake_power_fail(true);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(fake_power_reads(), 1);
    step(5000);                            /* the retry waits for the period */
    CHECK_EQ(fake_power_reads(), 1);
    fake_power_fail(false);
    fake_power_set(3900, 0, 0);
    step(10000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3900, 75, 0, 0);                 /* not blended with anything */
    fake_power_fail(true);
    step(20000);                           /* a later failure changes nothing */
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

/* R2: power_pending true = sample now instead of waiting for the period. */
TEST(power_pending_samples_early) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK(!port_work_pending(&vm));
    fake_power_set(3700, 1, 0);
    fake_power_new();
    CHECK(port_work_pending(&vm));
    CHECK(port_work_pending(&vm));         /* a pure read */
    step(50);
    CHECK_EQ(fake_power_reads(), 2);
    collect(owner);
    CHECK_EQ(ngot, 1);                     /* Charging changed: reported now */
    is(0, 3700, 50, 1, 0);
    CHECK(!port_work_pending(&vm));        /* read cleared the platform flag */
    step(51);
    CHECK_EQ(fake_power_reads(), 2);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 10050);                    /* the period restarts from the early sample */
    teardown();
}

TEST(power_full_mailbox_drops) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner));
    uint32_t d0 = input_dropped();
    step(0);
    CHECK_EQ(input_dropped() - d0, 1);
    CHECK(owner->state != A_EXITED && !(owner->flags & AF_EXIT_PENDING));
    teardown();
}

TEST(power_close_reopen_state) {
    setup();
    term pp = open_power(owner, 0);
    step(0);
    collect(owner);
    port_close(&vm, pp);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    step(10000);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_power_set(3500, 0, 0);
    CHECK(port_is(open_power(owner, 0)));
    step(10001);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3500, 25, 0, 0);                 /* no stale average */
    teardown();
}

/* A failed read leaves the platform's flag set: no spin, retry at the period. */
TEST(power_pending_stuck_with_failing_read_no_spin) {
    setup();
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    fake_power_fail(true);
    fake_power_new();
    CHECK(port_work_pending(&vm));         /* sample early */
    step(100);
    CHECK_EQ(fake_power_reads(), 2);
    for (uint64_t t = 101; t < 10100; t += 500) {
        CHECK(!port_work_pending(&vm));
        step(t);
    }
    CHECK_EQ(fake_power_reads(), 2);       /* one failed read, then quiet */
    CHECK(!port_work_pending(&vm));
    step(10100);                           /* the period: try again */
    CHECK_EQ(fake_power_reads(), 3);
    fake_power_fail(false);
    step(20100);
    CHECK_EQ(fake_power_reads(), 4);
    CHECK(!port_work_pending(&vm));        /* a good read consumed the flag */
    teardown();
}

/* A failed read with no flag (a chip with no value yet, unix before its first
 * battery line) does not back off: the value that arrives later is sampled
 * at once, not at the next period. */
TEST(power_failed_read_without_flag_samples_on_flag) {
    setup();
    fake_power_fail(true);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    CHECK_EQ(fake_power_reads(), 1);
    CHECK(!port_work_pending(&vm));
    fake_power_fail(false);
    fake_power_set(3900, 1, 0);
    fake_power_new();
    CHECK(port_work_pending(&vm));
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3900, 75, 1, 0);
    teardown();
}

/* can_event follows live(); power_enable brackets the open port. */
TEST(power_can_event_follows_live) {
    setup();
    fake_live_hook(true);
    CHECK(port_is(open_power(owner, 0)));
    CHECK_EQ(fake_power_enabled(), 1);
    Port p;
    memset(&p, 0, sizeof p);
    CHECK(input_power_driver.can_event(&p));
    CHECK(input_kbd_driver.can_event(&p));
    fake_set_live(false);
    CHECK(!input_power_driver.can_event(&p));
    CHECK(!input_kbd_driver.can_event(&p));
    uint64_t d;
    fake_set_live(true);
    CHECK(port_next_deadline(&vm, &d));    /* the next sample keeps a timer-less VM waiting... */
    fake_set_live(false);
    CHECK(!port_next_deadline(&vm, &d));   /* ...unless the hardware is dead */
    teardown();
    CHECK_EQ(fake_power_enabled(), 0);
}

static bool nb_pending(void) { return false; }
static void nb_drain(Vm *v, uint64_t n) { (void)v; (void)n; }
static bool nb_live(const Vm *v) { (void)v; return false; }
static bool nb_next(const Vm *v, uint64_t *d) { (void)v; (void)d; return false; }
static const PortSource filler = { nb_pending, nb_drain, nb_live, nb_next, NULL };

/* No free source slot: input_install logs and the port opens as absent hardware. */
TEST(power_install_without_slot_disables_port) {
    setup();
    input_uninstall();
    int n = 0;
    while (n < 8 && port_add_source(&filler)) n++;   /* the same source twice is only for the test */
    input_install();
    CHECK(!port_is(open_power(owner, 0)));
    for (int i = 0; i < n; i++) port_remove_source(&filler);
    input_install();
    CHECK(port_is(open_power(owner, 0)));
    teardown();
}

/* I1: an event-driven chip is never sampled by time: only a new value (the
 * flag) or a Read samples. */
TEST(power_event_driven_no_periodic_samples) {
    setup();
    fake_event_driven(true);
    CHECK(port_is(open_power(owner, 0)));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 0);
    fake_power_set(3760, 0, 0);
    fake_power_new();
    step(10);
    collect(owner);
    CHECK_EQ(ngot, 0);                     /* (3700*3+3760)/4 = 3715: under 2 points */
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));   /* no sample deadline */
    int reads = fake_power_reads();
    for (uint64_t t = 1000; t <= 60000; t += 1000) {
        CHECK(!port_work_pending(&vm));
        step(t);
    }
    CHECK_EQ(fake_power_reads(), reads);   /* time alone never samples */
    teardown();
}

/* I2: a Read before the first value fails; the value that arrives later is
 * sampled at once and answers the Read. */
TEST(power_event_driven_read_before_first_value) {
    setup();
    fake_event_driven(true);
    fake_power_fail(true);
    term pp = open_power(owner, 0);
    CHECK(port_is(pp));
    step(0);
    send_read(owner, pp);
    step(10);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK(!port_work_pending(&vm));        /* failed with no flag: wait for it */
    fake_power_fail(false);
    fake_power_set(4100, 0, 0);
    fake_power_new();
    CHECK(port_work_pending(&vm));         /* sampled at once, not at a period */
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 4100, 100, 0, 0);
    teardown();
}

/* A Read whose read failed is answered at the next period, without spinning. */
TEST(power_read_retried_after_failed_read) {
    setup();
    term pp = open_power(owner, 0);
    step(0);
    collect(owner);
    fake_power_fail(true);
    send_read(owner, pp);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(fake_power_reads(), 2);
    CHECK(!port_work_pending(&vm));
    step(200);
    CHECK_EQ(fake_power_reads(), 2);
    fake_power_fail(false);
    step(10100);
    collect(owner);
    CHECK_EQ(ngot, 1);                     /* the Read is answered though nothing changed */
    is(0, 3700, 50, 0, 0);
    step(20100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

/* A Read whose event was dropped is answered at the next period. */
TEST(power_read_retried_after_drop) {
    setup();
    term pp = open_power(owner, 0);
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    uint16_t mw = owner->max_words;
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner));
    uint32_t d0 = input_dropped();
    send_read(owner, pp);
    step(100);
    CHECK_EQ(input_dropped() - d0, 1);
    CHECK(!port_work_pending(&vm));
    owner->max_words = mw;
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 0);
    step(10100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 0);
    teardown();
}

/* I1: event-driven mode, an event dropped over the quota must not wedge the
 * port: a later line (flag) and a later Read are delivered. */
TEST(power_event_driven_drop_recovers) {
    setup();
    fake_event_driven(true);
    term pp = open_power(owner, 0);
    CHECK(port_is(pp));
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    uint16_t full = owner->max_words;
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner));
    uint32_t d0 = input_dropped();
    fake_power_set(3700, 1, 0);            /* a state change: reported, but over quota */
    fake_power_new();
    step(10);
    CHECK_EQ(input_dropped() - d0, 1);
    owner->max_words = full;
    fake_power_set(3700, 0, 1);
    fake_power_new();                      /* a later line */
    CHECK(port_work_pending(&vm));
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 0, 1);
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner));
    fake_power_set(3700, 1, 1);
    fake_power_new();
    step(30);                              /* dropped again */
    owner->max_words = full;
    send_read(owner, pp);                  /* a Read, no new line */
    CHECK(port_work_pending(&vm));
    step(40);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 3700, 50, 1, 1);
    teardown();
}
