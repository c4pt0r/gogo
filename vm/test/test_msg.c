#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "term.h"

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
    Vm vm; va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    CHECK_EQ(msg_copy_words(a, term_from_int(42)), 0);
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
    Vm vm; va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32), *b = mk(&vm, 32);
    term in[2] = { term_from_int(2), term_from_int(3) };
    term inner = mk_struct(a, 8, in, 2);
    term out_f[2] = { term_from_int(1), inner };
    term outer = mk_struct(a, 9, out_f, 2);
    uint16_t hp = a->hp;
    CHECK_EQ(msg_copy_words(a, outer), 8);
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
    Vm vm; va_reset();
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
    Vm vm; va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 24), *b = mk(&vm, 24);
    term pid = b->id;
    actor_free(&vm, b);
    CHECK_EQ(msg_send(&vm, a, pid, term_from_int(1)), 1);
    CHECK_EQ(msg_send(&vm, a, term_from_int(5), term_from_int(1)), 1);
}

TEST(mailbox_fifo) {
    Vm vm; va_reset();
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
    Vm vm; va_reset();
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
    Vm vm; va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32);
    size_t before = va_used_bytes();
    Actor *b = mk(&vm, 24);
    term f[1] = { term_from_int(1) };
    term s = mk_struct(a, 1, f, 1);
    CHECK_EQ(msg_send(&vm, a, b->id, s), 0);
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(2)), 0);
    actor_free(&vm, b);
    CHECK_EQ(va_used_bytes(), before + 0);
}

TEST(send_string_and_closure) {
    Vm vm; va_reset();
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
    CHECK_EQ(msg_copy_words(a, term_from_ref(co)), 7);
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
    Vm vm; va_reset();
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

TEST(mbox_overflow_is_mailbox_full) {
    Vm vm; va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = mk(&vm, 32), *b = mk(&vm, 24);
    b->mbox_words = 65535;
    CHECK_EQ(msg_send(&vm, a, b->id, term_from_int(1)), -2);
    CHECK(b->mbox_head == NULL);
}
