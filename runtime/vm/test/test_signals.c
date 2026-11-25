/* The single exit path, deferred exits, the mailbox quota and the
 * emergency reserve. */
#include "test.h"
#include "test_host.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "signals.h"
#include "config.h"
#include "term.h"
#include <string.h>
#include <signal.h>   /* the C header must not be shadowed by vm/ */

extern void (*platform_unix_err_hook)(const char *, size_t);
static char err_buf[512]; static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}
static void capture_start(void) { err_len = 0; err_buf[0] = 0; platform_unix_err_hook = capture_err; }
static void capture_stop(void) { platform_unix_err_hook = NULL; }

/* consts[0] = "main" (3 words), consts[3] = "worker" (4 words). */
#define NAMES_WORDS 7
static void put_names(uint32_t *c) {
    c[0] = hdr_make(3, K_STRING); c[1] = 4; memcpy(&c[2], "main", 4);
    c[3] = hdr_make(4, K_STRING); c[4] = 6; memset(&c[5], 0, 8); memcpy(&c[5], "worker", 6);
}

/* One function "main" (0 params, 3 regs) with the given code. */
#define MAX_CODE 8
#define IMG_MAX (8 + 3 + NAMES_WORDS + MAX_CODE + 1 + 2)
static uint32_t build(uint32_t *w, const uint32_t *code, uint32_t n_code) {
    memset(w, 0, IMG_MAX * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = NAMES_WORDS; w[5] = n_code; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (3u << 16); w[10] = 0;
    put_names(&w[11]);
    memcpy(&w[11 + NAMES_WORDS], code, n_code * sizeof(uint32_t));
    return img_seal(w);
}

static Actor *start(Vm *vm, const Image *img, uint16_t words) {
    Actor *a = actor_new(vm, words);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 3, -1));
    a->pc = img->code;
    return a;
}

/* RECEIVE into r1, then RET r1. */
static const uint32_t receiver_code[] = {
    OP_MAKE(OP_BUILTIN, 1, 0, 0), B_RECEIVE,
    OP_MAKE(OP_RET, 1, 0, 0),
};

TEST(exit_report_kinds) {
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, receiver_code, 3)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    static const struct { uint32_t code; const char *want; } cases[] = {
        { R_NORMAL, "" }, { R_SHUTDOWN, "" },
        { R_KILLED, "gogo: actor <pid 0> crashed: killed\n" },
        { R_OUT_OF_MEMORY, "gogo: actor <pid 0> crashed: oom\n" },
        { R_MAILBOX_OVERFLOW, "gogo: actor <pid 0> crashed: mailbox_overflow\n" },
        { R_NO_PROC, "gogo: actor <pid 0> crashed: noproc\n" },
        { R_DIV_ZERO, "gogo: actor <pid 0> crashed: divzero\n" },
        { R_BAD_CODE, "gogo: actor <pid 0> crashed: badcode\n" },
        { R_BAD_SHIFT, "gogo: actor <pid 0> crashed: badshift\n" },
        { 150, "gogo: actor <pid 0> crashed: exit(150)\n" },
        { (uint32_t)-3, "gogo: actor <pid 0> crashed: exit(-3)\n" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Actor *a = actor_new(&vm, 24);
        capture_start();
        actor_exit(&vm, a, cases[i].code, T_EMPTYSTR);
        capture_stop();
        CHECK(strcmp(err_buf, cases[i].want) == 0);
        CHECK_EQ(vm.live, 0);
    }
    /* A function name is appended. */
    Actor *a = actor_new(&vm, 24);
    capture_start();
    actor_exit(&vm, a, R_DIV_ZERO, term_from_const(3));
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: divzero in worker\n") == 0);
}

/* A malformed name (header claiming fewer than 2 words) is not printed. */
TEST(exit_report_bad_name_ignored) {
    static uint32_t consts[4] = { 0, 0xFFFFFFFFu, 0, 0 };
    Image img; Vm vm;
    va_reset(&vm);
    memset(&img, 0, sizeof img);
    consts[0] = hdr_make(1, K_STRING);
    img.consts = consts;
    img.consts_words = 4;
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = actor_new(&vm, 24);
    capture_start();
    actor_exit(&vm, a, R_DIV_ZERO, term_from_const(0));
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: divzero\n") == 0);
    a = actor_new(&vm, 24);
    capture_start();
    actor_exit(&vm, a, R_DIV_ZERO, term_from_const(9));      /* out of range */
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: divzero\n") == 0);
    CHECK(SIGINT != 0);
}

/* A marked actor exits with the pending code when scheduled, before it
 * runs a single instruction, and receives no further messages. */
TEST(mark_exit_runnable_actor_exits_before_running) {
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, receiver_code, 3)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *r = start(&vm, &img, 32);
    Actor *s = actor_new(&vm, 24);
    CHECK_EQ(interp_run(&vm, r), RUN_WAIT);
    r->state = A_WAITING;
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(1)), 0);   /* wakes r */
    CHECK(vm.runq_head == r);
    actor_mark_exit(&vm, r, R_KILLED, T_EMPTYSTR);
    CHECK(vm.runq_head == r && vm.runq_tail == r);             /* not pushed twice */
    uint16_t mw = r->mbox_words;
    CHECK(msg_send(&vm, s, r->id, term_from_int(2)) >= 0);     /* dropped */
    CHECK_EQ(r->mbox_words, mw);
    actor_mark_exit(&vm, r, R_NO_PROC, T_EMPTYSTR);                        /* first code wins */
    CHECK(runq_pop(&vm) == r);
    capture_start();
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);  /* would have received and returned normally */
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: killed\n") == 0);
    CHECK_EQ(vm.live, 1);
    CHECK(actor_lookup(&vm, term_make_pid(0, 0)) == NULL);
    actor_free(&vm, s);
}

TEST(mark_exit_waiting_actor_is_woken) {
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, receiver_code, 3)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *r = start(&vm, &img, 32);
    CHECK_EQ(interp_run(&vm, r), RUN_WAIT);
    r->state = A_WAITING;
    actor_mark_exit(&vm, r, R_MAILBOX_OVERFLOW, T_EMPTYSTR);
    CHECK_EQ(r->state, A_RUNNABLE);
    CHECK(runq_pop(&vm) == r);
    CHECK(runq_pop(&vm) == NULL);
    capture_start();
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);
    capture_stop();
    CHECK(strstr(err_buf, "crashed: mailbox_overflow\n") != NULL);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);                           /* extra freed too */
}

/* Delivery that would take the receiver past its limit drops the message,
 * marks the receiver and returns 0; the sender is unaffected. */
TEST(quota_overflow_marks_receiver) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *s = actor_new(&vm, 24), *r = actor_new(&vm, 24);
    r->max_words = 40;
    r->state = A_WAITING;
    int sent = 0;
    while (!(r->flags & AF_EXIT_PENDING) && sent < 100) {
        CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(sent)), 0);
        CHECK((uint32_t)r->mem_words + r->mbox_words <= 40u);
        sent++;
    }
    CHECK(r->flags & AF_EXIT_PENDING);
    CHECK(sent > 1 && sent < 100);
    uint16_t mw = r->mbox_words;
    CHECK(msg_send(&vm, s, r->id, term_from_int(0)) >= 0);
    CHECK_EQ(r->mbox_words, mw);
    CHECK_EQ(s->state, A_RUNNABLE);
    CHECK(!(s->flags & AF_EXIT_PENDING));
    CHECK(runq_pop(&vm) == r);                 /* woken by the mark */
    CHECK(runq_pop(&vm) == NULL);
    capture_start();
    actor_free(&vm, r);
    actor_free(&vm, s);
    capture_stop();
    CHECK_EQ(vm.live, 0);
}

TEST(quota_mailbox_counter_limit) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 32), *b = actor_new(&vm, 24);
    b->mbox_words = 65535;
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(1)), 0);
    CHECK(b->mbox_head == NULL);
    CHECK(b->flags & AF_EXIT_PENDING);
    b->mbox_words = 0;
    actor_free(&vm, b);
    actor_free(&vm, a);
}

/* The quota looks only at the receiver: a sender over its own limit with a
 * full mailbox still delivers. */
TEST(quota_ignores_sender_state) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *s = actor_new(&vm, 64), *r = actor_new(&vm, 24);
    s->max_words = 10;
    s->mbox_words = 65000;
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(1)), 0);
    CHECK(r->mbox_head != NULL);
    CHECK(!(r->flags & AF_EXIT_PENDING));
    CHECK(!(s->flags & AF_EXIT_PENDING));
    s->mbox_words = 0;
    actor_free(&vm, r);
    actor_free(&vm, s);
}

/* An actor overflowing its own mailbox (self-send) exits right after the
 * SEND: SELF r0; loop: SEND(r0, r1); JMP loop. */
TEST(self_send_overflow_exits_at_once) {
    static const uint32_t code[] = {
        OP_MAKE(OP_BUILTIN, 0, 0, 0), B_SELF,
        OP_MAKE(OP_BUILTIN, 2, 0, 2), B_SEND,
        OP_MAKE(OP_JMP, 0, 0, 0), 2,
    };
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, code, 6)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *a = start(&vm, &img, 32);
    a->max_words = 48;
    capture_start();
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: mailbox_overflow\n") == 0);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* EXIT(code) ends the actor with that code and no function name. */
TEST(builtin_exit) {
    static const uint32_t code[] = {
        OP_MAKE(OP_LOADK, 0, 0, 0), 0,      /* patched */
        OP_MAKE(OP_BUILTIN, 1, 0, 1), B_EXIT,
        OP_MAKE(OP_RET, 1, 0, 0),
    };
    uint32_t c[5]; memcpy(c, code, sizeof c);
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    c[1] = term_from_int(150);
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, c, 5)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = start(&vm, &img, 32);
    capture_start();
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: exit(150)\n") == 0);
    CHECK_EQ(vm.live, 0);
    /* Exit(Normal) is silent; a non-int code is badcode in main. */
    c[1] = term_from_int(0);
    CHECK_EQ(image_load(&img, w, build(w, c, 5)), IMG_OK);
    a = start(&vm, &img, 32);
    capture_start();
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    capture_stop();
    CHECK_EQ(err_len, 0);
    c[1] = T_TRUE;
    CHECK_EQ(image_load(&img, w, build(w, c, 5)), IMG_OK);
    a = start(&vm, &img, 32);
    capture_start();
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: badcode in main\n") == 0);
    CHECK_EQ(vm.live, 0);
}

/* With the machine out of memory, the exit path still gets its allocation
 * from the reserve, which is re-acquired once the exit has freed memory. */
TEST(reserve_covers_exit_path_allocation) {
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, receiver_code, 3)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    CHECK(vm_reserve_init(&vm));
    size_t base = va_used_bytes(&vm);             /* includes the reserve */
    Actor *r = start(&vm, &img, 32);
    CHECK_EQ(interp_run(&vm, r), RUN_WAIT);
    r->state = A_WAITING;
    size_t held = va_used_bytes(&vm);
    va_set_limit(&vm, held);                        /* nothing more fits */
    CHECK(va_alloc(&vm, 8) == NULL);
    void *p = signal_alloc(&vm, 8);                 /* falls back on the reserve */
    CHECK(p != NULL);
    CHECK(va_used_bytes(&vm) < held);             /* reserve released */
    va_free(&vm, p, 8);
    actor_mark_exit(&vm, r, R_KILLED, T_EMPTYSTR);         /* allocates the pending code */
    CHECK(r->flags & AF_EXIT_PENDING);
    CHECK(runq_pop(&vm) == r);
    capture_start();
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: killed\n") == 0);
    /* The actor's memory is back, so the reserve was taken again. */
    CHECK_EQ(va_used_bytes(&vm), base);
    va_set_limit(&vm, 0);
    size_t before = va_used_bytes(&vm);
    vm_reserve_release(&vm);
    CHECK_EQ(va_used_bytes(&vm), before - GOGO_RESERVE_BYTES);
}

/* ---- links, trap exit, kill ---- */

static uint32_t count_lines(const char *s, const char *needle) {
    uint32_t n = 0;
    for (const char *p = s; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static uint32_t mbox_len(const Actor *a) {
    uint32_t n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* Field i of the struct at heap ref t in a. */
static term field(const Actor *a, term t, uint32_t i) {
    return a->mem[term_ref_off(t) + 2u + i];
}

/* Receives the oldest message of a, which must be an ExitMsg; returns its
 * From and stores Reason.Code / Reason.Func. */
static term recv_exit(Vm *vm, Actor *a, int32_t *code, term *func) {
    term t;
    CHECK(msg_receive(vm, a, &t));
    CHECK(term_is_ref(t));
    CHECK_EQ(a->mem[term_ref_off(t) + 1u], TYPE_EXITMSG);
    term r = field(a, t, 1);
    CHECK_EQ(a->mem[term_ref_off(r) + 1u], TYPE_REASON);
    *code = term_to_int(field(a, r, 0));
    *func = field(a, r, 1);
    return field(a, t, 0);
}

/* With memory limited, sig_link either succeeds completely or fails with
 * nothing allocated that survives the two actors. */
TEST(link_prealloc_oom) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    uint32_t failed = 0, linked = 0;
    for (size_t extra = 0; extra <= 384; extra += 8) {   /* past the full cost */
        Actor *a = actor_new(&vm, 24), *b = actor_new(&vm, 24);
        va_set_limit(&vm, va_used_bytes(&vm) + extra);
        bool ok = sig_link(&vm, a, b->id);
        va_set_limit(&vm, 0);
        CHECK_EQ(sig_is_linked(a, b->id), ok);
        CHECK_EQ(sig_is_linked(b, a->id), ok);
        if (ok) linked++; else failed++;
        if (extra == 0) CHECK(!ok);
        actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
        CHECK(!sig_is_linked(a, b->id));
        actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
        CHECK_EQ(va_used_bytes(&vm), base);
    }
    CHECK(failed > 0 && linked > 0);
}

/* Duplicate links are one link; unlink frees both preallocated blocks;
 * unlinking an unlinked or dead pid does nothing. */
TEST(link_unlink_idempotent) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 24), *b = actor_new(&vm, 24);
    CHECK(sig_link(&vm, a, a->id));            /* self: no effect */
    CHECK(!sig_is_linked(a, a->id));
    CHECK(sig_link(&vm, a, b->id));
    size_t once = va_used_bytes(&vm);
    CHECK(sig_link(&vm, a, b->id));
    CHECK(sig_link(&vm, b, a->id));
    CHECK_EQ(va_used_bytes(&vm), once);
    sig_unlink(&vm, b, a->id);
    CHECK(!sig_is_linked(a, b->id) && !sig_is_linked(b, a->id));
    CHECK(va_used_bytes(&vm) < once);
    size_t after = va_used_bytes(&vm);
    sig_unlink(&vm, a, b->id);
    sig_unlink(&vm, a, term_make_pid(3, 0));
    CHECK_EQ(va_used_bytes(&vm), after);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
}

/* A-B-C-A ring plus a trapping m linked to A. C crashes: A and B are
 * marked (cascade, no recursion), each actor exits exactly once with C's
 * reason and function, and m gets a single ExitMsg from A. */
TEST(ring_each_exits_once) {
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, receiver_code, 3)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    size_t base = va_used_bytes(&vm);
    Actor *m = actor_new(&vm, 32), *a = start(&vm, &img, 32), *b = start(&vm, &img, 32),
          *c = start(&vm, &img, 32);
    m->state = A_WAITING;
    a->state = b->state = c->state = A_WAITING;
    sig_set_trap(m, true);
    CHECK(sig_link(&vm, a, b->id));
    CHECK(sig_link(&vm, b, c->id));
    CHECK(sig_link(&vm, c, a->id));
    CHECK(sig_link(&vm, m, a->id));
    term apid = a->id;
    capture_start();
    actor_exit(&vm, c, R_DIV_ZERO, term_from_const(3));
    Actor *x;
    uint32_t ran = 0;
    while ((x = runq_pop(&vm)) != NULL) {
        if (x == m) continue;                  /* keep m's mailbox to inspect */
        CHECK_EQ(interp_run(&vm, x), RUN_EXIT);
        ran++;
    }
    capture_stop();
    CHECK_EQ(ran, 2);
    CHECK_EQ(count_lines(err_buf, "crashed: divzero in worker\n"), 3);
    CHECK_EQ(count_lines(err_buf, "crashed:"), 3);
    CHECK_EQ(vm.live, 1);
    CHECK_EQ(mbox_len(m), 1);
    int32_t code; term func;
    CHECK_EQ(recv_exit(&vm, m, &code, &func), apid);
    CHECK_EQ(code, R_DIV_ZERO);
    CHECK_EQ(func, term_from_const(3));
    CHECK_EQ(m->mbox_words, 0);
    actor_exit(&vm, m, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* Normal exit: a trapping peer gets ExitMsg(Normal), a non-trapping one is
 * unaffected. The notification bypasses the receiver's quota but is
 * counted in mbox_words. */
TEST(link_normal_exit_and_quota_bypass) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *d = actor_new(&vm, 24), *t = actor_new(&vm, 24), *n = actor_new(&vm, 24);
    sig_set_trap(t, true);
    CHECK(sig_link(&vm, d, t->id));
    CHECK(sig_link(&vm, d, n->id));
    t->max_words = 24;                          /* no room for any message */
    term did = d->id;
    capture_start();
    actor_exit(&vm, d, R_NORMAL, T_EMPTYSTR);
    capture_stop();
    CHECK_EQ(err_len, 0);
    CHECK(!(n->flags & AF_EXIT_PENDING) && n->mbox_head == NULL);
    CHECK(!(t->flags & AF_EXIT_PENDING));
    CHECK_EQ(mbox_len(t), 1);
    CHECK(t->mbox_words > 0);
    t->max_words = 0;
    int32_t code; term func;
    CHECK_EQ(recv_exit(&vm, t, &code, &func), did);
    CHECK_EQ(code, R_NORMAL);
    CHECK_EQ(func, T_EMPTYSTR);
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, n, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
}

/* Kill: Normal ignored by a non-trapping target; other codes mark it;
 * a trapping target gets an ExitMsg unless the code is Killed. */
TEST(kill_rules) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *k = actor_new(&vm, 24), *t = actor_new(&vm, 24);
    t->state = A_WAITING;
    CHECK(sig_kill(&vm, k, t->id, R_NORMAL));
    CHECK(!(t->flags & AF_EXIT_PENDING));
    sig_set_trap(t, true);
    CHECK(sig_kill(&vm, k, t->id, R_NORMAL));
    CHECK(sig_kill(&vm, k, t->id, 150));
    CHECK_EQ(mbox_len(t), 2);
    CHECK(runq_pop(&vm) == t);                 /* woken once */
    CHECK(runq_pop(&vm) == NULL);
    int32_t code; term func;
    CHECK_EQ(recv_exit(&vm, t, &code, &func), k->id);
    CHECK_EQ(code, R_NORMAL);
    CHECK_EQ(recv_exit(&vm, t, &code, &func), k->id);
    CHECK_EQ(code, 150);
    CHECK(sig_kill(&vm, k, t->id, R_KILLED));  /* untrappable */
    uint32_t pc; term pf;
    CHECK(actor_exit_pending(t, &pc, &pf));
    CHECK_EQ(pc, R_KILLED);
    CHECK(sig_kill(&vm, k, term_make_pid(3, 0), R_KILLED));  /* dead pid: nothing */
    capture_start();
    actor_exit(&vm, t, pc, pf);
    actor_exit(&vm, k, R_NORMAL, T_EMPTYSTR);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 1> crashed: killed\n") == 0);
    CHECK_EQ(va_used_bytes(&vm), base);

    /* Killing oneself: Killed cannot be trapped; a non-trapper exits with
     * the code given. */
    Actor *s = actor_new(&vm, 24);
    sig_set_trap(s, true);
    CHECK(sig_kill(&vm, s, s->id, R_KILLED));
    CHECK(actor_exit_pending(s, &pc, &pf));
    CHECK_EQ(pc, R_KILLED);
    CHECK_EQ(mbox_len(s), 0);
    capture_start();
    actor_exit(&vm, s, pc, pf);
    s = actor_new(&vm, 24);
    CHECK(sig_kill(&vm, s, s->id, 150));
    CHECK(actor_exit_pending(s, &pc, &pf));
    CHECK_EQ(pc, 150);
    actor_exit(&vm, s, pc, pf);
    capture_stop();
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* Linking a dead pid: a trapping caller gets ExitMsg{pid, NoProc}, a
 * non-trapping one is marked to exit with NoProc. */
TEST(link_dead_pid_is_noproc) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 24), *d = actor_new(&vm, 24);
    term dead = d->id;
    actor_exit(&vm, d, R_NORMAL, T_EMPTYSTR);
    sig_set_trap(a, true);
    CHECK(sig_link(&vm, a, dead));
    CHECK(!sig_is_linked(a, dead));
    int32_t code; term func;
    CHECK_EQ(recv_exit(&vm, a, &code, &func), dead);
    CHECK_EQ(code, R_NO_PROC);
    sig_set_trap(a, false);
    CHECK(sig_link(&vm, a, dead));
    uint32_t pc; term pf;
    CHECK(actor_exit_pending(a, &pc, &pf));
    CHECK_EQ(pc, R_NO_PROC);
    capture_start();
    actor_exit(&vm, a, pc, pf);
    capture_stop();
    CHECK(strcmp(err_buf, "gogo: actor <pid 0> crashed: noproc\n") == 0);
    CHECK_EQ(vm.live, 0);
}

/* ---- monitors ---- */

/* Receives the oldest message of a, which must be a DownMsg; returns its
 * From and stores the Ref serial and Reason.Code / Reason.Func. */
static term recv_down(Vm *vm, Actor *a, uint64_t *serial, int32_t *code, term *func) {
    term t;
    CHECK(msg_receive(vm, a, &t));
    CHECK(term_is_ref(t));
    CHECK_EQ(a->mem[term_ref_off(t) + 1u], TYPE_DOWNMSG);
    const uint32_t *ro = a->mem + term_ref_off(field(a, t, 0));
    CHECK_EQ(hdr_kind(ro[0]), K_REF);
    *serial = (uint64_t)ro[1] | ((uint64_t)ro[2] << 32);
    term r = field(a, t, 2);
    CHECK_EQ(a->mem[term_ref_off(r) + 1u], TYPE_REASON);
    *code = term_to_int(field(a, r, 0));
    *func = field(a, r, 1);
    return field(a, t, 1);
}

static uint64_t ref_serial_in(const Actor *a, term ref) {
    const uint32_t *o = a->mem + term_ref_off(ref);
    return (uint64_t)o[1] | ((uint64_t)o[2] << 32);
}

/* A down message arrives whatever the exit reason (Normal too) and bypasses
 * the watcher's quota but counts in mbox_words; two monitors of one actor
 * give two messages with their own refs. */
TEST(monitor_delivers_on_any_exit) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    for (int round = 0; round < 2; round++) {
        Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
        term tid = t->id;
        term r1 = sig_monitor(&vm, w, tid), r2 = sig_monitor(&vm, w, tid);
        CHECK(r1 != T_NIL && r2 != T_NIL);
        uint64_t s1 = ref_serial_in(w, r1), s2 = ref_serial_in(w, r2);
        CHECK(s1 != s2);
        w->max_words = w->mem_words;            /* no room for any message */
        uint32_t code = round ? 150u : R_NORMAL;
        capture_start();
        actor_exit(&vm, t, code, round ? term_from_const(3) : T_EMPTYSTR);
        capture_stop();
        CHECK_EQ(mbox_len(w), 2);
        CHECK(w->mbox_words > 0);
        CHECK(!(w->flags & AF_EXIT_PENDING));
        w->max_words = 0;
        uint64_t sn; int32_t c; term f;
        CHECK_EQ(recv_down(&vm, w, &sn, &c, &f), tid);
        CHECK(sn == s1 || sn == s2);                /* newest monitor first */
        uint64_t first = sn;
        CHECK_EQ(c, (int32_t)code);
        CHECK_EQ(f, round ? term_from_const(3) : T_EMPTYSTR);
        CHECK_EQ(recv_down(&vm, w, &sn, &c, &f), tid);
        CHECK(sn != first && (sn == s1 || sn == s2));
        actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    }
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* A code that does not fit a small int is boxed in the preallocated block. */
TEST(monitor_big_code) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
    term tid = t->id;
    CHECK(sig_monitor(&vm, w, tid) != T_NIL);
    capture_start();
    actor_exit(&vm, t, 0x7fffff00u, T_EMPTYSTR);
    capture_stop();
    term m;
    CHECK(msg_receive(&vm, w, &m));
    term reason = field(w, m, 2);
    term c = field(w, reason, 0);
    CHECK(term_is_ref(c));
    CHECK_EQ(w->mem[term_ref_off(c) + 1u], 0x7fffff00u);
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
}

/* Demonitor frees the preallocated block and both entries; unknown or
 * repeated Demonitor does nothing; the monitored actor's exit then sends
 * nothing. */
TEST(demonitor_frees_and_is_idempotent) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
    size_t base = va_used_bytes(&vm);
    term r = sig_monitor(&vm, w, t->id);
    CHECK(r != T_NIL);
    size_t held = va_used_bytes(&vm);
    CHECK(held > base);
    sig_demonitor(&vm, w, ref_serial_in(w, r));
    size_t after = va_used_bytes(&vm);
    CHECK(after < held);
    sig_demonitor(&vm, w, ref_serial_in(w, r));
    sig_demonitor(&vm, w, 12345);                 /* unknown serial */
    CHECK_EQ(va_used_bytes(&vm), after);
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(mbox_len(w), 0);
    sig_demonitor(&vm, w, ref_serial_in(w, r));                     /* the target is gone */
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
}

/* A fired monitor is gone: Demonitor of its ref frees nothing twice, and a
 * message already queued stays queued. */
TEST(demonitor_after_fire) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
    term r = sig_monitor(&vm, w, t->id);
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    sig_demonitor(&vm, w, ref_serial_in(w, r));
    CHECK_EQ(mbox_len(w), 1);
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* The watcher exits first: the monitored actor's entries and blocks go
 * with it, and the target's later exit sends nothing (no use after free). */
TEST(monitor_watcher_exits_first) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
    CHECK(sig_monitor(&vm, w, t->id) != T_NIL);
    CHECK(sig_monitor(&vm, w, t->id) != T_NIL);
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* A watcher already marked for exit gets no message (it would be dropped
 * anyway); its block is freed. Watcher and target each watching the other,
 * both dying in the same pass, free everything once. */
TEST(monitor_marked_watcher_and_mutual) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
    CHECK(sig_monitor(&vm, w, t->id) != T_NIL);
    CHECK(sig_monitor(&vm, t, w->id) != T_NIL);
    w->state = t->state = A_WAITING;            /* marking queues each */
    actor_mark_exit(&vm, w, R_KILLED, T_EMPTYSTR);
    actor_mark_exit(&vm, t, R_KILLED, T_EMPTYSTR);
    CHECK(runq_pop(&vm) == w);                  /* as the scheduler would */
    CHECK(runq_pop(&vm) == t);
    CHECK(runq_pop(&vm) == NULL);
    capture_start();
    actor_exit(&vm, t, R_KILLED, T_EMPTYSTR);   /* w is marked: nothing queued */
    CHECK_EQ(mbox_len(w), 0);
    actor_exit(&vm, w, R_KILLED, T_EMPTYSTR);
    capture_stop();
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* Monitoring a dead or stale pid queues a NoProc DownMsg to the caller at
 * once and leaves no entry; monitoring oneself returns a ref and never
 * delivers. */
TEST(monitor_dead_pid_and_self) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *w = actor_new(&vm, 32), *d = actor_new(&vm, 24);
    term dead = d->id;
    actor_exit(&vm, d, R_NORMAL, T_EMPTYSTR);
    term r = sig_monitor(&vm, w, dead);
    CHECK(r != T_NIL);
    CHECK_EQ(mbox_len(w), 1);
    uint64_t sn; int32_t c; term f;
    CHECK_EQ(recv_down(&vm, w, &sn, &c, &f), dead);
    CHECK_EQ(c, R_NO_PROC);
    CHECK_EQ(f, T_EMPTYSTR);
    CHECK_EQ(sn, ref_serial_in(w, r));
    term r2 = sig_monitor(&vm, w, term_make_pid(3, 0));   /* never used slot */
    CHECK(r2 != T_NIL && ref_serial_in(w, r2) != sn);
    size_t held = va_used_bytes(&vm);
    term rs = sig_monitor(&vm, w, w->id);
    CHECK(rs != T_NIL);
    CHECK_EQ(va_used_bytes(&vm), held);                      /* no entry, no block */
    sig_demonitor(&vm, w, ref_serial_in(w, rs));
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* With memory limited, sig_monitor either succeeds completely or returns
 * T_NIL with nothing that survives the two actors. */
TEST(monitor_prealloc_oom) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    uint32_t failed = 0, ok = 0;
    for (size_t extra = 0; extra <= 384; extra += 8) {   /* past the full cost */
        Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24);
        va_set_limit(&vm, va_used_bytes(&vm) + extra);
        term r = sig_monitor(&vm, w, t->id);
        va_set_limit(&vm, 0);
        if (r == T_NIL) failed++; else ok++;
        actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
        actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
        CHECK_EQ(va_used_bytes(&vm), base);
    }
    CHECK(failed > 0 && ok > 0);
    /* Same for a dead pid: the NoProc message is allocated normally. */
    Actor *w = actor_new(&vm, 32), *d = actor_new(&vm, 24);
    term dead = d->id;
    actor_exit(&vm, d, R_NORMAL, T_EMPTYSTR);
    va_set_limit(&vm, va_used_bytes(&vm));
    CHECK(sig_monitor(&vm, w, dead) == T_NIL);
    va_set_limit(&vm, 0);
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* SpawnLink with room for the child but not for its link blocks: the
 * spawner crashes with oom, the half-made child is freed, nothing leaks. */
TEST(spawn_link_prealloc_oom) {
    static const uint32_t code[] = {
        OP_MAKE(OP_MKCLOSURE, 0, 0, 0), 0,
        OP_MAKE(OP_BUILTIN, 1, 0, 1), B_SPAWN_LINK,
        OP_MAKE(OP_RET, 1, 0, 0),
    };
    uint32_t w[IMG_MAX]; Image img; Vm vm;
    va_reset(&vm);
    CHECK_EQ(image_load(&img, w, build(w, code, 5)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes(&vm);
    uint32_t oom = 0, ok = 0;
    for (size_t extra = 0; extra <= 1024; extra += 8) {
        Actor *a = start(&vm, &img, 32);
        va_set_limit(&vm, va_used_bytes(&vm) + extra);
        capture_start();
        CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
        capture_stop();
        va_set_limit(&vm, 0);
        if (strstr(err_buf, "crashed: oom")) {
            oom++;
            CHECK_EQ(vm.live, 0);                  /* spawner and child gone */
            CHECK(runq_pop(&vm) == NULL);
        } else {
            ok++;
            CHECK_EQ(vm.live, 1);                  /* the linked child */
            Actor *child = runq_pop(&vm);
            CHECK(child != NULL);
            capture_start();
            actor_exit(&vm, child, R_NORMAL, T_EMPTYSTR);
            capture_stop();
        }
        CHECK_EQ(vm.live, 0);
        CHECK_EQ(va_used_bytes(&vm), base);
    }
    /* The limit was raised in steps of 8 bytes until the spawn worked, and
     * a link needs two blocks of well over 8 bytes: some oom runs got past
     * the child's allocation and failed at the link blocks. */
    CHECK(oom > 1 && ok > 0);
}

/* Monitors are charged to the watcher: with room for exactly three, the
 * fourth fails with nothing made (the interpreter then exits the watcher
 * with oom), an unrelated actor is untouched, and the charge goes back to
 * zero on Demonitor, on delivery and on the watcher's exit. */
TEST(monitor_charged_to_watcher) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *w = actor_new(&vm, 32), *t = actor_new(&vm, 24), *u = actor_new(&vm, 24);
    term tid = t->id;
    CHECK_EQ(actor_extra_charged(w), 0);
    term r = sig_monitor(&vm, w, tid);
    CHECK(r != T_NIL);
    uint32_t per = actor_extra_charged(w);
    CHECK(per > 14u);                       /* entries + the DownMsg block */
    CHECK_EQ(actor_extra_charged(t), 0);    /* nothing charged to the target */
    sig_demonitor(&vm, w, ref_serial_in(w, r));
    CHECK_EQ(actor_extra_charged(w), 0);
    /* Room for three monitors, not four; the refs (3 words each) fit in
     * the block's free space, so it does not grow. */
    uint16_t mem = w->mem_words;
    w->max_words = (uint16_t)(mem + w->mbox_words + 3u * per + per / 2u);
    term refs[3];
    for (int i = 0; i < 3; i++) {
        refs[i] = sig_monitor(&vm, w, tid);
        CHECK(refs[i] != T_NIL);
    }
    CHECK_EQ(w->mem_words, mem);
    CHECK_EQ(actor_extra_charged(w), 3u * per);
    size_t held = va_used_bytes(&vm);
    CHECK(sig_monitor(&vm, w, tid) == T_NIL);
    CHECK_EQ(actor_extra_charged(w), 3u * per);
    CHECK_EQ(va_used_bytes(&vm), held);        /* no entries, no block */
    CHECK_EQ(actor_extra_charged(u), 0);    /* unrelated actor untouched */
    CHECK_EQ(mbox_len(u), 0);
    CHECK(!(u->flags & AF_EXIT_PENDING));
    /* Demonitor releases one; the target's exit releases the rest as the
     * DownMsgs move into w's mailbox (where mbox_words counts them). */
    sig_demonitor(&vm, w, ref_serial_in(w, refs[0]));
    CHECK_EQ(actor_extra_charged(w), 2u * per);
    actor_exit(&vm, t, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(actor_extra_charged(w), 0);
    CHECK_EQ(mbox_len(w), 2);
    /* The watcher's own exit releases its monitors of a live target and
     * leaves no entry on the target. */
    Actor *t2 = actor_new(&vm, 24);
    w->max_words = 0;
    CHECK(sig_monitor(&vm, w, t2->id) != T_NIL);
    CHECK(sig_monitor(&vm, w, t2->id) != T_NIL);
    CHECK_EQ(actor_extra_charged(w), 2u * per);
    actor_exit(&vm, w, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, t2, R_NORMAL, T_EMPTYSTR);
    /* An actor freed without exiting (never ran) releases its watchers'
     * charges too. */
    Actor *w3 = actor_new(&vm, 32), *n = actor_new(&vm, 24);
    CHECK(sig_monitor(&vm, w3, n->id) != T_NIL);
    CHECK_EQ(actor_extra_charged(w3), per);
    actor_free(&vm, n);
    CHECK_EQ(actor_extra_charged(w3), 0);
    actor_exit(&vm, w3, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, u, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* Each link side is charged to its own actor: released on unlink, on the
 * peer's exit and on a free without exit; a link that does not fit the
 * caller's limit makes nothing. */
TEST(link_charged) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 4), 0);
    size_t base = va_used_bytes(&vm);
    Actor *a = actor_new(&vm, 24), *b = actor_new(&vm, 24), *c = actor_new(&vm, 24);
    CHECK(sig_link(&vm, a, b->id));
    uint32_t per = actor_extra_charged(a);
    CHECK(per > 10u);                       /* entry + the ExitMsg block */
    CHECK_EQ(actor_extra_charged(b), per);
    CHECK(sig_link(&vm, b, a->id));         /* deduplicated: no new charge */
    CHECK_EQ(actor_extra_charged(a), per);
    sig_unlink(&vm, b, a->id);
    CHECK_EQ(actor_extra_charged(a), 0);
    CHECK_EQ(actor_extra_charged(b), 0);
    /* Delivery: the peer's exit releases the survivor's side. */
    CHECK(sig_link(&vm, a, b->id));
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(actor_extra_charged(a), 0);
    /* Over the caller's limit: no link, no charge, peer untouched. */
    a->max_words = (uint16_t)(a->mem_words + per - 1u);
    CHECK(!sig_link(&vm, a, c->id));
    CHECK(!sig_is_linked(a, c->id) && !sig_is_linked(c, a->id));
    CHECK_EQ(actor_extra_charged(a), 0);
    CHECK_EQ(actor_extra_charged(c), 0);
    a->max_words = 0;
    /* The caller's exit releases the peer's side. */
    CHECK(sig_link(&vm, a, c->id));
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(actor_extra_charged(c), 0);
    /* A peer freed without exiting (never ran) unlinks and releases. */
    Actor *n = actor_new(&vm, 24);
    CHECK(sig_link(&vm, c, n->id));
    actor_free(&vm, n);
    CHECK(!sig_is_linked(c, n->id));
    CHECK_EQ(actor_extra_charged(c), 0);
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);
}
