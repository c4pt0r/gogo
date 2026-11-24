#include "test.h"
#include "actor.h"
#include "heap.h"
#include "alloc.h"
#include "term.h"
#include "config.h"

/* Writes a K_STRUCT of n fields at off: [hdr][type_id][fields...]. */
static void put_struct(Actor *a, uint32_t off, uint32_t type_id,
                       const term *fields, uint32_t n) {
    a->mem[off] = hdr_make(n + 2, K_STRUCT);
    a->mem[off + 1] = type_id;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = fields[i];
}

static term field(Actor *a, term ref, uint32_t i) {
    return a->mem[term_ref_off(ref) + 2 + i];
}

static Actor *setup(Vm *vm, uint16_t words) {
    va_reset();
    CHECK_EQ(vm_init(vm, NULL, 16), 0);
    Actor *a = actor_new(vm, words);
    CHECK(a != NULL);
    return a;
}

TEST(actor_size) {
    CHECK(sizeof(Actor) <= 48 + 6 * (sizeof(void *) - 4));
}

TEST(push_frame_layout) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 2, -1));
    CHECK_EQ(a->sp, 20);
    CHECK_EQ(a->fp, 20);
    CHECK_EQ(a->mem[20], T_NIL);
    CHECK_EQ(a->mem[21], T_NIL);
    CHECK_EQ(a->mem[22], term_from_int(0));     /* no caller */
    CHECK_EQ(a->mem[23], term_from_int(-1));
    CHECK(heap_push_frame(&vm, a, 1, 7));
    CHECK_EQ(a->fp, 17);
    CHECK_EQ(a->mem[18], term_from_int(24 - 20)); /* top-relative caller fp */
    CHECK_EQ(a->mem[19], term_from_int(7));
}

TEST(alloc_and_gc_keeps_reachable) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 2, -1));
    term f3[3] = { term_from_int(10), T_TRUE, term_from_int(-3) };
    uint32_t off = heap_alloc(&vm, a, 5, NULL, 0);
    put_struct(a, off, 7, f3, 3);
    a->mem[a->fp] = term_from_ref(off);
    term f1[1] = { term_from_int(99) };
    uint32_t junk = heap_alloc(&vm, a, 3, NULL, 0);
    put_struct(a, junk, 1, f1, 1);
    CHECK_EQ(a->hp, 8);

    heap_gc(&vm, a, 0, NULL, 0);
    CHECK_EQ(a->hp, 5);
    term r0 = a->mem[a->fp];
    CHECK(term_is_ref(r0));
    CHECK_EQ(a->mem[term_ref_off(r0)], hdr_make(5, K_STRUCT));
    CHECK_EQ(a->mem[term_ref_off(r0) + 1], 7);
    CHECK_EQ(field(a, r0, 0), term_from_int(10));
    CHECK_EQ(field(a, r0, 1), T_TRUE);
    CHECK_EQ(field(a, r0, 2), term_from_int(-3));
    CHECK_EQ(a->mem[a->fp + 1], T_NIL);
    CHECK_EQ(a->mem[a->fp + 2], term_from_int(0));
    CHECK_EQ(a->mem[a->fp + 3], term_from_int(-1));
}

TEST(gc_follows_nested) {
    Vm vm;
    Actor *a = setup(&vm, 32);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    uint32_t junk = heap_alloc(&vm, a, 4, NULL, 0);   /* garbage first */
    term fj[2] = { term_from_int(1), term_from_int(2) };
    put_struct(a, junk, 0, fj, 2);
    uint32_t b = heap_alloc(&vm, a, 4, NULL, 0);
    term fb[2] = { term_from_int(42), T_FALSE };
    put_struct(a, b, 2, fb, 2);
    uint32_t aa = heap_alloc(&vm, a, 4, NULL, 0);
    term fa[2] = { term_from_ref(b), term_from_int(5) };
    put_struct(a, aa, 3, fa, 2);
    a->mem[a->fp] = term_from_ref(aa);

    heap_gc(&vm, a, 0, NULL, 0);
    CHECK_EQ(a->hp, 8);
    term ra = a->mem[a->fp];
    term rb = field(a, ra, 0);
    CHECK(term_is_ref(rb));
    CHECK_EQ(a->mem[term_ref_off(rb) + 1], 2);
    CHECK_EQ(field(a, rb, 0), term_from_int(42));
    CHECK_EQ(field(a, rb, 1), T_FALSE);
    CHECK_EQ(field(a, ra, 1), term_from_int(5));
}

TEST(gc_shared_object_copied_once) {
    Vm vm;
    Actor *a = setup(&vm, 32);
    CHECK(heap_push_frame(&vm, a, 2, -1));
    uint32_t s = heap_alloc(&vm, a, 3, NULL, 0);
    a->mem[s] = hdr_make(3, K_STRING);
    a->mem[s + 1] = 4;
    a->mem[s + 2] = 0x64636260u;   /* raw bytes with low bits 00: must not be followed */
    a->mem[a->fp] = term_from_ref(s);
    a->mem[a->fp + 1] = term_from_ref(s);
    heap_gc(&vm, a, 0, NULL, 0);
    CHECK_EQ(a->hp, 3);
    CHECK_EQ(a->mem[a->fp], a->mem[a->fp + 1]);
    CHECK_EQ(a->mem[term_ref_off(a->mem[a->fp]) + 2], 0x64636260u);
}

TEST(gc_updates_roots) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    uint32_t junk = heap_alloc(&vm, a, 2, NULL, 0);
    a->mem[junk] = hdr_make(2, K_INT);
    a->mem[junk + 1] = 0x7FFFFFFFu;
    uint32_t o = heap_alloc(&vm, a, 2, NULL, 0);
    a->mem[o] = hdr_make(2, K_INT);
    a->mem[o + 1] = 0x80000000u;
    term roots[2] = { term_from_ref(o), term_from_int(3) };
    heap_gc(&vm, a, 0, roots, 2);
    CHECK_EQ(a->hp, 2);
    CHECK_EQ(roots[0], term_from_ref(0));
    CHECK_EQ(roots[1], term_from_int(3));
    CHECK_EQ(a->mem[1], 0x80000000u);
}

TEST(gc_grows_and_relocates_stack) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 2, -1));
    CHECK(heap_push_frame(&vm, a, 2, 5));
    uint32_t o = heap_alloc(&vm, a, 4, NULL, 0);
    term f[2] = { term_from_int(8), term_from_int(9) };
    put_struct(a, o, 1, f, 2);
    a->mem[a->fp + 1] = term_from_ref(o);
    /* 8 stack + 4 heap used; 30 more does not fit in 24. */
    uint32_t big = heap_alloc(&vm, a, 30, NULL, 0);
    CHECK(big != UINT32_MAX);
    CHECK(a->mem_words >= 12 * 2 + 30);
    CHECK_EQ(a->sp, a->mem_words - 8);
    CHECK_EQ(a->fp, a->sp);
    CHECK_EQ(big, 4);
    term r1 = a->mem[a->fp + 1];
    CHECK_EQ(field(a, r1, 1), term_from_int(9));
    /* Saved caller fp is top-relative: the caller frame is 4 words below top. */
    CHECK_EQ(a->mem[a->fp + 2], term_from_int(4));
    CHECK_EQ(a->mem[a->fp + 3], term_from_int(5));
    /* The caller frame moved intact to the new top. */
    CHECK_EQ(a->mem[a->mem_words - 1], term_from_int(-1));
    CHECK_EQ(a->mem[a->mem_words - 2], term_from_int(0));
    CHECK_EQ(a->mem[a->mem_words - 3], T_NIL);
}

TEST(gc_closure_skips_func_idx) {
    Vm vm;
    Actor *a = setup(&vm, 32);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    uint32_t junk = heap_alloc(&vm, a, 4, NULL, 0);
    term fj[2] = { term_from_int(1), term_from_int(2) };
    put_struct(a, junk, 0, fj, 2);
    uint32_t s = heap_alloc(&vm, a, 3, NULL, 0);
    term fs[1] = { term_from_int(77) };
    put_struct(a, s, 5, fs, 1);
    uint32_t c = heap_alloc(&vm, a, 3, NULL, 0);
    a->mem[c] = hdr_make(3, K_CLOSURE);
    a->mem[c + 1] = 4;                      /* func_idx that looks like a ref */
    a->mem[c + 2] = term_from_ref(s);
    a->mem[a->fp] = term_from_ref(c);
    heap_gc(&vm, a, 0, NULL, 0);
    CHECK_EQ(a->hp, 6);
    uint32_t nc = term_ref_off(a->mem[a->fp]);
    CHECK_EQ(a->mem[nc + 1], 4);
    CHECK(term_is_ref(a->mem[nc + 2]));
    CHECK_EQ(field(a, a->mem[nc + 2], 0), term_from_int(77));
}

TEST(gc_shrinks_sparse_block) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    uint32_t big = heap_alloc(&vm, a, 200, NULL, 0);   /* forces growth */
    CHECK(big != UINT32_MAX);
    a->mem[big] = hdr_make(200, K_STRING);
    a->mem[big + 1] = 0;
    CHECK(a->mem_words >= 200);
    heap_gc(&vm, a, 0, NULL, 0);    /* the string is garbage */
    CHECK_EQ(a->mem_words, 24);
    CHECK_EQ(a->hp, 0);
    CHECK_EQ(a->sp, 21);
    CHECK_EQ(a->mem[23], term_from_int(-1));
}

TEST(heap_oom) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    term roots[1] = { T_NIL };
    int n = 0;
    uint32_t off = 0;
    for (; n < 100; n++) {
        off = heap_alloc(&vm, a, 1000, roots, 1);
        if (off == UINT32_MAX) break;
        a->mem[off] = hdr_make(1000, K_STRUCT);
        a->mem[off + 1] = 0;
        a->mem[off + 2] = roots[0];             /* chain to previous */
        for (uint32_t i = 3; i < 1000; i++) a->mem[off + i] = term_from_int((int32_t)i);
        roots[0] = term_from_ref(off);
    }
    CHECK(off == UINT32_MAX);
    CHECK(n >= 60);
    CHECK(a->mem_words <= HEAP_MAX_WORDS);
    /* The chain survived every GC. */
    int len = 0;
    for (term t = roots[0]; t != T_NIL; t = field(a, t, 0)) {
        CHECK_EQ(field(a, t, 997), term_from_int(999));
        len++;
    }
    CHECK_EQ(len, n);
}

TEST(push_frame_oom) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(!heap_push_frame(&vm, a, 65535, -1));
    CHECK_EQ(a->sp, a->mem_words);
}

TEST(test_pid_generation) {
    Vm vm;
    va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 24);
    uint16_t s = term_pid_slot(a->id);
    term old = a->id;
    CHECK(actor_lookup(&vm, old) == a);
    CHECK_EQ(a->state, A_RUNNABLE);
    CHECK_EQ(vm.live, 1);
    actor_free(&vm, a);
    CHECK_EQ(vm.live, 0);
    CHECK(actor_lookup(&vm, old) == NULL);
    Actor *b = actor_new(&vm, 24);
    CHECK_EQ(term_pid_slot(b->id), s);
    CHECK(b->id != old);
    CHECK(actor_lookup(&vm, old) == NULL);
    CHECK(actor_lookup(&vm, b->id) == b);
    CHECK(actor_lookup(&vm, term_from_int(1)) == NULL);
    actor_free(&vm, b);
}

TEST(actor_table_full) {
    Vm vm;
    va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 2), 0);
    Actor *a = actor_new(&vm, 24);
    Actor *b = actor_new(&vm, 24);
    CHECK(a != NULL && b != NULL);
    CHECK(actor_new(&vm, 24) == NULL);
    actor_free(&vm, a);
    actor_free(&vm, b);
}

TEST(runq_fifo) {
    Vm vm;
    va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    Actor *a = actor_new(&vm, 24), *b = actor_new(&vm, 24);
    CHECK(runq_pop(&vm) == NULL);
    runq_push(&vm, a);
    runq_push(&vm, b);
    CHECK(runq_pop(&vm) == a);
    runq_push(&vm, a);
    CHECK(runq_pop(&vm) == b);
    CHECK(runq_pop(&vm) == a);
    CHECK(runq_pop(&vm) == NULL);
}

TEST(actor_free_releases_memory) {
    Vm vm;
    va_reset();
    CHECK_EQ(vm_init(&vm, NULL, 4), 0);
    size_t before = va_used_bytes();
    Actor *a = actor_new(&vm, 24);
    CHECK(heap_alloc(&vm, a, 200, NULL, 0) != UINT32_MAX);  /* grows block */
    actor_free(&vm, a);
    CHECK_EQ(va_used_bytes(), before);
}

TEST(idle_footprint) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK(heap_push_frame(&vm, a, 6, -1));
    uint32_t env = heap_alloc(&vm, a, 4, NULL, 0);
    CHECK(env != UINT32_MAX);
    a->mem[env] = hdr_make(4, K_CLOSURE);
    a->mem[env + 1] = 0;
    a->mem[env + 2] = T_NIL;
    a->mem[env + 3] = T_NIL;
    size_t bytes = actor_memory_bytes(a);
    CHECK_EQ(bytes, sizeof(Actor) + sizeof(uintptr_t) + 24 * 4);
    CHECK(bytes <= 160 + (sizeof(Actor) - 48));
}

/* Fills a rooted chain of 20-word structs until allocation fails. */
TEST(limit_enforced) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    a->max_words = 256;
    CHECK(heap_push_frame(&vm, a, 1, -1));
    term roots[1] = { T_NIL };
    int n = 0;
    uint32_t off = 0;
    for (; n < 1000; n++) {
        off = heap_alloc(&vm, a, 20, roots, 1);
        if (off == UINT32_MAX) break;
        a->mem[off] = hdr_make(20, K_STRUCT);
        a->mem[off + 1] = 0;
        a->mem[off + 2] = roots[0];
        for (uint32_t i = 3; i < 20; i++) a->mem[off + i] = term_from_int((int32_t)i);
        roots[0] = term_from_ref(off);
    }
    CHECK(off == UINT32_MAX);
    CHECK(n >= 10);                              /* 10 * 20 + 3 stack words fit */
    CHECK((uint32_t)a->mem_words + a->mbox_words <= 256);
    int len = 0;
    for (term t = roots[0]; t != T_NIL; t = field(a, t, 0)) len++;
    CHECK_EQ(len, n);
}

TEST(limit_counts_mailbox) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    a->max_words = 128;
    a->mbox_words = 100;                         /* as if 100 words were queued */
    CHECK(heap_push_frame(&vm, a, 1, -1));
    CHECK_EQ(heap_alloc(&vm, a, 40, NULL, 0), UINT32_MAX);
    CHECK((uint32_t)a->mem_words + a->mbox_words <= 128);
    a->mbox_words = 0;
    CHECK(heap_alloc(&vm, a, 40, NULL, 0) != UINT32_MAX);
}

TEST(actor_max_words_default) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    CHECK_EQ(a->max_words, 0);
    CHECK_EQ(actor_max_words(a), GOGO_DEFAULT_MAX_WORDS);
    a->max_words = 300;
    CHECK_EQ(actor_max_words(a), 300);
}

/* Peak va use during one heap_alloc/heap_gc must stay within
 * (everything outside this actor's block) + old block + new block. */
static void check_peak(size_t before, uint32_t old_words, const Actor *a) {
    size_t outside = before - (size_t)old_words * 4u;
    CHECK(va_peak_bytes() <= outside + (size_t)old_words * 4u + (size_t)a->mem_words * 4u);
}

TEST(gc_peak_growing) {
    for (int certain = 0; certain < 2; certain++) {
        Vm vm;
        Actor *a = setup(&vm, 200);
        CHECK(heap_push_frame(&vm, a, 1, -1));
        uint32_t o = heap_alloc(&vm, a, 100, NULL, 0);
        a->mem[o] = hdr_make(100, K_STRING);
        a->mem[o + 1] = 0;
        a->mem[a->fp] = term_from_ref(o);        /* live: the GC must grow */
        uint32_t need = certain ? 250 : 150;     /* 250 cannot fit in 200 at all */
        size_t before = va_used_bytes();
        uint32_t old_words = a->mem_words;
        va_reset_peak();
        CHECK(heap_alloc(&vm, a, need, NULL, 0) != UINT32_MAX);
        CHECK(a->mem_words > old_words);
        check_peak(before, old_words, a);
        CHECK_EQ(a->mem[term_ref_off(a->mem[a->fp])], hdr_make(100, K_STRING));
    }
}

TEST(gc_peak_shrinking) {
    Vm vm;
    Actor *a = setup(&vm, 400);
    CHECK(heap_push_frame(&vm, a, 1, -1));
    uint32_t o = heap_alloc(&vm, a, 10, NULL, 0);   /* 13 words used of 400 */
    a->mem[o] = hdr_make(10, K_STRING);
    a->mem[o + 1] = 0;
    a->mem[a->fp] = term_from_ref(o);
    size_t before = va_used_bytes();
    va_reset_peak();
    heap_gc(&vm, a, 0, NULL, 0);
    CHECK(a->mem_words < 400);
    check_peak(before, 400, a);
    CHECK_EQ(a->hp, 10);
    CHECK_EQ(a->mem[term_ref_off(a->mem[a->fp])], hdr_make(10, K_STRING));
    CHECK_EQ(a->mem[a->mem_words - 1], term_from_int(-1));
}

/* Queued messages over the limit leave no room at all: the block may not
 * grow, so allocation and frame pushes fail. */
TEST(saturated_limit_blocks_growth) {
    Vm vm;
    Actor *a = setup(&vm, 24);
    a->max_words = 64;
    a->mbox_words = 100;
    CHECK(heap_push_frame(&vm, a, 2, -1));          /* fits the block */
    uint32_t free_words = (uint32_t)(a->sp - a->hp);
    CHECK_EQ(heap_alloc(&vm, a, free_words + 1, NULL, 0), UINT32_MAX);
    CHECK(a->mem_words <= 24);
    CHECK(!heap_push_frame(&vm, a, (uint16_t)free_words, 0));
    CHECK(a->mem_words <= 24);
    a->mbox_words = 0;
    actor_free(&vm, a);
}
