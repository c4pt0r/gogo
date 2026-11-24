/* Timers: ordering, wakeups, SendAfter, cancellation, and that no timer
 * outlives or touches the actor it targets. */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "signals.h"
#include "timer.h"
#include "term.h"
#include <string.h>

extern void (*platform_unix_err_hook)(const char *, size_t);
static void drop_err(const char *s, size_t n) { (void)s; (void)n; }

/* One function "main" (0 params, 3 regs):
 *   LOADK r0, <ms>; BUILTIN r1, r0, 1, RECEIVE_TIMEOUT (-> r1, r2);
 *   BUILTIN r0, r0, 0, RECEIVE (blocks, so r1/r2 can be inspected); RET r0
 * Layout: 8 header + 3 func + 3 consts + 7 code = 21 words. */
#define IMG_WORDS 21
static void build_rt(uint32_t *w, int32_t ms) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = 0x4F474F47; w[1] = 1; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 7; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (3u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_LOADK, 0, 0, 0); w[15] = term_from_int(ms);
    w[16] = OP_MAKE(OP_BUILTIN, 1, 0, 1); w[17] = B_RECEIVE_TIMEOUT;
    w[18] = OP_MAKE(OP_BUILTIN, 0, 0, 0); w[19] = B_RECEIVE;
    w[20] = OP_MAKE(OP_RET, 0, 0, 0);
}

static void setup_ms(uint32_t *w, Image *img, Vm *vm, int32_t ms) {
    va_reset();
    build_rt(w, ms);
    CHECK_EQ(image_load(img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(vm, img, 8), 0);
}

static void setup(uint32_t *w, Image *img, Vm *vm) { setup_ms(w, img, vm, 50); }

/* An actor about to run main. */
static Actor *start(Vm *vm, const Image *img) {
    Actor *a = actor_new(vm, 32);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 3, -1));
    a->pc = img->code;
    return a;
}

#define REGS_OF(a) ((a)->mem + (a)->fp)

static Actor *waiting_actor(Vm *vm) {
    Actor *a = actor_new(vm, 32);
    CHECK(a != NULL);
    a->state = A_WAITING;
    return a;
}

static int timer_count(const Vm *vm) {
    int n = 0;
    for (const Timer *t = vm->timers; t; t = t->next) n++;
    return n;
}

TEST(timer_order) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm), *c = waiting_actor(&vm);
    CHECK(timer_add_wakeup(&vm, a, 30));
    CHECK(timer_add_wakeup(&vm, b, 10));
    CHECK(timer_add_wakeup(&vm, c, 20));
    CHECK(a->flags & AF_WAKEUP_ARMED);
    uint64_t next = 0;
    CHECK(timer_next(&vm, &next));
    CHECK_EQ(next, 10);

    timer_fire_due(&vm, 15);
    CHECK(runq_pop(&vm) == b);
    CHECK(runq_pop(&vm) == NULL);
    CHECK_EQ(b->state, A_RUNNABLE);
    CHECK((b->flags & AF_TIMED_OUT) && !(b->flags & AF_WAKEUP_ARMED));
    CHECK_EQ(a->state, A_WAITING);

    timer_fire_due(&vm, 100);
    CHECK(runq_pop(&vm) == c);
    CHECK(runq_pop(&vm) == a);
    CHECK(runq_pop(&vm) == NULL);
    CHECK(!timer_next(&vm, &next));
    actor_free(&vm, a); actor_free(&vm, b); actor_free(&vm, c);
    CHECK_EQ(va_used_bytes(), base);
}

/* Deadlines are 64-bit: values past 2^32 ms order and fire correctly. */
TEST(timer_large_deadlines) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    const uint64_t big = (uint64_t)1 << 32;
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm), *c = waiting_actor(&vm);
    CHECK(timer_add_wakeup(&vm, a, big + 10));
    CHECK(timer_add_wakeup(&vm, b, big - 5));
    CHECK(timer_add_wakeup(&vm, c, big + 5));
    uint64_t next = 0;
    CHECK(timer_next(&vm, &next));
    CHECK_EQ(next, big - 5);
    timer_fire_due(&vm, big);
    CHECK(runq_pop(&vm) == b);
    CHECK(runq_pop(&vm) == NULL);
    CHECK(timer_next(&vm, &next));
    CHECK_EQ(next, big + 5);
    timer_fire_due(&vm, big + 5);
    CHECK(runq_pop(&vm) == c);
    CHECK(runq_pop(&vm) == NULL);
    timer_fire_due(&vm, big + 10);
    CHECK(runq_pop(&vm) == a);
    CHECK(!timer_next(&vm, &next));
    actor_free(&vm, a); actor_free(&vm, b); actor_free(&vm, c);
}

/* A message wakes the actor first; the wakeup firing afterwards must not
 * queue it a second time. */
TEST(timer_stale_wakeup_not_double_queued) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *r = waiting_actor(&vm), *s = actor_new(&vm, 32);
    CHECK(timer_add_wakeup(&vm, r, 10));
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(1)), 0);
    CHECK_EQ(r->state, A_RUNNABLE);
    timer_fire_due(&vm, 10);
    CHECK(runq_pop(&vm) == r);
    CHECK(runq_pop(&vm) == NULL);          /* queued exactly once */
    timer_cancel_wakeup(&vm, r);           /* what taking the message does */
    CHECK(!(r->flags & (AF_TIMED_OUT | AF_WAKEUP_ARMED)));
    actor_free(&vm, r); actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

TEST(timer_send_after_delivers_and_cancels) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *s = actor_new(&vm, 64), *r = waiting_actor(&vm);

    term ref = timer_add_send(&vm, s, 5, r->id, term_from_int(7));
    CHECK(term_is_ref(ref));
    const uint32_t *obj = s->mem + term_ref_off(ref);
    CHECK_EQ(obj[0], hdr_make(3, K_REF));
    uint64_t serial = obj[1];
    CHECK(timer_cancel(&vm, serial));
    CHECK(!timer_cancel(&vm, serial));     /* already gone */
    CHECK_EQ(timer_count(&vm), 0);

    /* The message is copied at call time: the sender's heap may change. */
    uint32_t off = heap_alloc(&vm, s, 3, NULL, 0);
    s->mem[off] = hdr_make(3, K_STRUCT); s->mem[off + 1] = 0; s->mem[off + 2] = term_from_int(42);
    ref = timer_add_send(&vm, s, 5, r->id, term_from_ref(off));
    CHECK(term_is_ref(ref));
    s->mem[off + 2] = term_from_int(99);
    timer_fire_due(&vm, 4);
    CHECK(r->mbox_head == NULL);
    timer_fire_due(&vm, 5);
    CHECK(runq_pop(&vm) == r);
    CHECK(r->mbox_head != NULL);
    CHECK_EQ(r->mbox_head->words, 3);
    CHECK_EQ(r->mbox_head->data[2], term_from_int(42));
    CHECK(!timer_cancel(&vm, s->mem[term_ref_off(ref) + 1]));   /* fired */
    actor_free(&vm, s); actor_free(&vm, r);
    CHECK_EQ(va_used_bytes(), base);
}

/* A target that is gone when the timer fires, even one freed without the
 * timer being cancelled (stale pid), just drops the message. */
TEST(timer_send_after_dead_target_drops) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *s = actor_new(&vm, 64), *r = actor_new(&vm, 32);
    term to = r->id;
    actor_free(&vm, r);                    /* no timer_cancel_actor */
    Actor *reuse = actor_new(&vm, 32);     /* same slot, next generation */
    CHECK(term_is_ref(timer_add_send(&vm, s, 1, to, term_from_int(1))));
    timer_fire_due(&vm, 1);
    CHECK(reuse->mbox_head == NULL);
    CHECK_EQ(timer_count(&vm), 0);
    actor_free(&vm, s); actor_free(&vm, reuse);
    CHECK_EQ(va_used_bytes(), base);
}

/* actor_exit frees the timers targeting the actor (its wakeup and
 * SendAfters to it); a SendAfter it made for someone else stays. */
TEST(timer_cancelled_on_exit) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = waiting_actor(&vm), *other = waiting_actor(&vm);
    CHECK(timer_add_wakeup(&vm, a, 10));
    CHECK(term_is_ref(timer_add_send(&vm, other, 20, a->id, term_from_int(1))));
    CHECK(term_is_ref(timer_add_send(&vm, a, 30, other->id, term_from_int(2))));
    CHECK_EQ(timer_count(&vm), 3);
    a->state = A_RUNNABLE;
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(timer_count(&vm), 1);
    timer_fire_due(&vm, 100);
    CHECK(runq_pop(&vm) == other);
    CHECK(other->mbox_head && other->mbox_head->root == term_from_int(2));
    actor_free(&vm, other);
    CHECK_EQ(va_used_bytes(), base);
}

/* An actor killed while blocked in ReceiveTimeout exits and leaves no
 * timer behind. */
TEST(timer_receive_timeout_killed) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *k = start(&vm, &img);
    CHECK_EQ(interp_run(&vm, k), RUN_WAIT);
    k->state = A_WAITING;
    CHECK_EQ(timer_count(&vm), 1);
    actor_mark_exit(&vm, k, R_KILLED, T_EMPTYSTR);
    CHECK(runq_pop(&vm) == k);
    platform_unix_err_hook = drop_err;
    CHECK_EQ(interp_run(&vm, k), RUN_EXIT);
    platform_unix_err_hook = NULL;
    CHECK_EQ(timer_count(&vm), 0);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(), base);
}

/* Timeout fires: the re-run BUILTIN yields (nil, false). */
TEST(timer_receive_timeout_expires) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *t = start(&vm, &img);
    CHECK_EQ(interp_run(&vm, t), RUN_WAIT);
    CHECK(t->pc == img.code + 2);          /* on the RECEIVE_TIMEOUT */
    t->state = A_WAITING;
    uint64_t next;
    CHECK(timer_next(&vm, &next));
    timer_fire_due(&vm, next);
    CHECK(runq_pop(&vm) == t);
    CHECK_EQ(interp_run(&vm, t), RUN_WAIT);
    CHECK(t->pc == img.code + 4);          /* moved on to the RECEIVE */
    CHECK_EQ(REGS_OF(t)[1], T_NIL);
    CHECK_EQ(REGS_OF(t)[2], T_FALSE);
    CHECK(!(t->flags & (AF_TIMED_OUT | AF_WAKEUP_ARMED)));
    CHECK_EQ(timer_count(&vm), 0);
    t->state = A_RUNNABLE;
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(), base);
}

/* A message arriving first: (msg, true), and the wakeup is cancelled. */
TEST(timer_receive_timeout_message_first) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *t = start(&vm, &img), *s = actor_new(&vm, 32);
    CHECK_EQ(interp_run(&vm, t), RUN_WAIT);
    t->state = A_WAITING;
    CHECK_EQ(msg_send(&vm, s, t->id, term_from_int(7)), 0);
    CHECK(runq_pop(&vm) == t);
    CHECK_EQ(interp_run(&vm, t), RUN_WAIT);
    CHECK(t->pc == img.code + 4);
    CHECK_EQ(REGS_OF(t)[1], term_from_int(7));
    CHECK_EQ(REGS_OF(t)[2], T_TRUE);
    CHECK_EQ(timer_count(&vm), 0);
    CHECK(!(t->flags & (AF_TIMED_OUT | AF_WAKEUP_ARMED)));
    t->state = A_RUNNABLE;
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* ReceiveTimeout(0) on an empty mailbox returns at once, arming nothing. */
TEST(timer_receive_timeout_zero) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup_ms(w, &img, &vm, 0);
    Actor *t = start(&vm, &img);
    CHECK_EQ(interp_run(&vm, t), RUN_WAIT);
    CHECK(t->pc == img.code + 4);
    CHECK_EQ(REGS_OF(t)[1], T_NIL);
    CHECK_EQ(REGS_OF(t)[2], T_FALSE);
    CHECK_EQ(timer_count(&vm), 0);
    t->state = A_RUNNABLE;
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
}

/* A loop of SendAfter(1000000, self, 5):
 *   LOADK r1, 1000000; SELF r2; LOADK r3, 5; SEND_AFTER r0, r1..r3; JMP 6
 * Layout: 8 header + 3 func + 3 consts + 10 code = 24 words. */
#define LOOP_WORDS 24
static void build_send_loop(uint32_t *w) {
    memset(w, 0, LOOP_WORDS * sizeof(uint32_t));
    w[0] = 0x4F474F47; w[1] = 1; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 10; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (4u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_LOADK, 1, 0, 0); w[15] = term_from_int(1000000);
    w[16] = OP_MAKE(OP_BUILTIN, 2, 0, 0); w[17] = B_SELF;
    w[18] = OP_MAKE(OP_LOADK, 3, 0, 0); w[19] = term_from_int(5);
    w[20] = OP_MAKE(OP_BUILTIN, 0, 1, 3); w[21] = B_SEND_AFTER;
    w[22] = OP_MAKE(OP_JMP, 0, 0, 0); w[23] = 6;
}

static char oom_err[256]; static size_t oom_len;
static void capture_oom(const char *s, size_t n) {
    if (oom_len + n < sizeof oom_err) { memcpy(oom_err + oom_len, s, n); oom_len += n; oom_err[oom_len] = 0; }
}

/* Pending SendAfter messages count against the creator's limit: a creator
 * piling up timers exits with oom once they would pass it, and nobody
 * else is affected. The charge goes away when a timer fires, is cancelled,
 * or its creator exits. */
TEST(send_after_charged_to_creator) {
    uint32_t w[LOOP_WORDS]; Image img; Vm vm;
    va_reset();
    build_send_loop(w);
    CHECK_EQ(image_load(&img, w, LOOP_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 8), 0);
    size_t base = va_used_bytes();

    Actor *u = waiting_actor(&vm);          /* unrelated */
    Actor *c = actor_new(&vm, 32);
    CHECK(heap_push_frame(&vm, c, 4, -1));
    c->pc = img.code;
    c->max_words = 256;
    oom_len = 0; oom_err[0] = 0;
    platform_unix_err_hook = capture_oom;
    int runs = 0, rc;
    while ((rc = interp_run(&vm, c)) == RUN_YIELD && runs < 100) {
        runs++;
        CHECK((uint32_t)c->mem_words + actor_charged_words(c) <= actor_max_words(c));
    }
    platform_unix_err_hook = NULL;
    CHECK_EQ(rc, RUN_EXIT);
    CHECK(strstr(oom_err, "crashed: oom in main") != NULL);
    CHECK_EQ(timer_count(&vm), 0);          /* its timers (to itself) died with it */
    CHECK_EQ(vm.live, 1);

    /* The unrelated actor still takes messages. */
    Actor *s = actor_new(&vm, 64);
    CHECK_EQ(msg_send(&vm, s, u->id, term_from_int(1)), 0);
    CHECK(u->mbox_head != NULL && !(u->flags & AF_EXIT_PENDING));
    CHECK(runq_pop(&vm) == u);

    /* Release on cancel, on fire, and when the creator exits. */
    term ref = timer_add_send(&vm, s, 10, u->id, term_from_int(2));
    CHECK(actor_charged_words(s) > 0);
    CHECK(timer_cancel(&vm, s->mem[term_ref_off(ref) + 1]));
    CHECK_EQ(actor_charged_words(s), 0);
    CHECK(term_is_ref(timer_add_send(&vm, s, 10, u->id, term_from_int(3))));
    CHECK(actor_charged_words(s) > 0);
    timer_fire_due(&vm, 10);
    CHECK_EQ(actor_charged_words(s), 0);
    CHECK(term_is_ref(timer_add_send(&vm, s, 20, u->id, term_from_int(4))));
    actor_exit(&vm, s, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(timer_count(&vm), 1);          /* still fires for its target */
    timer_fire_due(&vm, 20);                /* must not touch the freed creator */
    CHECK(u->mbox_head && u->mbox_head->next && u->mbox_head->next->next &&
          u->mbox_head->next->next->root == term_from_int(4));
    actor_free(&vm, u);
    CHECK_EQ(va_used_bytes(), base);
}

/* A second timer_add_wakeup while one is armed adds nothing. */
TEST(timer_wakeup_armed_once) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    CHECK(timer_add_wakeup(&vm, a, 10));
    CHECK(timer_add_wakeup(&vm, a, 20));
    CHECK_EQ(timer_count(&vm), 1);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(timer_count(&vm), 0);
}
