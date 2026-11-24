/* Call / Reply: reply blocks in the mailbox, selective receive by Ref
 * serial, and that a blocked Call sends its request exactly once. */
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

/* main (0 params, 6 regs):
 *   r0 = Self(); r1 = 7; r2 = 500; r3, r4 = Call(r0, r1, r2);
 *   Sleep(r2) (blocks, so r3/r4 can be inspected; ignores messages); RET
 * Layout: 8 header + 3 func + 3 consts + 11 code = 25 words. */
#define IMG_WORDS 25
static void build_call(uint32_t *w, int32_t ms) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 11; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (6u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_BUILTIN, 0, 0, 0); w[15] = B_SELF;
    w[16] = OP_MAKE(OP_LOADK, 1, 0, 0); w[17] = term_from_int(7);
    w[18] = OP_MAKE(OP_LOADK, 2, 0, 0); w[19] = term_from_int(ms);
    w[20] = OP_MAKE(OP_BUILTIN, 3, 0, 3); w[21] = B_CALL;
    w[22] = OP_MAKE(OP_BUILTIN, 0, 2, 1); w[23] = B_SLEEP;
    w[24] = OP_MAKE(OP_RET, 0, 0, 0);
}

static void setup(uint32_t *w, Image *img, Vm *vm, int32_t ms) {
    va_reset();
    build_call(w, ms);
    CHECK_EQ(image_load(img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(vm, img, 8), 0);
}

static Actor *start(Vm *vm, const Image *img) {
    Actor *a = actor_new(vm, 64);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 6, -1));
    a->pc = img->code;
    return a;
}

static int timer_count(const Vm *vm) {
    return (int)timer_pending(vm);
}

#define REGS_OF(a) ((a)->mem + (a)->fp)

static int mbox_count(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* Sum of the footprints of the queued messages: what mbox_words must be. */
static uint32_t mbox_sum(const Actor *a) {
    uint32_t n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n += msg_footprint_words(m->words);
    return n;
}

/* A K_REF with this serial on s's heap. */
static term make_ref(Vm *vm, Actor *s, uint64_t serial) {
    uint32_t off = heap_alloc(vm, s, 3, NULL, 0);
    CHECK(off != UINT32_MAX);
    s->mem[off] = hdr_make(3, K_REF);
    s->mem[off + 1] = (uint32_t)serial;
    s->mem[off + 2] = (uint32_t)(serial >> 32);
    return term_from_ref(off);
}

/* A struct {x} with a nested struct, to exercise ref relocation. */
static term make_body(Vm *vm, Actor *s, int32_t x) {
    uint32_t in = heap_alloc(vm, s, 3, NULL, 0);
    s->mem[in] = hdr_make(3, K_STRUCT); s->mem[in + 1] = 0; s->mem[in + 2] = term_from_int(x);
    uint32_t out = heap_alloc(vm, s, 3, NULL, 0);
    s->mem[out] = hdr_make(3, K_STRUCT); s->mem[out + 1] = 0; s->mem[out + 2] = term_from_ref(in);
    return term_from_ref(out);
}

static int32_t body_x(const Actor *a, term t) {
    const uint32_t *o = a->mem + term_ref_off(t);
    const uint32_t *in = a->mem + term_ref_off(o[2]);
    return term_to_int(in[2]);
}

/* Receive skips and frees reply blocks and keeps mbox_words exact. */
TEST(reply_skipped_by_receive) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 50);
    size_t base = va_used_bytes();
    Actor *r = actor_new(&vm, 64), *s = actor_new(&vm, 64);
    term ref = make_ref(&vm, s, 77);
    CHECK_EQ(msg_send_reply(&vm, s, r->id, ref, make_body(&vm, s, 1)), 0);
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(5)), 0);
    CHECK_EQ(msg_send_reply(&vm, s, r->id, ref, term_from_int(2)), 0);
    CHECK_EQ(mbox_count(r), 3);
    CHECK_EQ(r->mbox_words, mbox_sum(r));

    term t;
    CHECK(msg_receive(&vm, r, &t));            /* skipped the head reply */
    CHECK_EQ(t, term_from_int(5));
    CHECK_EQ(mbox_count(r), 1);                /* the second reply stays queued... */
    CHECK_EQ(r->mbox_words, mbox_sum(r));
    CHECK(!msg_receive(&vm, r, &t));           /* ...until Receive meets it */
    CHECK(r->mbox_head == NULL && r->mbox_tail == NULL);
    CHECK_EQ(r->mbox_words, 0);
    actor_free(&vm, r); actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* msg_take_reply: takes the matching reply (body relocated), frees stale
 * ones, leaves ordinary messages queued. */
TEST(take_reply_selective) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 50);
    size_t base = va_used_bytes();
    Actor *r = actor_new(&vm, 64), *s = actor_new(&vm, 64);
    term old = make_ref(&vm, s, 1), cur = make_ref(&vm, s, 0x100000002ull);
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(5)), 0);
    CHECK_EQ(msg_send_reply(&vm, s, r->id, old, term_from_int(9)), 0);
    CHECK_EQ(msg_send_reply(&vm, s, r->id, cur, make_body(&vm, s, 42)), 0);
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(6)), 0);

    term t;
    CHECK_EQ(msg_take_reply(&vm, r, 3, &t), 0);          /* no such serial */
    CHECK_EQ(mbox_count(r), 2);                          /* stale one freed */
    CHECK_EQ(r->mbox_words, mbox_sum(r));
    CHECK_EQ(msg_send_reply(&vm, s, r->id, cur, make_body(&vm, s, 42)), 0);
    CHECK_EQ(msg_take_reply(&vm, r, 0x100000002ull, &t), 1);
    CHECK_EQ(body_x(r, t), 42);
    CHECK_EQ(mbox_count(r), 2);
    CHECK_EQ(r->mbox_words, mbox_sum(r));
    CHECK(msg_receive(&vm, r, &t)); CHECK_EQ(t, term_from_int(5));
    CHECK(msg_receive(&vm, r, &t)); CHECK_EQ(t, term_from_int(6));
    CHECK_EQ(r->mbox_words, 0);
    actor_free(&vm, r); actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* A dead or stale target drops the reply silently. */
TEST(reply_dead_target_drops) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 50);
    size_t base = va_used_bytes();
    Actor *r = actor_new(&vm, 64), *s = actor_new(&vm, 64);
    term to = r->id;
    actor_free(&vm, r);
    CHECK_EQ(msg_send_reply(&vm, s, to, make_ref(&vm, s, 1), term_from_int(1)), 1);
    actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* The caller is woken by unrelated messages while waiting: it re-blocks
 * without sending the request again, then takes the reply. */
TEST(call_no_resend_on_resume) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 500);
    size_t base = va_used_bytes();
    Actor *c = start(&vm, &img), *s = actor_new(&vm, 64);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
    c->state = A_WAITING;
    CHECK(c->pc == img.code + 6);                /* on the CALL */
    CHECK(c->flags & AF_CALL_PENDING);
    CHECK(c->flags & AF_WAKEUP_ARMED);
    CHECK_EQ(mbox_count(c), 1);                  /* the request, to itself */
    CHECK_EQ(c->mbox_head->data[1], TYPE_REQUEST);

    for (int i = 0; i < 3; i++) {                /* unrelated messages */
        CHECK_EQ(msg_send(&vm, s, c->id, term_from_int(i)), 0);
        CHECK(runq_pop(&vm) == c);
        CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
        c->state = A_WAITING;
        CHECK(c->pc == img.code + 6);
        CHECK(c->flags & AF_CALL_PENDING);
        CHECK_EQ(mbox_count(c), 2 + i);          /* request + ordinary ones only */
    }
    CHECK_EQ(timer_count(&vm), 1);

    /* The Ref the call waits on is in r3; answer it. */
    term ref = REGS_OF(c)[3];
    CHECK(term_is_ref(ref));
    const uint32_t *ro = c->mem + term_ref_off(ref);
    term sref = make_ref(&vm, s, (uint64_t)ro[1] | ((uint64_t)ro[2] << 32));
    CHECK_EQ(msg_send_reply(&vm, s, c->id, sref, term_from_int(99)), 0);
    CHECK(runq_pop(&vm) == c);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);      /* now blocked in Sleep */
    c->state = A_WAITING;
    CHECK(c->pc == img.code + 8);
    CHECK_EQ(REGS_OF(c)[3], term_from_int(99));
    CHECK_EQ(REGS_OF(c)[4], T_TRUE);
    CHECK(!(c->flags & (AF_CALL_PENDING | AF_TIMED_OUT)));
    CHECK_EQ(timer_count(&vm), 1);               /* only the Sleep's wakeup */
    CHECK_EQ(c->mbox_words, mbox_sum(c));
    CHECK_EQ(mbox_count(c), 4);                  /* request + 3 ordinary */
    c->state = A_RUNNABLE;
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* Timeout: (nil, false), pending state cleared, a late reply is invisible. */
TEST(call_timeout_clears_state) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 500);
    size_t base = va_used_bytes();
    Actor *c = start(&vm, &img), *s = actor_new(&vm, 64);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
    c->state = A_WAITING;
    term ref = REGS_OF(c)[3];
    const uint32_t *ro = c->mem + term_ref_off(ref);
    term sref = make_ref(&vm, s, (uint64_t)ro[1] | ((uint64_t)ro[2] << 32));
    uint64_t next;
    CHECK(timer_next(&vm, &next));
    timer_fire_due(&vm, next);
    CHECK(runq_pop(&vm) == c);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
    c->state = A_WAITING;
    CHECK(c->pc == img.code + 8);
    CHECK_EQ(REGS_OF(c)[3], T_NIL);
    CHECK_EQ(REGS_OF(c)[4], T_FALSE);
    CHECK(!(c->flags & (AF_CALL_PENDING | AF_TIMED_OUT)));

    /* The late reply is invisible to Receive: it only sees the request. */
    CHECK_EQ(msg_send_reply(&vm, s, c->id, sref, term_from_int(1)), 0);
    CHECK_EQ(mbox_count(c), 2);
    term t;
    CHECK(msg_receive(&vm, c, &t));
    CHECK(term_is_ref(t));                       /* the Request to itself */
    CHECK(!msg_receive(&vm, c, &t));             /* reply skipped and freed */
    CHECK_EQ(mbox_count(c), 0);
    CHECK_EQ(c->mbox_words, 0);
    c->state = A_RUNNABLE;
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* A dead target answers (nil, false) at once: no request, no wait. */
TEST(call_dead_target_immediate) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 5000);
    size_t base = va_used_bytes();
    Actor *c = start(&vm, &img);
    /* Patch r0 = Self() into r0 = r5 and put a stale pid in r5. */
    Actor *tmp = actor_new(&vm, 32);
    term dead = tmp->id;
    actor_free(&vm, tmp);
    w[14] = OP_MAKE(OP_MOVE, 0, 5, 0); w[15] = OP_MAKE(OP_NOP, 0, 0, 0);
    REGS_OF(c)[5] = dead;
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);      /* straight to Sleep */
    CHECK(c->pc == img.code + 8);
    CHECK_EQ(REGS_OF(c)[3], T_NIL);
    CHECK_EQ(REGS_OF(c)[4], T_FALSE);
    CHECK_EQ(mbox_count(c), 0);
    CHECK(!(c->flags & AF_CALL_PENDING));
    c->state = A_RUNNABLE;
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(), base);
}

/* A stale reply wakes an actor blocked in plain Receive: it skips the reply
 * and blocks again (no out-of-memory crash). */
TEST(stale_reply_reblocks_receive) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset();
    build_call(w, 50);
    w[20] = OP_MAKE(OP_BUILTIN, 3, 0, 0); w[21] = B_RECEIVE;   /* r3 = Receive() */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 8), 0);
    size_t base = va_used_bytes();
    Actor *c = start(&vm, &img), *s = actor_new(&vm, 64);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
    c->state = A_WAITING;
    CHECK_EQ(msg_send_reply(&vm, s, c->id, make_ref(&vm, s, 5), term_from_int(1)), 0);
    CHECK(runq_pop(&vm) == c);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);      /* re-blocked, still alive */
    c->state = A_WAITING;
    CHECK(c->pc == img.code + 6);
    CHECK_EQ(mbox_count(c), 0);
    CHECK_EQ(c->mbox_words, 0);
    c->state = A_RUNNABLE;
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    actor_free(&vm, s);
    CHECK_EQ(va_used_bytes(), base);
}

/* ms <= 0: the request is sent once, no reply is queued, so (nil, false)
 * comes back at once and the caller moves on. */
TEST(call_zero_ms_sends_once_returns_false) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm, 0);
    size_t base = va_used_bytes();
    w[22] = OP_MAKE(OP_BUILTIN, 0, 1, 1);        /* Sleep(r1 = 7): blocks */
    Actor *c = start(&vm, &img);
    CHECK_EQ(interp_run(&vm, c), RUN_WAIT);
    c->state = A_WAITING;
    CHECK(c->pc == img.code + 8);
    CHECK_EQ(REGS_OF(c)[3], T_NIL);
    CHECK_EQ(REGS_OF(c)[4], T_FALSE);
    CHECK_EQ(mbox_count(c), 1);                  /* the one request */
    CHECK(!(c->flags & AF_CALL_PENDING));
    CHECK_EQ(timer_count(&vm), 1);               /* only Sleep's wakeup */
    c->state = A_RUNNABLE;
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(), base);
}
