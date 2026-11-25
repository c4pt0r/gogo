#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "term.h"
#include <string.h>


/* Words of the message msg_build makes for t (UINT32_MAX if none): the
 * test-side size check (the VM has no separate size-only entry). */
static uint32_t copy_words(Vm *vm, Actor *a, term t) {
    Msg *m = msg_build(vm, a, t, a->mod, NULL);
    uint32_t w = m ? m->words : UINT32_MAX;
    if (m) msg_free(vm, m);
    return w;
}

static Actor *mk(Vm *vm, uint16_t words) {
    Actor *a = actor_new(vm, words);
    CHECK(a != NULL);
    return a;
}

/* Allocates and fills a K_STRUCT {type_id, fields...} in a; returns its ref. */
static term mk_struct(Actor *a, uint32_t type_id, const term *f, uint32_t n) {
    uint32_t off = heap_alloc(NULL, a, n + 2, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(n + 2, K_STRUCT);
    a->mem[off + 1] = type_id;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term fld(Actor *a, term ref, uint32_t i) {
    return a->mem[term_ref_off(ref) + 2 + i];
}

TEST(send_int) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    CHECK_EQ(copy_words(&vm, a, term_from_int(42)), 0);
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(42)), 0);
    CHECK(b->mbox_head != NULL);
    CHECK_EQ(b->mbox_head->words, 0);
    term out = T_NIL;
    CHECK(msg_receive(&vm, b, &out));
    CHECK_EQ(out, term_from_int(42));
    CHECK(!msg_receive(&vm, b, &out));
    CHECK_EQ(b->mbox_words, 0);
    CHECK(b->mbox_head == NULL && b->mbox_tail == NULL);
}

TEST(send_struct_nested) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32), *b = mk(&vm, 32);
    term in[2] = { term_from_int(2), term_from_int(3) };
    term inner = mk_struct(a, 8, in, 2);
    term out_f[2] = { term_from_int(1), inner };
    term outer = mk_struct(a, 9, out_f, 2);
    uint16_t hp = a->hp;
    CHECK_EQ(copy_words(&vm, a, outer), 8);
    CHECK_EQ(msg_send(&vm, a, b->id, outer), 0);

    /* sender untouched */
    CHECK_EQ(a->hp, hp);
    CHECK_EQ(fld(a, outer, 0), term_from_int(1));
    CHECK_EQ(fld(a, outer, 1), inner);

    /* receiver has pre-existing heap data, so offsets must be relocated */
    term pre[1] = { term_from_int(7) };
    (void)mk_struct(b, 1, pre, 1);
    term r = T_NIL;
    CHECK(msg_receive(&vm, b, &r));
    CHECK(term_is_ref(r));
    CHECK_EQ(b->mem[term_ref_off(r)], hdr_make(4, K_STRUCT));
    CHECK_EQ(b->mem[term_ref_off(r) + 1], 9);
    CHECK_EQ(fld(b, r, 0), term_from_int(1));
    term ri = fld(b, r, 1);
    CHECK(term_is_ref(ri));
    CHECK(ri != inner);
    CHECK_EQ(b->mem[term_ref_off(ri) + 1], 8);
    CHECK_EQ(fld(b, ri, 0), term_from_int(2));
    CHECK_EQ(fld(b, ri, 1), term_from_int(3));
}

TEST(send_wakes_waiting) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    b->state = A_WAITING;
    CHECK_EQ(msg_send(&vm, a, b->id, T_TRUE), 0);
    CHECK_EQ(b->state, A_RUNNABLE);
    CHECK(runq_pop(&vm) == b);
    CHECK(runq_pop(&vm) == NULL);
    /* a runnable receiver is not enqueued twice */
    CHECK_EQ(msg_send(&vm, a, b->id, T_TRUE), 0);
    CHECK(runq_pop(&vm) == NULL);
}

TEST(send_dead_dropped) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    term pid = b->id;
    actor_free(&vm, b);
    CHECK_EQ(msg_send(&vm, a, pid, term_from_int(1)), 1);
    CHECK_EQ(msg_send(&vm, a, term_from_int(5), term_from_int(1)), 1);
}

TEST(mailbox_fifo) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    for (int i = 1; i <= 3; i++) CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(i)), 0);
    for (int i = 1; i <= 3; i++) {
        term out = T_NIL;
        CHECK(msg_receive(&vm, b, &out));
        CHECK_EQ(out, term_from_int(i));
    }
    term out;
    CHECK(!msg_receive(&vm, b, &out));
}

TEST(mbox_counts_footprint) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32), *b = mk(&vm, 24);
    term f[2] = { term_from_int(1), term_from_int(2) };
    term s = mk_struct(a, 1, f, 2);                 /* 4 words */
    size_t before = actor_memory_bytes(b);
    CHECK_EQ(msg_send(&vm, a, b->id, s), 0);
    size_t queued = actor_memory_bytes(b) - before;
    CHECK(queued >= 16);
    CHECK_EQ(queued, (size_t)b->mbox_words * 4u);
    CHECK(queued >= sizeof(Msg) + 16);
    term out;
    CHECK(msg_receive(&vm, b, &out));
    CHECK_EQ(b->mbox_words, 0);
}

TEST(actor_free_frees_queued_msgs) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32);
    size_t before = va_used_bytes(&vm);
    Actor *b = mk(&vm, 24);
    term f[1] = { term_from_int(1) };
    term s = mk_struct(a, 1, f, 1);
    CHECK_EQ(msg_send(&vm, a, b->id, s), 0);
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(2)), 0);
    actor_free(&vm, b);
    CHECK_EQ(va_used_bytes(&vm), before + 0);
}

TEST(send_string_and_closure) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 40), *b = mk(&vm, 40);
    uint32_t so = heap_alloc(NULL, a, 3, NULL, 0);
    a->mem[so] = hdr_make(3, K_STRING);
    a->mem[so + 1] = 4;
    a->mem[so + 2] = 0x64636260u;               /* low bits 00: not a ref */
    uint32_t co = heap_alloc(NULL, a, 4, NULL, 0);
    a->mem[co] = hdr_make(4, K_CLOSURE);
    a->mem[co + 1] = 4;                         /* func_idx 4 looks like a ref */
    a->mem[co + 2] = term_from_ref(so);
    a->mem[co + 3] = term_from_int(5);
    CHECK_EQ(copy_words(&vm, a, term_from_ref(co)), 7);
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_ref(co)), 0);
    term pre = mk_struct(b, 1, NULL, 0);        /* shift receiver offsets */
    (void)pre;
    term r;
    CHECK(msg_receive(&vm, b, &r));
    uint32_t o = term_ref_off(r);
    CHECK_EQ(b->mem[o + 1], 4);
    CHECK_EQ(b->mem[o + 3], term_from_int(5));
    uint32_t s2 = term_ref_off(b->mem[o + 2]);
    CHECK_EQ(b->mem[s2 + 1], 4);
    CHECK_EQ(b->mem[s2 + 2], 0x64636260u);
}

TEST(receive_triggers_gc) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32), *b = mk(&vm, 24);
    term f[2] = { term_from_int(8), term_from_int(9) };
    term s = mk_struct(a, 1, f, 2);
    CHECK_EQ(msg_send(&vm, a, b->id, s), 0);
    /* fill b's heap with garbage so the receive needs a GC/grow */
    CHECK(heap_alloc(NULL, b, 22, NULL, 0) != UINT32_MAX);
    term r;
    CHECK(msg_receive(&vm, b, &r));
    CHECK_EQ(fld(b, r, 1), term_from_int(9));
}

TEST(send_shared_object_copied_once) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 40), *b = mk(&vm, 40);
    term pf[2] = { term_from_int(1), term_from_int(2) };
    term p = mk_struct(a, 7, pf, 2);
    term qf[2] = { p, p };
    term q = mk_struct(a, 8, qf, 2);
    CHECK_EQ(copy_words(&vm, a, q), 4 + 4);
    Msg *m = msg_build(&vm, a, q, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(m->words, 4 /*Q*/ + 4 /*P*/);
    (void)mk_struct(b, 1, NULL, 0);             /* shift receiver offsets */
    term r = T_NIL;
    CHECK(msg_unpack(&vm, b, m, &r));
    msg_free(&vm, m);
    CHECK_EQ(b->mem[term_ref_off(r) + 1], 8);
    term r0 = fld(b, r, 0), r1 = fld(b, r, 1);
    CHECK(term_is_ref(r0));
    CHECK_EQ(r0, r1);
    CHECK_EQ(b->mem[term_ref_off(r0) + 1], 7);
    CHECK_EQ(fld(b, r0, 0), term_from_int(1));
    CHECK_EQ(fld(b, r0, 1), term_from_int(2));
}

/* 20 levels, each a struct whose two fields both reference the previous
 * level: a tree copy would need 2^20 objects. */
static term mk_dag(Actor *a, int levels) {
    term f[2] = { term_from_int(0), term_from_int(0) };
    term prev = mk_struct(a, 1, f, 2);
    for (int i = 1; i < levels; i++) {
        f[0] = prev; f[1] = prev;
        prev = mk_struct(a, 1, f, 2);
    }
    return prev;
}

TEST(send_dag_no_blowup) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 120), *b = mk(&vm, 120);
    term top = mk_dag(a, 20);
    Msg *m = msg_build(&vm, a, top, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(m->words, 20 * 4);
    term r = T_NIL;
    CHECK(msg_unpack(&vm, b, m, &r));
    msg_free(&vm, m);
    /* walk down the received chain: both fields share one copy per level */
    for (int i = 1; i < 20; i++) {
        CHECK_EQ(fld(b, r, 0), fld(b, r, 1));
        r = fld(b, r, 0);
        CHECK(term_is_ref(r));
    }
    CHECK_EQ(fld(b, r, 0), term_from_int(0));
}

TEST(send_restores_source) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 120);
    term top = mk_dag(a, 20);
    uint32_t before[120];
    CHECK_EQ(a->mem_words, 120);
    memcpy(before, a->mem, sizeof before);
    Msg *m = msg_build(&vm, a, top, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(memcmp(before, a->mem, sizeof before), 0);
    msg_free(&vm, m);
}

TEST(send_restores_source_on_alloc_failure) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 120);
    term top = mk_dag(a, 20);                   /* 80 words: buffer must grow */
    uint32_t before[120];
    memcpy(before, a->mem, sizeof before);
    size_t used = va_used_bytes(&vm);
    va_set_limit(&vm, used + 100);                   /* 16-word buffer fits, growth fails */
    CHECK(msg_build(&vm, a, top, 0, NULL) == NULL);
    CHECK_EQ(memcmp(before, a->mem, sizeof before), 0);
    CHECK_EQ(va_used_bytes(&vm), used);
    va_set_limit(&vm, 0);
}

/* p -> q -> p: the copy terminates, keeps the cycle, restores the sender. */
TEST(send_cycle) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 40), *b = mk(&vm, 40);
    term pf[1] = { term_from_int(0) };
    term p = mk_struct(a, 1, pf, 1);
    term qf[1] = { p };
    term q = mk_struct(a, 2, qf, 1);
    a->mem[term_ref_off(p) + 2] = q;            /* close the cycle */
    uint32_t before[40];
    memcpy(before, a->mem, sizeof before);
    Msg *m = msg_build(&vm, a, p, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(m->words, 3 + 3);
    CHECK_EQ(memcmp(before, a->mem, sizeof before), 0);
    (void)mk_struct(b, 9, NULL, 0);             /* shift receiver offsets */
    term r = T_NIL;
    CHECK(msg_unpack(&vm, b, m, &r));
    msg_free(&vm, m);
    term rq = fld(b, r, 0);
    CHECK(term_is_ref(rq));
    CHECK_EQ(b->mem[term_ref_off(rq) + 1], 2);
    CHECK_EQ(fld(b, rq, 0), r);
}

/* A queued message that fits the limit once it leaves the mailbox is
 * received without oom: it is not counted in both mailbox and heap. */
TEST(receive_near_limit) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 80), *b = mk(&vm, 24);
    b->max_words = 100;
    CHECK(heap_push_frame(&vm, b, 1, -1));
    term f[58];
    for (int i = 0; i < 58; i++) f[i] = term_from_int(i);
    term s = mk_struct(a, 3, f, 58);            /* 60 words */
    CHECK_EQ(msg_send(&vm, a, b->id, s), 0);
    CHECK(b->mbox_words >= 60);
    term r = T_NIL;
    bool ok = msg_receive(&vm, b, &r);
    CHECK(ok);
    if (!ok) return;
    CHECK_EQ(b->mbox_words, 0);
    CHECK(b->mem_words <= 100);
    CHECK_EQ(fld(b, r, 57), term_from_int(57));
}

/* A 600-node list (2400+ words, within the MCU limit too) where every node also points at one shared leaf, plus a
 * shared tail pair: the copy is sized first and allocated once, so the
 * allocator's peak during msg_build exceeds its start by no more than the
 * message itself (+256 slack). */
TEST(msg_build_peak) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 3000);
    if (!a) return;
    term leaf_f[1] = { term_from_int(7) };
    term leaf = mk_struct(a, 3, leaf_f, 1);
    term head = T_NIL;
    for (int i = 0; i < 600; i++) {
        term f[2] = { leaf, head };            /* shared leaf, list link */
        head = mk_struct(a, 4, f, 2);
    }
    uint32_t words = copy_words(&vm, a, head);
    CHECK_EQ(words, 600 * 4 + 3);
    size_t before = va_used_bytes(&vm);
    va_reset_peak(&vm);
    Msg *m = msg_build(&vm, a, head, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(m->words, words);
    CHECK(va_peak_bytes(&vm) <= before + msg_alloc_bytes(m->words) + 256u);
    CHECK_EQ(va_used_bytes(&vm), before + msg_alloc_bytes(m->words));
    /* The leaf is copied once, and the sender's heap is intact. */
    CHECK_EQ(fld(a, head, 0), leaf);
    CHECK_EQ(a->mem[term_ref_off(leaf)], hdr_make(3, K_STRUCT));
    msg_free(&vm, m);
}

/* Cycles and shared substructure, including deep chains in both address
 * directions, size and copy exactly (pointer reversal must restore). */
TEST(msg_size_cycle) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 64);
    term f[2] = { term_from_int(1), T_NIL };
    term x = mk_struct(a, 5, f, 2);
    term g[2] = { x, x };
    term y = mk_struct(a, 6, g, 2);
    a->mem[term_ref_off(x) + 3] = y;             /* x.1 -> y -> x */
    CHECK_EQ(copy_words(&vm, a, y), 8);
    CHECK_EQ(fld(a, x, 1), y);
    CHECK_EQ(fld(a, y, 0), x);
    CHECK_EQ(fld(a, y, 1), x);
    Msg *m = msg_build(&vm, a, y, 0, NULL);
    CHECK(m != NULL);
    if (m) { CHECK_EQ(m->words, 8); msg_free(&vm, m); }
    CHECK_EQ(a->mem[term_ref_off(x)], hdr_make(4, K_STRUCT));
}

/* Non-fatal delivery (M5 §9.1): a message that would take the receiver
 * past its quota is refused and left to the caller; the receiver is not
 * marked and keeps running. One that fits is queued like msg_enqueue. */
TEST(try_enqueue_never_kills) {
    Vm vm; va_reset(&vm);
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 80), *b = mk(&vm, 24);
    b->max_words = 80;
    b->state = A_WAITING;
    term f[58];
    for (int i = 0; i < 58; i++) f[i] = term_from_int(i);
    term s = mk_struct(a, 3, f, 58);            /* 60 words: 24 + 60+ > 80 */
    Msg *m = msg_build(&vm, a, s, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(!msg_try_enqueue(&vm, b, m));
    CHECK(!(b->flags & AF_EXIT_PENDING));
    CHECK(b->mbox_head == NULL);
    CHECK_EQ(b->mbox_words, 0);
    CHECK_EQ(b->state, A_WAITING);
    CHECK_EQ(m->words, 60);                     /* still the caller's */
    msg_free(&vm, m);

    /* A small one fits, is queued and wakes b. */
    m = msg_build(&vm, a, term_from_int(5), 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(msg_try_enqueue(&vm, b, m));
    CHECK(b->mbox_head == m);
    CHECK_EQ(b->state, A_RUNNABLE);
    CHECK(runq_pop(&vm) == b);
    term r = T_NIL;
    CHECK(msg_receive(&vm, b, &r));
    CHECK_EQ(r, term_from_int(5));

    /* A receiver marked for exit takes nothing. */
    m = msg_build(&vm, a, term_from_int(6), 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    b->flags |= AF_EXIT_PENDING;
    CHECK(!msg_try_enqueue(&vm, b, m));
    CHECK(b->mbox_head == NULL);
    b->flags &= (uint8_t)~AF_EXIT_PENDING;
    msg_free(&vm, m);
}
