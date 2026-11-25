/* The kbd port and the input source against a fake InputHw, on a real VM:
 * open rules, characters and modifiers, the Fn layer, the press Code kept
 * for the release, the 8-key limit, auto-repeat, close/reopen state, a full
 * mailbox, and the US keymap. Time is the fake clock `now`; each step
 * runs port_drain, as the scheduler does. */
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

#define SHIFT MOD_SHIFT
#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm;
static Actor *owner;
static term kp;

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
    kp = T_NIL;
}

static void teardown(void) {
    port_close_all(&vm);
    platform_unix_set_clock(NULL);
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

static term heap_cfg(Actor *a, int d, int r) {
    uint32_t off = heap_alloc(&vm, a, 4, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(4, K_STRUCT);
    a->mem[off + 1] = TYPE_KBD_CONFIG;
    a->mem[off + 2] = term_from_int(d);
    a->mem[off + 3] = term_from_int(r);
    return term_from_ref(off);
}

static term open_kbd(Actor *a, int d, int r) {
    a->hp = 0;
    return port_open(&vm, a, heap_str(a, "kbd"), heap_cfg(a, d, r));
}

static void step(uint64_t t) { now = t; port_drain(&vm, t); }

typedef struct { int32_t code, ch, mods; bool down; } Ev;
static Ev got[64];
static int ngot;

/* Moves a's mailbox into got[]. */
static void collect(Actor *a) {
    ngot = 0;
    for (Msg *m = a->mbox_head; m; m = a->mbox_head) {
        CHECK_EQ(m->data[0], hdr_make(6, K_STRUCT));
        CHECK_EQ(m->data[1], TYPE_KBD_KEY);
        if (ngot < 64) {
            got[ngot].code = term_to_int(m->data[2]);
            got[ngot].ch = term_to_int(m->data[3]);
            got[ngot].mods = term_to_int(m->data[4]);
            got[ngot].down = m->data[5] == T_TRUE;
            CHECK(m->data[5] == T_TRUE || m->data[5] == T_FALSE);
            ngot++;
        }
        a->mbox_head = m->next;
        if (!a->mbox_head) a->mbox_tail = NULL;
        a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
        msg_free(&vm, m);
    }
}

static void is(int i, int code, int ch, int mods, bool down) {
    CHECK(i < ngot);
    if (i >= ngot) return;
    CHECK_EQ(got[i].code, code);
    CHECK_EQ(got[i].ch, ch);
    CHECK_EQ(got[i].mods, mods);
    CHECK_EQ(got[i].down, down);
}

static void press(uint16_t raw) { fake_push(raw, true); }
static void release(uint16_t raw) { fake_push(raw, false); }

TEST(kbd_open_without_keyboard_fails) {
    setup();
    fake_no_keyboard();
    CHECK(!port_is(open_kbd(owner, 0, 0)));
    CHECK_EQ(fake_opens(), 0);
    teardown();
}

TEST(kbd_second_owner_fails) {
    setup();
    Actor *b = actor_new(&vm, 256);
    b->state = A_WAITING;
    kp = open_kbd(owner, 0, 0);
    CHECK(port_is(kp));
    CHECK(!port_is(open_kbd(b, 0, 0)));
    CHECK_EQ(fake_opens(), 1);
    port_close(&vm, kp);
    CHECK_EQ(fake_closes(), 1);
    CHECK(port_is(open_kbd(b, 0, 0)));          /* free again */
    teardown();
}

TEST(kbd_plain_char) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_A); release(R_A);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 2);
    is(0, 0x04, 'a', 0, true);
    is(1, 0x04, 'a', 0, false);
    teardown();
}

TEST(kbd_shift_char) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_SHIFT); press(R_1); release(R_1); release(R_SHIFT);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 4);
    is(0, 0xE1, 0, SHIFT, true);
    is(1, 0x1E, '!', SHIFT, true);
    is(2, 0x1E, '!', SHIFT, false);
    is(3, 0xE1, 0, 0, false);
    teardown();
}

TEST(kbd_ctrl_c) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_CTRL); press(R_C);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 2);
    is(0, 0xE0, 0, MOD_CTRL, true);
    is(1, 0x06, 3, MOD_CTRL, true);
    teardown();
}

TEST(kbd_fn_layer) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_FN); press(R_SEMI); release(R_SEMI); press(R_A); press(R_1);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 5);
    is(0, 0xF0, 0, MOD_FN, true);
    is(1, 0x52, 0, MOD_FN, true);
    is(2, 0x52, 0, MOD_FN, false);
    is(3, 0x04, 'a', MOD_FN, true);       /* no fn_code: normal code, ModFn */
    is(4, 0x3A, 0, MOD_FN, true);         /* Fn+1 = F1: no character */
    teardown();
}

TEST(kbd_release_keeps_press_code) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_FN); press(R_SEMI); release(R_FN); release(R_SEMI);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 4);
    is(2, 0xF0, 0, 0, false);
    is(3, 0x52, 0, 0, false);
    teardown();
}

TEST(kbd_nine_held_ninth_ignored) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    uint16_t ks[9] = { R_A, R_B, R_C, R_D, R_E, R_F, R_G, R_H, R_I };
    for (int i = 0; i < 9; i++) press(ks[i]);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 8);                    /* the ninth press: nothing */
    is(7, 0x0B, 'h', 0, true);
    release(R_A); press(R_I);             /* a slot is free again */
    step(2);
    collect(owner);
    CHECK_EQ(ngot, 2);
    is(1, 0x0C, 'i', 0, true);
    teardown();
}

TEST(kbd_repeat) {
    setup();
    CHECK(port_is(open_kbd(owner, 400, 50)));
    press(R_A);
    int times[8], n = 0;
    for (uint64_t t = 0; t <= 520; t += 10) {
        step(t);
        collect(owner);
        for (int i = 0; i < ngot; i++) {
            CHECK(got[i].down && got[i].code == 0x04 && got[i].ch == 'a');
            if (n < 8) times[n++] = (int)t;
        }
    }
    CHECK_EQ(n, 4);
    if (n == 4) { CHECK_EQ(times[0], 0); CHECK_EQ(times[1], 400); CHECK_EQ(times[2], 450); CHECK_EQ(times[3], 500); }
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 550);
    release(R_A);
    step(530);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x04, 'a', 0, false);
    CHECK(!port_next_deadline(&vm, &d));
    step(1000);
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

TEST(kbd_repeat_last_key_only_and_no_catch_up) {
    setup();
    CHECK(port_is(open_kbd(owner, 100, 0)));        /* period <= 0: 50 */
    press(R_A); press(R_SHIFT); press(R_B);
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 3);
    step(100);                                       /* only B repeats, with Shift */
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x05, 'B', SHIFT, true);
    step(150);
    collect(owner);
    CHECK_EQ(ngot, 1);
    step(5000);                                      /* a long stall: one event, not 100 */
    collect(owner);
    CHECK_EQ(ngot, 1);
    release(R_A);                                    /* an older key: B goes on */
    step(5050);
    collect(owner);
    CHECK_EQ(ngot, 2);
    is(1, 0x05, 'B', SHIFT, true);
    release(R_SHIFT);                                /* a modifier does not stop it */
    step(5100);
    collect(owner);
    CHECK_EQ(ngot, 2);
    teardown();
}

TEST(kbd_repeat_uses_current_mods) {
    setup();
    CHECK(port_is(open_kbd(owner, 100, 50)));
    press(R_SHIFT); press(R_B);
    step(0);
    collect(owner);
    CHECK_EQ(ngot, 2);
    release(R_SHIFT);
    step(10);
    collect(owner);
    CHECK_EQ(ngot, 1);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x05, 'b', 0, true);
    teardown();
}

TEST(kbd_no_repeat_with_zero_delay) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 50)));
    press(R_A);
    step(0);
    step(10000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    teardown();
}

TEST(kbd_close_stops_repeat_and_clears) {
    setup();
    kp = open_kbd(owner, 100, 50);
    CHECK(port_is(kp));
    press(R_SHIFT); press(R_A);
    step(0);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 3);
    port_close(&vm, kp);
    step(300);
    collect(owner);
    CHECK_EQ(ngot, 0);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(port_is(open_kbd(owner, 100, 50)));       /* reopen: queue flushed, state clear */
    step(400);
    collect(owner);
    CHECK_EQ(ngot, 0);
    release(R_A);                                    /* stray release: normal code, no stale Shift */
    step(500);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x04, 'a', 0, false);
    teardown();
}

TEST(kbd_full_mailbox_drops) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    owner->max_words = (uint16_t)(owner->mem_words + actor_charged_words(owner) + 20u);
    uint32_t d0 = input_dropped();
    int sent = 0;
    while (input_dropped() == d0 && sent < 100) {
        press(R_A); release(R_A);
        step(1);
        sent += 2;
    }
    CHECK(input_dropped() > d0 && sent < 100);
    uint32_t d1 = input_dropped();
    for (int i = 0; i < 5; i++) { press(R_A); release(R_A); }
    step(2);
    CHECK_EQ(input_dropped() - d1, 10);
    CHECK(owner->state != A_EXITED && !(owner->flags & AF_EXIT_PENDING));
    collect(owner);
    CHECK(ngot > 0);
    press(R_B);
    step(3);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x05, 'b', 0, true);
    CHECK_EQ(input_dropped() - d1, 10);
    teardown();
}

TEST(kbd_read_error_ends_drain) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    uint32_t e0 = input_read_errors();
    fake_fail_next_read();
    press(R_A);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(input_read_errors() - e0, 1);
    step(2);                                         /* the event is still queued */
    collect(owner);
    CHECK_EQ(ngot, 1);
    teardown();
}

/* kbd_pending is a pure read: asking before the drain must not lose events. */
TEST(kbd_pending_is_pure_read) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_A); press(R_B);
    CHECK(port_work_pending(&vm));
    CHECK(port_work_pending(&vm));
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK(!port_work_pending(&vm));                  /* the empty read cleared the flag */
    teardown();
}

TEST(kbd_error_mid_drain_recovers) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    uint32_t e0 = input_read_errors();
    press(R_A); press(R_B); press(R_C);
    fake_fail_read_after(1);                         /* first read ok, second fails */
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(input_read_errors() - e0, 1);
    CHECK(port_work_pending(&vm));                   /* flag still set: retry */
    step(2);
    collect(owner);
    CHECK_EQ(ngot, 2);
    is(0, 0x05, 'b', 0, true);
    is(1, 0x06, 'c', 0, true);
    teardown();
}

/* M3: a persistent read failure backs off instead of spinning. */
TEST(kbd_persistent_error_backs_off) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    press(R_A);
    fake_fail_all_reads(true);
    step(1);                                         /* 1st failure: retry at once */
    CHECK(port_work_pending(&vm));
    step(2);                                         /* 2nd: back off 10 ms */
    CHECK_EQ(fake_kbd_reads(), 2);
    CHECK(!port_work_pending(&vm));
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 12);
    step(11);
    CHECK_EQ(fake_kbd_reads(), 2);
    step(12);                                        /* 3rd: back off 20 ms */
    CHECK_EQ(fake_kbd_reads(), 3);
    step(31);
    CHECK_EQ(fake_kbd_reads(), 3);
    for (uint64_t t = 32; t < 2000; t++) step(t);    /* capped at 640 ms: bounded reads */
    CHECK(fake_kbd_reads() < 14);
    fake_fail_all_reads(false);
    step(3000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x04, 'a', 0, true);
    teardown();
}

/* Runs the scheduler's idle logic from t0 to t1 (as interp.c: drain while
 * port_due_now, else sleep to the next deadline) and returns the passes
 * (drains); stops at max passes. */
static int sched_passes(uint64_t t0, uint64_t t1, int max) {
    int passes = 0;
    now = t0;
    while (now < t1 && passes < max) {
        if (port_due_now(&vm)) {
            passes++;
            step(now);
            continue;
        }
        uint64_t d;
        if (!port_next_deadline(&vm, &d) || d >= t1) break;
        now = d > now ? d : now + 1;
    }
    return passes;
}

/* I3: an INT line stuck low (kbd_pending true, the FIFO empty) is no
 * progress: after one immediate retry it backs off like a read error, so
 * the scheduler sleeps instead of spinning. A real event racing the clear
 * (pending once, then an event) costs no backoff; after the line recovers
 * keys come through. */
TEST(kbd_stuck_int_backs_off) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    step(0);                                         /* the open's look: empty */
    fake_kbd_stuck(true);
    int passes = sched_passes(1, 2001, 1000);
    CHECK(passes < 16);                              /* 10, 20, ... 640 ms: about 9 */
    CHECK(fake_kbd_reads() < 18);
    fake_kbd_stuck(false);
    press(R_A);
    passes = sched_passes(2001, 3000, 1000);
    CHECK(passes >= 1 && passes < 4);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x04, 'a', 0, true);
    /* The race: pending after an empty read once, then the event comes. */
    fake_kbd_stuck(true);
    step(3000);                                      /* empty, still pending: one strike */
    fake_kbd_stuck(false);
    press(R_B);
    CHECK(port_due_now(&vm));                        /* no backoff after one */
    step(3000);
    collect(owner);
    CHECK_EQ(ngot, 1);
    is(0, 0x05, 'b', 0, true);
    teardown();
}

/* M2: the 32-bit deadlines survive the wrap of the low 32 bits of the clock. */
TEST(kbd_repeat_across_32bit_wrap) {
    setup();
    uint64_t t0 = 0xFFFFFF00ull;
    now = t0;
    CHECK(port_is(open_kbd(owner, 400, 50)));
    press(R_A);
    step(t0);
    collect(owner);
    CHECK_EQ(ngot, 1);
    step(t0 + 399);
    collect(owner);
    CHECK_EQ(ngot, 0);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, t0 + 400);
    step(t0 + 400);                                  /* 0x1_0000_0090 */
    collect(owner);
    CHECK_EQ(ngot, 1);
    step(t0 + 450);
    collect(owner);
    CHECK_EQ(ngot, 1);
    teardown();
}

static bool k_nb_pending(const Vm *v, void *c) { (void)v; (void)c; return false; }
static void k_nb_drain(Vm *v, void *c, uint64_t n) { (void)v; (void)c; (void)n; }
static bool k_nb_live(const Vm *v, void *c) { (void)v; (void)c; return false; }
static bool k_nb_next(const Vm *v, void *c, uint64_t *d) { (void)v; (void)c; (void)d; return false; }
static const PortSource k_filler = { k_nb_pending, k_nb_drain, k_nb_live, k_nb_next, NULL };

/* No free source slot: input_install logs and the port opens as absent hardware. */
TEST(kbd_install_without_slot_disables_port) {
    setup();
    input_uninstall(&vm);
    int n = 0;
    while (n < 8 && port_add_source(&vm, &k_filler, NULL)) n++;
    input_install(&vm);
    CHECK(!port_is(open_kbd(owner, 0, 0)));
    for (int i = 0; i < n; i++) port_remove_source(&vm, &k_filler);
    input_install(&vm);
    CHECK(port_is(open_kbd(owner, 0, 0)));
    teardown();
}

TEST(kbd_owner_exit_closes) {
    setup();
    CHECK(port_is(open_kbd(owner, 0, 0)));
    port_close_owned(&vm, owner->id);
    CHECK_EQ(fake_closes(), 1);
    press(R_A);
    step(1);
    collect(owner);
    CHECK_EQ(ngot, 0);
    teardown();
}

TEST(keymap_us_table) {
    static const char n[] = "abcdefghijklmnopqrstuvwxyz1234567890", s[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    for (int i = 0; i < 36; i++) {
        CHECK_EQ(input_key_char((uint8_t)(0x04 + i), 0), n[i]);
        CHECK_EQ(input_key_char((uint8_t)(0x04 + i), MOD_SHIFT), s[i]);
    }
    static const char sn[] = "-=[]\\#;'`,./", ss[] = "_+{}|~:\"~<>?";
    for (int i = 0; i < 12; i++) {
        CHECK_EQ(input_key_char((uint8_t)(0x2D + i), 0), sn[i]);
        CHECK_EQ(input_key_char((uint8_t)(0x2D + i), MOD_SHIFT), ss[i]);
    }
    CHECK_EQ(input_key_char(0x28, 0), 10);
    CHECK_EQ(input_key_char(0x29, 0), 27);
    CHECK_EQ(input_key_char(0x2A, 0), 8);
    CHECK_EQ(input_key_char(0x2B, 0), 9);
    CHECK_EQ(input_key_char(0x2C, 0), 32);
    CHECK_EQ(input_key_char(0x2C, MOD_SHIFT), 32);
    CHECK_EQ(input_key_char(0x4C, 0), 127);
    for (int c = 0x3A; c <= 0x45; c++) CHECK_EQ(input_key_char((uint8_t)c, 0), 0);     /* F-keys */
    for (int c = 0x4F; c <= 0x52; c++) CHECK_EQ(input_key_char((uint8_t)c, 0), 0);     /* arrows */
    for (int c = 0xE0; c <= 0xE3; c++) CHECK_EQ(input_key_char((uint8_t)c, 0), 0);
    CHECK_EQ(input_key_char(0xF0, 0), 0);
    CHECK_EQ(input_key_char(0x04, MOD_CTRL), 1);
    CHECK_EQ(input_key_char(0x1D, MOD_CTRL | MOD_SHIFT), 26);
    CHECK_EQ(input_key_char(0x04, MOD_ALT | MOD_OPT | MOD_FN), 'a');
}
