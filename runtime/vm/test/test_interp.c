/* Interpreter-level actor tests on a hand-built image. */
#include "test.h"
#include "test_host.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "term.h"
#include <string.h>

/* Capture diagnostics (see platform_unix.c) so test output stays clean. */
extern void (*platform_unix_err_hook)(const char *, size_t);
static char err_buf[256]; static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}

/* One function "main" (0 params, 2 regs):
 *   BUILTIN r1, r0, 0, RECEIVE
 *   RET r1
 * Layout: 8 header + 3 func + 3 consts + 3 code = 17 words. */
#define IMG_WORDS 20

static void build_receiver(uint32_t *w) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 3; w[6] = 0; w[7] = 0;
    w[8] = 0;                       /* code_off */
    w[9] = 0 | (2u << 16);          /* 0 params, 2 regs */
    w[10] = 0;                      /* name: consts[0] */
    w[11] = hdr_make(3, K_STRING);
    w[12] = 4;
    memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_BUILTIN, 1, 0, 0);
    w[15] = B_RECEIVE;
    w[16] = OP_MAKE(OP_RET, 1, 0, 0);
}

/* An actor about to run main, with an entry frame. */
static Actor *start(Vm *vm, const Image *img, uint16_t words) {
    Actor *a = actor_new(vm, words);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 2, -1));
    a->pc = img->code;
    return a;
}

/* A struct of `words` total words (all fields small ints) in a's heap. */
static term big_struct(Actor *a, uint32_t words) {
    uint32_t off = heap_alloc(NULL, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRUCT);
    a->mem[off + 1] = 0;
    for (uint32_t i = 2; i < words; i++) a->mem[off + i] = term_from_int((int32_t)i);
    return term_from_ref(off);
}

TEST(receive_blocks_then_wakes) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *r = start(&vm, &img, 32);
    Actor *s = actor_new(&vm, 32);

    CHECK_EQ(interp_run(&vm, r), RUN_WAIT);
    CHECK(r->pc == img.code);             /* stays on the BUILTIN */
    r->state = A_WAITING;                 /* as vm_run does */
    CHECK_EQ(msg_send(&vm, s, r->id, term_from_int(7)), 0);
    CHECK_EQ(r->state, A_RUNNABLE);
    CHECK(runq_pop(&vm) == r);            /* woken exactly once */
    CHECK(runq_pop(&vm) == NULL);
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);   /* returned: exited and freed */
    CHECK_EQ(vm.live, 1);
    actor_free(&vm, s);
}

/* A message the receiver's heap cannot take because the machine is out of
 * memory crashes the receiver with oom (its queued message is freed with
 * it), and other actors go on. */
TEST(receive_oom_crashes_only_receiver) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes(&vm);

    /* The receiver's 256-word block is full: 200 live words (r0) plus
     * garbage, so taking the message needs a new block. */
    Actor *r = start(&vm, &img, 256);
    term keep = big_struct(r, 200);
    r->mem[r->fp] = keep;
    big_struct(r, (uint32_t)(r->sp - r->hp));

    Actor *s = actor_new(&vm, 128);
    CHECK(s != NULL);
    CHECK_EQ(msg_send(&vm, s, r->id, big_struct(s, 100)), 0);
    Actor *ok = start(&vm, &img, 32);
    CHECK_EQ(msg_send(&vm, s, ok->id, term_from_int(1)), 0);

    va_set_limit(&vm, va_used_bytes(&vm));                /* no new blocks */
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);       /* crashed: oom, freed */
    platform_unix_err_hook = NULL;
    va_set_limit(&vm, 0);
    CHECK(strstr(err_buf, "crashed: oom in main") != NULL);
    CHECK_EQ(interp_run(&vm, ok), RUN_EXIT);      /* unaffected */

    actor_free(&vm, s);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(&vm), base);              /* r's message was freed */
}

TEST(make_ref_allocates_serials) {
    uint32_t w[IMG_WORDS + 3]; Image img; Vm vm;
    va_reset(&vm);
    build_receiver(w);
    w[15] = B_MAKE_REF;
    w[16] = OP_MAKE(OP_BUILTIN, 0, 0, 0);      /* then block in RECEIVE */
    w[17] = B_RECEIVE;
    w[5] = 4;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    for (uint32_t serial = 1; serial <= 2; serial++) {
        Actor *a = start(&vm, &img, 32);
        CHECK_EQ(interp_run(&vm, a), RUN_WAIT);
        term r = a->mem[a->fp + 1];            /* r1 */
        CHECK(term_is_ref(r));
        const uint32_t *obj = a->mem + term_ref_off(r);
        CHECK_EQ(obj[0], hdr_make(3, K_REF));
        CHECK_EQ(obj[1], serial);
        CHECK_EQ(obj[2], 0);
        actor_free(&vm, a);
    }
}

TEST(unimplemented_builtin_is_badcode) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    build_receiver(w);
    w[15] = B_LINK;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = start(&vm, &img, 32);
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    platform_unix_err_hook = NULL;
    CHECK(strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(vm.live, 0);
}

/* LOADK operands are checked when executed, so
 * a LOADK hidden in an ext word (reached by a jump that lands on it) still
 * refuses a const ref
 * into the middle of an object (here a fake K_STRUCT inside a string's
 * payload): badcode, not a forged object. With the ref at the string
 * itself the same code loads it. */
static const char *run_hidden_loadk(uint32_t k_off, int direct) {
    static uint32_t w[8 + 3 + 8 + 9 + 1 + 2]; static Image img; static Vm vm;
    va_reset(&vm);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 8; w[5] = 9; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (2u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = hdr_make(5, K_STRING); w[15] = 12;
    w[16] = hdr_make(3, K_STRUCT); w[17] = 0; w[18] = term_from_ref(0x100000);
    uint32_t *c = w + 19;
    c[0] = OP_MAKE(OP_JMP, 0, 0, 0); c[1] = 3;              /* into the next ext */
    c[2] = OP_MAKE(OP_LOADI32, 0, 0, 0); c[3] = OP_MAKE(OP_LOADK, 0, 0, 0);
    if (direct) c[0] = c[1] = c[2] = OP_MAKE(OP_NOP, 0, 0, 0);   /* a plain LOADK at 3 */
    c[4] = term_from_const(k_off);                         /* an unknown op linearly */
    c[5] = OP_MAKE(OP_NOP, 0, 0, 0);
    c[6] = OP_MAKE(OP_BUILTIN, 0, 0, 0); c[7] = B_RECEIVE;
    c[8] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = start(&vm, &img, 32);
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(&vm, a);
    platform_unix_err_hook = NULL;
    if (rc != RUN_EXIT) actor_free(&vm, a);
    return err_buf;
}

TEST(hidden_loadk_interior_const_is_badcode) {
    CHECK(strstr(run_hidden_loadk(5, 0), "crashed: badcode") != NULL);
    CHECK(strstr(run_hidden_loadk(3, 0), "crashed") == NULL);
}

/* VM-1 (round 1 repro): an image whose plain LOADK names the middle of an
 * object loads (LOADK operands are checked when executed, not at load) and
 * crashes the actor with badcode when that LOADK runs; ones into a
 * string's header word + 1 or past the const area likewise. */
TEST(loadk_interior_const_is_badcode) {
    CHECK(strstr(run_hidden_loadk(5, 1), "crashed: badcode") != NULL);
    CHECK(strstr(run_hidden_loadk(4, 1), "crashed: badcode") != NULL);
    CHECK(strstr(run_hidden_loadk(8, 1), "crashed: badcode") != NULL);
    CHECK(strstr(run_hidden_loadk(3, 1), "crashed") == NULL);
}

/* One function "main" (0 params, 3 regs) with two K_INT64 consts x, y:
 *   LOADK r0, x; LOADK r1, y; <op> r2, r0, r1 (c ignored by NEG);
 *   RECEIVE r0 (blocks, so the result can be inspected)
 * x_small != 0 loads x as the small int x_small instead. */
#define I64_WORDS 30

static void build_int64_op(uint32_t *w, uint32_t op, int64_t x, int64_t y, int32_t x_small) {
    memset(w, 0, I64_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 9; w[5] = 7; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (3u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = hdr_make(3, K_INT64); w[15] = (uint32_t)(uint64_t)x; w[16] = (uint32_t)((uint64_t)x >> 32);
    w[17] = hdr_make(3, K_INT64); w[18] = (uint32_t)(uint64_t)y; w[19] = (uint32_t)((uint64_t)y >> 32);
    w[20] = OP_MAKE(OP_LOADK, 0, 0, 0); w[21] = x_small ? term_from_int(x_small) : term_from_const(3);
    w[22] = OP_MAKE(OP_LOADK, 1, 0, 0); w[23] = term_from_const(6);
    w[24] = OP_MAKE(op, 2, 0, (op == OP_NEG || op == OP_NEG64 || op == OP_BOX64) ? 0 : 1);
    w[25] = OP_MAKE(OP_BUILTIN, 0, 0, 0);
    w[26] = B_RECEIVE;
}

/* Runs the op; returns the crash text ("" if none) and the result in *out. */
static const char *run_int64_op(uint32_t op, int64_t x, int64_t y, int32_t x_small, term *out, int64_t *val) {
    static uint32_t w[I64_WORDS]; static Image img; static Vm vm;
    va_reset(&vm);
    build_int64_op(w, op, x, y, x_small);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = actor_new(&vm, 32);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&vm, a, 3, -1));
    a->pc = img.code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(&vm, a);
    platform_unix_err_hook = NULL;
    *out = T_NIL;
    *val = 0;
    if (rc == RUN_EXIT) return err_buf;         /* crashed; a is gone */
    CHECK_EQ(rc, RUN_WAIT);
    *out = a->mem[a->fp + 2];
    if (term_is_int(*out)) *val = term_to_int(*out);
    if (term_is_ref(*out)) {
        const uint32_t *o = a->mem + term_ref_off(*out);
        if (o[0] == hdr_make(3, K_INT64)) *val = (int64_t)((uint64_t)o[1] | ((uint64_t)o[2] << 32));
    }
    actor_free(&vm, a);
    return err_buf;
}

TEST(int64_edge_cases) {
    term t; int64_t v;
    const int64_t min = INT64_MIN, max = INT64_MAX;
    CHECK_EQ(run_int64_op(OP_DIV64, min, -1, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_REM64, min, -1, 0, &t, &v)[0], 0); CHECK(v == 0 && term_is_int(t));
    CHECK_EQ(run_int64_op(OP_NEG64, min, 0, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_ADD64, max, 1, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_SUB64, min, 1, 0, &t, &v)[0], 0); CHECK(v == max);
    CHECK_EQ(run_int64_op(OP_MUL64, max, max, 0, &t, &v)[0], 0); CHECK(v == 1 && term_is_int(t));
    CHECK_EQ(run_int64_op(OP_REM64, -7, 2, 0, &t, &v)[0], 0); CHECK(v == -1);
    CHECK_EQ(run_int64_op(OP_LT64, -1, 0, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_LE64, 1LL << 40, 1, 0, &t, &v)[0], 0); CHECK(t == T_FALSE);
    CHECK_EQ(run_int64_op(OP_EQ64, 1LL << 40, 1LL << 40, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_NE64, 1LL << 40, (1LL << 40) + 1, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK(strstr(run_int64_op(OP_DIV64, 5, 0, 0, &t, &v), "crashed: divzero") != NULL);
    CHECK(strstr(run_int64_op(OP_REM64, 5, 0, 0, &t, &v), "crashed: divzero") != NULL);
    /* The generic ops are int-only: an int64 box operand is malformed code. */
    CHECK(strstr(run_int64_op(OP_ADD, 1, 5, 0, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_ADD, 0, 5, 3, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_NEG, 1, 0, 0, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_LT, 0, 5, 3, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_EQ, 0, 5, 3, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_NE, 5, 5, 0, &t, &v), "crashed: badcode") != NULL);
}

/* The *64 ops take either representation and box only results outside 31
 * bits. */
TEST(int64_immediates) {
    term t; int64_t v;
    /* immediate 3 + box 5 = immediate 8 */
    CHECK_EQ(run_int64_op(OP_ADD64, 0, 5, 3, &t, &v)[0], 0); CHECK(term_is_int(t) && v == 8);
    /* box 2^40 - box (2^40 - 5) = immediate 5 */
    CHECK_EQ(run_int64_op(OP_SUB64, 1LL << 40, (1LL << 40) - 5, 0, &t, &v)[0], 0);
    CHECK(term_is_int(t) && v == 5);
    /* immediate TERM_INT_MAX + box 1 crosses into a box, and the min side */
    CHECK_EQ(run_int64_op(OP_ADD64, 0, 1, TERM_INT_MAX, &t, &v)[0], 0);
    CHECK(term_is_ref(t) && v == (int64_t)TERM_INT_MAX + 1);
    CHECK_EQ(run_int64_op(OP_SUB64, 0, 1, TERM_INT_MIN, &t, &v)[0], 0);
    CHECK(term_is_ref(t) && v == (int64_t)TERM_INT_MIN - 1);
    CHECK_EQ(run_int64_op(OP_SUB64, 0, 0, TERM_INT_MIN, &t, &v)[0], 0);
    CHECK(term_is_int(t) && v == TERM_INT_MIN);
    /* NEG64 of the immediate TERM_INT_MIN must box (2^30 does not fit). */
    CHECK_EQ(run_int64_op(OP_NEG64, 0, 0, TERM_INT_MIN, &t, &v)[0], 0);
    CHECK(term_is_ref(t) && v == -(int64_t)TERM_INT_MIN);
    /* Mixed representations compare by value. */
    CHECK_EQ(run_int64_op(OP_EQ64, 0, 7, 7, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_NE64, 0, 7, 7, &t, &v)[0], 0); CHECK(t == T_FALSE);
    CHECK_EQ(run_int64_op(OP_LT64, 0, 7, 6, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_LE64, 0, 7, 8, &t, &v)[0], 0); CHECK(t == T_FALSE);
    CHECK_EQ(run_int64_op(OP_DIV64, 0, 2, 9, &t, &v)[0], 0); CHECK(term_is_int(t) && v == 4);
    CHECK_EQ(run_int64_op(OP_MUL64, 0, 1LL << 40, 1, &t, &v)[0], 0); CHECK(term_is_ref(t) && v == 1LL << 40);
    /* BOX64 r2, r0: an immediate becomes a heap K_INT64; a box stays. */
    CHECK_EQ(run_int64_op(OP_BOX64, 0, 0, -9, &t, &v)[0], 0); CHECK(term_is_ref(t) && v == -9);
    CHECK_EQ(run_int64_op(OP_BOX64, 1LL << 40, 0, 0, &t, &v)[0], 0);
    CHECK(t == term_from_const(3) && v == 0);
    /* Anything else is malformed. */
    static uint32_t w[I64_WORDS]; Image img; Vm vm;
    const uint32_t ops[] = { OP_ADD64, OP_LT64, OP_EQ64, OP_NEG64, OP_BOX64 };
    for (int kind = 0; kind < 2; kind++) {
        for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
            va_reset(&vm);
            build_int64_op(w, ops[i], 0, 1, 0);
            if (kind == 0) w[21] = T_TRUE;               /* a bool */
            else {                                       /* a K_INT box */
                w[20] = OP_MAKE(OP_LOADI32, 0, 0, 0);
                w[21] = (uint32_t)INT32_MIN;
            }
            CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
            CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
            Actor *a = actor_new(&vm, 32);
            CHECK(heap_push_frame(&vm, a, 3, -1));
            a->pc = img.code;
            err_len = 0; err_buf[0] = 0;
            platform_unix_err_hook = capture_err;
            CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
            platform_unix_err_hook = NULL;
            CHECK(strstr(err_buf, "crashed: badcode") != NULL);
            CHECK_EQ(vm.live, 0);
        }
    }
}

/* Runs CONV r2, r<src>, c after LOADK r0, <r0> (patched in by the caller
 * through w[20..21]) and LOADK r1, <y box>; returns the result register. */
static term run_conv(uint32_t *w, Image *img, Vm *vm, Actor **ap, uint32_t src, uint32_t c) {
    w[24] = OP_MAKE(OP_CONV, 2, src, c);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(vm, img, 4), 0);
    Actor *a = actor_new(vm, 32);
    CHECK(heap_push_frame(vm, a, 3, -1));
    a->pc = img->code;
    *ap = a;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(vm, a);
    platform_unix_err_hook = NULL;
    if (rc == RUN_EXIT) { *ap = NULL; return T_NIL; }
    CHECK_EQ(rc, RUN_WAIT);
    return a->mem[a->fp + 2];
}

TEST(int64_conv) {
    static uint32_t w[I64_WORDS]; Image img; Vm vm; Actor *a;
    term r;
    /* int -> int64: a small int stays the same immediate. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, -3);
    r = run_conv(w, &img, &vm, &a, 0, 1);
    CHECK(a && r == term_from_int(-3));
    actor_free(&vm, a);
    /* int -> int64: a K_INT box becomes a K_INT64 box. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[20] = OP_MAKE(OP_LOADI32, 0, 0, 0); w[21] = (uint32_t)INT32_MIN;
    r = run_conv(w, &img, &vm, &a, 0, 1);
    CHECK(a && term_is_ref(r));
    CHECK_EQ(a->mem[term_ref_off(r)], hdr_make(3, K_INT64));
    CHECK_EQ(a->mem[term_ref_off(r) + 1], 0x80000000u);
    CHECK_EQ(a->mem[term_ref_off(r) + 2], 0xFFFFFFFFu);
    actor_free(&vm, a);
    /* int64 -> int: a box truncates to its low 32 bits. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, -0x180000000LL, 0);
    r = run_conv(w, &img, &vm, &a, 1, 0);
    CHECK(a && term_is_ref(r));
    CHECK_EQ(a->mem[term_ref_off(r)], hdr_make(2, K_INT));
    CHECK_EQ((int32_t)a->mem[term_ref_off(r) + 1], INT32_MIN);
    actor_free(&vm, a);
    /* int64 -> int: a box of a small value becomes an immediate. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, (1LL << 32) + 5, 0);
    r = run_conv(w, &img, &vm, &a, 1, 0);
    CHECK(a && r == term_from_int(5));
    actor_free(&vm, a);
    /* int64 -> int: an immediate stays. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, -7);
    r = run_conv(w, &img, &vm, &a, 0, 0);
    CHECK(a && r == term_from_int(-7));
    actor_free(&vm, a);
    /* Wrong representations are badcode: a bool, a K_INT box where an
     * int64 is expected, a K_INT64 box where an int is expected. */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[21] = T_TRUE;
    r = run_conv(w, &img, &vm, &a, 0, 1);
    CHECK(!a && strstr(err_buf, "crashed: badcode") != NULL);
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[20] = OP_MAKE(OP_LOADI32, 0, 0, 0); w[21] = (uint32_t)INT32_MIN;
    r = run_conv(w, &img, &vm, &a, 0, 0);
    CHECK(!a && strstr(err_buf, "crashed: badcode") != NULL);
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 1LL << 40, 0);
    r = run_conv(w, &img, &vm, &a, 1, 1);
    CHECK(!a && strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(vm.live, 0);
}

/* One function "main" (0 params, 5 regs) calling NOW 10000 times:
 *   LOADK r0, 10000; LOADK r2, 1; LOADK r4, 0
 *   6: BUILTIN r1, r0, 0, NOW; SUB r0, r0, r2; LT r3, r4, r0; JMPT r3, @6
 *   BUILTIN r0, r0, 0, RECEIVE */
#define NOW_WORDS 31

TEST(now_no_alloc) {
    static uint32_t w[NOW_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 14; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (5u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_LOADK, 0, 0, 0); w[15] = term_from_int(10000);
    w[16] = OP_MAKE(OP_LOADK, 2, 0, 0); w[17] = term_from_int(1);
    w[18] = OP_MAKE(OP_LOADK, 4, 0, 0); w[19] = term_from_int(0);
    w[20] = OP_MAKE(OP_BUILTIN, 1, 0, 0); w[21] = B_NOW;
    w[22] = OP_MAKE(OP_SUB, 0, 0, 2);
    w[23] = OP_MAKE(OP_LT, 3, 4, 0);
    w[24] = OP_MAKE(OP_JMPT, 3, 0, 0); w[25] = 6;
    w[26] = OP_MAKE(OP_BUILTIN, 0, 0, 0); w[27] = B_RECEIVE;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = actor_new(&vm, 32);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&vm, a, 5, -1));
    a->pc = img.code;
    const uint32_t *mem = a->mem;
    uint16_t hp = a->hp, words = a->mem_words;
    size_t used = va_used_bytes(&vm);
    va_reset_peak(&vm);                      /* a GC would allocate a new block */
    int rc, slices = 0;
    while ((rc = interp_run(&vm, a)) == RUN_YIELD) slices++;
    CHECK_EQ(rc, RUN_WAIT);
    CHECK_EQ(slices, 9);                  /* 9999 backward jumps, 1000 per slice */
    CHECK_EQ(term_to_int(a->mem[a->fp]), 0);
    CHECK(term_is_int(a->mem[a->fp + 1]));
    /* No allocation and no GC: same block, same size, same heap top. */
    CHECK(a->mem == mem);
    CHECK_EQ(a->mem_words, words);
    CHECK_EQ(a->hp, hp);
    CHECK_EQ(va_used_bytes(&vm), used);
    CHECK_EQ(va_peak_bytes(&vm), used);
    actor_free(&vm, a);
    CHECK_EQ(vm.live, 0);
}

/* One function "main" (0 params, n_regs regs) running `code` (n words);
 * the code should end by blocking in RECEIVE so registers can be read.
 * Layout: 8 header + 3 func + 3 consts + code. */
#define CODE_MAX 40
static uint32_t code_img[14 + CODE_MAX + 1 + 2];

static const char *run_code(const uint32_t *code, uint32_t n, uint16_t n_regs,
                            Vm *vm, Image *img, Actor **out) {
    uint32_t *w = code_img;
    CHECK(n <= CODE_MAX);
    memset(w, 0, sizeof code_img);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = n; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | ((uint32_t)n_regs << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    memcpy(&w[14], code, n * 4u);
    va_reset(vm);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(vm, img, 4), 0);
    Actor *a = actor_new(vm, 64);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, n_regs, -1));
    a->pc = img->code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(vm, a);
    platform_unix_err_hook = NULL;
    *out = rc == RUN_EXIT ? NULL : a;
    return err_buf;
}

TEST(clear_sets_nil) {
    Vm vm; Image img; Actor *a;
    const uint32_t code[] = {
        OP_MAKE(OP_BUILTIN, 0, 0, 0), B_MAKE_REF,
        OP_MAKE(OP_BUILTIN, 1, 0, 0), B_MAKE_REF,
        OP_MAKE(OP_BUILTIN, 2, 0, 0), B_MAKE_REF,
        OP_MAKE(OP_CLEAR, 0, 2, 0),
        OP_MAKE(OP_CLEAR, 3, 0, 0),             /* empty range: nothing */
        OP_MAKE(OP_BUILTIN, 3, 0, 0), B_RECEIVE,
    };
    CHECK_EQ(run_code(code, 10, 4, &vm, &img, &a)[0], 0);
    CHECK(a != NULL);
    CHECK_EQ(a->mem[a->fp + 0], T_NIL);
    CHECK_EQ(a->mem[a->fp + 1], T_NIL);
    CHECK(term_is_ref(a->mem[a->fp + 2]));      /* outside the range: kept */
    actor_free(&vm, a);
}

TEST(clear_out_of_range_is_badcode) {
    Vm vm; Image img; Actor *a;
    const uint32_t code[] = {
        OP_MAKE(OP_CLEAR, 2, 3, 0),             /* r2..r4 of 4 registers */
        OP_MAKE(OP_BUILTIN, 0, 0, 0), B_RECEIVE,
    };
    CHECK(strstr(run_code(code, 3, 4, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
}

/* Zero PID and zero Ref: typed, equal to themselves only, harmless as
 * builtin arguments, printed as <pid 0> / <ref 0>. */
TEST(zero_pid_and_ref) {
    Vm vm; Image img; Actor *a;
    const uint32_t code[] = {
        OP_MAKE(OP_LOADK, 0, 0, 0), T_ZEROPID,
        OP_MAKE(OP_LOADK, 1, 0, 0), T_ZEROREF,
        OP_MAKE(OP_TYPEIS, 2, 0, 0), TY_PID,
        OP_MAKE(OP_TYPEIS, 3, 1, 0), TY_REF,
        OP_MAKE(OP_BUILTIN, 4, 1, 1), B_CANCEL_TIMER,
        OP_MAKE(OP_BUILTIN, 5, 1, 1), B_DEMONITOR,
        OP_MAKE(OP_EQ, 6, 1, 1),
        OP_MAKE(OP_BUILTIN, 7, 0, 2), B_SEND,   /* dropped */
        OP_MAKE(OP_BUILTIN, 8, 0, 0), B_MAKE_REF,
        OP_MAKE(OP_EQ, 9, 1, 8),
        OP_MAKE(OP_BUILTIN, 7, 0, 0), B_SELF,
        OP_MAKE(OP_EQ, 10, 0, 7),
        OP_MAKE(OP_TYPEIS, 11, 1, 0), TY_PID,
        OP_MAKE(OP_BUILTIN, 7, 0, 0), B_RECEIVE,
    };
    CHECK_EQ(run_code(code, 25, 12, &vm, &img, &a)[0], 0);
    CHECK(a != NULL);
    term *R = a->mem + a->fp;
    CHECK(a->id != T_ZEROPID);
    CHECK(term_is_pid(T_ZEROPID));
    CHECK(actor_lookup(&vm, T_ZEROPID) == NULL);
    CHECK_EQ(R[2], T_TRUE);
    CHECK_EQ(R[3], T_TRUE);
    CHECK_EQ(R[4], T_FALSE);
    CHECK_EQ(R[5], T_NIL);
    CHECK_EQ(R[6], T_TRUE);
    CHECK_EQ(R[9], T_FALSE);
    CHECK_EQ(R[10], T_FALSE);
    CHECK_EQ(R[11], T_FALSE);
    actor_free(&vm, a);
}

/* Slot 0 never carries generation 0, so no actor ever has the zero PID,
 * also after the 12-bit generation wraps. */
TEST(no_actor_has_zero_pid) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    for (uint32_t i = 0; i < 4097u + 3u; i++) {
        Actor *a = actor_new(&vm, 32);
        CHECK(a != NULL);
        CHECK_EQ(term_pid_slot(a->id), 0);
        CHECK(a->id != T_ZEROPID);
        actor_free(&vm, a);
    }
}

/* Two functions (multi-result return):
 *   main (0 params, 4 regs): LOADK r0, 7; CALL r<dst>, r0, 1, callee;
 *                            BUILTIN r3, r0, 0, RECEIVE (blocks)
 *   callee (1 param, 3 regs): BUILTIN r1, r0, 0, MAKE_REF; MOVE r2, r0;
 *                             RETN r1, <n>
 * Layout: 8 header + 6 func + 3 consts + 10 code = 27 words. */
#define RETN_WORDS 30
static uint32_t retn_img[RETN_WORDS];

static const char *run_retn(uint32_t dst, uint32_t n, Vm *vm, Image *img, Actor **out) {
    uint32_t *w = retn_img;
    memset(w, 0, sizeof retn_img);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 2; w[3] = 0;
    w[4] = 3; w[5] = 10; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (4u << 16); w[10] = 0;
    w[11] = 6; w[12] = 1 | (3u << 16); w[13] = 0;
    w[14] = hdr_make(3, K_STRING); w[15] = 4; memcpy(&w[16], "main", 4);
    w[17] = OP_MAKE(OP_LOADK, 0, 0, 0); w[18] = term_from_int(7);
    w[19] = OP_MAKE(OP_CALL, dst, 0, 1); w[20] = 1;
    w[21] = OP_MAKE(OP_BUILTIN, 3, 0, 0); w[22] = B_RECEIVE;
    w[23] = OP_MAKE(OP_BUILTIN, 1, 0, 0); w[24] = B_MAKE_REF;
    w[25] = OP_MAKE(OP_MOVE, 2, 0, 0);
    w[26] = OP_MAKE(OP_RETN, 1, n, 0);
    va_reset(vm);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(vm, img, 4), 0);
    Actor *a = actor_new(vm, 64);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 4, -1));
    a->pc = img->code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(vm, a);
    platform_unix_err_hook = NULL;
    *out = rc == RUN_EXIT ? NULL : a;
    return err_buf;
}

/* RETN a b writes the callee's ra..ra+b-1 into the caller's CALL
 * destination dst..dst+b-1 and pops the callee frame. */
TEST(retn_into_caller) {
    Vm vm; Image img; Actor *a;
    CHECK_EQ(run_retn(1, 2, &vm, &img, &a)[0], 0);
    CHECK(a != NULL);
    term *R = a->mem + a->fp;
    CHECK_EQ(a->fp + 4u + 2u, (uint32_t)a->mem_words);   /* back in the entry frame */
    CHECK_EQ(R[0], term_from_int(7));
    CHECK(term_is_ref(R[1]));
    CHECK_EQ(a->mem[term_ref_off(R[1])], hdr_make(3, K_REF));
    CHECK_EQ(R[2], term_from_int(7));
    actor_free(&vm, a);
    CHECK_EQ(vm.live, 0);
}

TEST(retn_past_caller_regs_is_badcode) {
    Vm vm; Image img; Actor *a;
    /* dst r3 + 2 results > 4 caller registers */
    CHECK(strstr(run_retn(3, 2, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    /* source range r1..r3 past the callee's 3 registers */
    CHECK(strstr(run_retn(0, 3, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    CHECK_EQ(vm.live, 0);
}

/* CONV c=2: to byte, the low 8 bits of an int or int64 in any
 * representation; always an immediate. */
TEST(conv_to_byte) {
    static uint32_t w[I64_WORDS]; Image img; Vm vm; Actor *a;
    term r;
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 300);
    r = run_conv(w, &img, &vm, &a, 0, 2);
    CHECK(a && r == term_from_int(44));
    actor_free(&vm, a);
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, -6);
    r = run_conv(w, &img, &vm, &a, 0, 2);
    CHECK(a && r == term_from_int(250));
    actor_free(&vm, a);
    /* a K_INT box */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[20] = OP_MAKE(OP_LOADI32, 0, 0, 0); w[21] = (uint32_t)INT32_MIN + 0x1FFu;
    r = run_conv(w, &img, &vm, &a, 0, 2);
    CHECK(a && r == term_from_int(0xFF));
    actor_free(&vm, a);
    /* a K_INT64 box */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, (1LL << 40) + 0x1234, 0);
    r = run_conv(w, &img, &vm, &a, 1, 2);
    CHECK(a && r == term_from_int(0x34));
    actor_free(&vm, a);
    /* not an int; an unknown c */
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[21] = T_TRUE;
    r = run_conv(w, &img, &vm, &a, 0, 2);
    CHECK(!a && strstr(err_buf, "crashed: badcode") != NULL);
    va_reset(&vm);
    build_int64_op(w, OP_CONV, 0, 0, 5);
    r = run_conv(w, &img, &vm, &a, 0, 3);
    CHECK(!a && strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(vm.live, 0);
}

/* Two functions (calling a closure):
 *   main (0 params, 5 regs): LOADK r1, 7; MKCLOSURE r2, r1, 1, callee
 *                            (or LOADK r2, nil); LOADK r3, 5;
 *                            CALLV r0, r2, <nargs>; BUILTIN r4, r0, 0, RECEIVE
 *   callee (2 params, 3 regs): ADD r2, r0, r1; RET r2
 * The capture 7 is the callee's r0, the argument 5 its r1.
 * Layout: 8 header + 6 func + 3 consts + 11 code = 28 words. */
#define CALLV_WORDS 31
static uint32_t callv_img[CALLV_WORDS];

static const char *run_callv(bool nil, uint32_t nargs, Vm *vm, Image *img, Actor **out) {
    uint32_t *w = callv_img;
    memset(w, 0, sizeof callv_img);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 2; w[3] = 0;
    w[4] = 3; w[5] = 11; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (5u << 16); w[10] = 0;
    w[11] = 9; w[12] = 2 | (3u << 16); w[13] = 0;
    w[14] = hdr_make(3, K_STRING); w[15] = 4; memcpy(&w[16], "main", 4);
    w[17] = OP_MAKE(OP_LOADK, 1, 0, 0); w[18] = term_from_int(7);
    if (nil) { w[19] = OP_MAKE(OP_LOADK, 2, 0, 0); w[20] = T_NIL; }
    else     { w[19] = OP_MAKE(OP_MKCLOSURE, 2, 1, 1); w[20] = 1; }
    w[21] = OP_MAKE(OP_LOADK, 3, 0, 0); w[22] = term_from_int(5);
    w[23] = OP_MAKE(OP_CALLV, 0, 2, nargs);
    w[24] = OP_MAKE(OP_BUILTIN, 4, 0, 0); w[25] = B_RECEIVE;
    w[26] = OP_MAKE(OP_ADD, 2, 0, 1);
    w[27] = OP_MAKE(OP_RET, 2, 0, 0);
    va_reset(vm);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(vm, img, 4), 0);
    Actor *a = actor_new(vm, 64);
    CHECK(a != NULL);
    CHECK(heap_push_frame(vm, a, 5, -1));
    a->pc = img->code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(vm, a);
    platform_unix_err_hook = NULL;
    *out = rc == RUN_EXIT ? NULL : a;
    return err_buf;
}

/* CALLV passes the captures, then the args; the result lands in ra and
 * execution resumes right after the (ext-less) CALLV. */
TEST(callv_captures_then_args) {
    Vm vm; Image img; Actor *a;
    CHECK_EQ(run_callv(false, 1, &vm, &img, &a)[0], 0);
    CHECK(a != NULL);
    term *R = a->mem + a->fp;
    CHECK_EQ(a->fp + 5u + 2u, (uint32_t)a->mem_words);   /* back in the entry frame */
    CHECK_EQ(R[0], term_from_int(12));
    CHECK(a->pc == img.code + 7);                        /* blocked on RECEIVE */
    actor_free(&vm, a);
    CHECK_EQ(vm.live, 0);
}

TEST(callv_nil_or_wrong_arity_is_badcode) {
    Vm vm; Image img; Actor *a;
    CHECK(strstr(run_callv(true, 1, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    /* 1 capture + 0 args != 2 params */
    CHECK(strstr(run_callv(false, 0, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    /* args past the caller's registers: r3..r4 + r5 */
    CHECK(strstr(run_callv(false, 3, &vm, &img, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    CHECK_EQ(vm.live, 0);
}

/* ---- bitwise ops ---- */

/* Operand kinds for run_bit: an int32 loaded with LOADI32 (boxed as K_INT
 * outside 31 bits), a K_INT64 const, or a string (not an integer). */
enum { BI32 = 0, B64 = 1, BSTR = 2 };
typedef struct { int64_t v; int kind; } BOp;

/* main (0 params, 3 regs): load x, load y, <op> r2, r0, r1; RECEIVE. Same
 * layout as build_int64_op. Returns the crash text ("" if none); the result
 * of a K_INT/K_INT64 box or immediate lands in *val, its term in *out. */
static const char *run_bit(uint32_t op, BOp x, BOp y, term *out, int64_t *val) {
    static uint32_t w[I64_WORDS]; static Image img; static Vm vm;
    va_reset(&vm);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 9; w[5] = 7; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (3u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = hdr_make(3, K_INT64); w[15] = (uint32_t)(uint64_t)x.v; w[16] = (uint32_t)((uint64_t)x.v >> 32);
    w[17] = hdr_make(3, K_INT64); w[18] = (uint32_t)(uint64_t)y.v; w[19] = (uint32_t)((uint64_t)y.v >> 32);
    const BOp *o[2] = { &x, &y };
    for (int i = 0; i < 2; i++) {
        if (o[i]->kind == BI32) {
            w[20 + 2 * i] = OP_MAKE(OP_LOADI32, (uint32_t)i, 0, 0); w[21 + 2 * i] = (uint32_t)(int32_t)o[i]->v;
        } else {
            w[20 + 2 * i] = OP_MAKE(OP_LOADK, (uint32_t)i, 0, 0);
            w[21 + 2 * i] = term_from_const(o[i]->kind == BSTR ? 0 : (i ? 6 : 3));
        }
    }
    w[24] = OP_MAKE(op, 2, 0, 1);
    w[25] = OP_MAKE(OP_BUILTIN, 0, 0, 0);
    w[26] = B_RECEIVE;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 4), 0);
    Actor *a = actor_new(&vm, 32);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&vm, a, 3, -1));
    a->pc = img.code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(&vm, a);
    platform_unix_err_hook = NULL;
    *out = T_NIL;
    *val = 0;
    if (rc == RUN_EXIT) return err_buf;
    CHECK_EQ(rc, RUN_WAIT);
    *out = a->mem[a->fp + 2];
    if (term_is_int(*out)) *val = term_to_int(*out);
    if (term_is_ref(*out)) {
        const uint32_t *p = a->mem + term_ref_off(*out);
        if (p[0] == hdr_make(2, K_INT)) *val = (int32_t)p[1];
        if (p[0] == hdr_make(3, K_INT64)) *val = (int64_t)((uint64_t)p[1] | ((uint64_t)p[2] << 32));
    }
    actor_free(&vm, a);
    return err_buf;
}

#define I(n)  ((BOp){ (n), BI32 })
#define L(n)  ((BOp){ (n), B64 })
#define BITOK(op, x, y, want) do { \
        term t_; int64_t v_; \
        CHECK_EQ(run_bit((op), (x), (y), &t_, &v_)[0], 0); \
        CHECK(v_ == (int64_t)(want)); } while (0)

TEST(bitops_int) {
    term t; int64_t v;
    BITOK(OP_AND, I(0x0F0F), I(0x00FF), 0x000F);
    BITOK(OP_OR, I(0x0F0F), I(0x00FF), 0x0FFF);
    BITOK(OP_XOR, I(0x0F0F), I(0x00FF), 0x0FF0);
    BITOK(OP_ANDNOT, I(0x0F0F), I(0x00FF), 0x0F00);
    BITOK(OP_AND, I(-1), I(INT32_MIN), INT32_MIN);          /* boxed operand and result */
    BITOK(OP_XOR, I(INT32_MAX), I(-1), INT32_MIN);
    BITOK(OP_SHL, I(1), I(30), 1073741824);
    CHECK_EQ(run_bit(OP_SHL, I(1), I(30), &t, &v)[0], 0); CHECK(term_is_ref(t));
    BITOK(OP_SHL, I(1), I(31), INT32_MIN);
    BITOK(OP_SHL, I(1), I(32), 0);
    BITOK(OP_SHL, I(3), I(0), 3);
    BITOK(OP_SHL, I(1), L(1LL << 40), 0);                   /* count as a K_INT64 box */
    BITOK(OP_SHR, I(-8), I(1), -4);
    BITOK(OP_SHR, I(-1), I(40), -1);
    BITOK(OP_SHR, I(5), I(32), 0);
    BITOK(OP_SHR, I(INT32_MIN), I(31), -1);
    BITOK(OP_SHR, I(-5), I(0), -5);
    BITOK(OP_SHR, I(-5), L(1LL << 40), -1);
    BITOK(OP_SHR, I(5), L(1LL << 40), 0);
    CHECK(strstr(run_bit(OP_SHL, I(1), I(-1), &t, &v), "crashed: badshift") != NULL);
    CHECK(strstr(run_bit(OP_SHR, I(1), L(-(1LL << 40)), &t, &v), "crashed: badshift") != NULL);
    CHECK(strstr(run_bit(OP_AND, (BOp){0, BSTR}, I(1), &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_bit(OP_SHL, I(1), (BOp){0, BSTR}, &t, &v), "crashed: badcode") != NULL);
    /* int ops reject an int64 box operand, like OP_ADD. */
    CHECK(strstr(run_bit(OP_OR, L(5), I(1), &t, &v), "crashed: badcode") != NULL);
}

TEST(bitops_int64) {
    term t; int64_t v;
    const int64_t min = INT64_MIN;
    BITOK(OP_AND64, L(0xFF00FF00FFLL), L(0x0F0F0F0F0FLL), 0x0F000F000FLL);
    BITOK(OP_OR64, L(1LL << 40), I(5), (1LL << 40) | 5);    /* mixed immediate / box */
    BITOK(OP_XOR64, I(5), L(1LL << 40), (1LL << 40) ^ 5);
    BITOK(OP_ANDNOT64, L((1LL << 40) | 7), I(2), (1LL << 40) | 5);
    BITOK(OP_XOR64, L(min), I(-1), INT64_MAX);
    CHECK_EQ(run_bit(OP_AND64, L(0xFF00000000LL | 3), I(3), &t, &v)[0], 0);
    CHECK(term_is_int(t) && v == 3);                        /* result fits: immediate */
    BITOK(OP_SHL64, I(1), I(62), 1LL << 62);
    CHECK_EQ(run_bit(OP_SHL64, I(1), I(62), &t, &v)[0], 0); CHECK(term_is_ref(t));
    BITOK(OP_SHL64, I(1), I(63), min);
    BITOK(OP_SHL64, I(1), I(64), 0);
    BITOK(OP_SHL64, I(1), L(1LL << 40), 0);
    BITOK(OP_SHL64, L(1LL << 40), I(0), 1LL << 40);
    BITOK(OP_SHL64, I(3), I(31), 3LL << 31);
    BITOK(OP_SHR64, L(min), I(63), -1);
    BITOK(OP_SHR64, L(min), I(64), -1);
    BITOK(OP_SHR64, L(1LL << 62), I(64), 0);
    BITOK(OP_SHR64, L(1LL << 62), I(100), 0);
    BITOK(OP_SHR64, L(1LL << 40), I(10), 1LL << 30);
    BITOK(OP_SHR64, I(-8), L(1), -4);
    BITOK(OP_SHR64, I(-8), L(1LL << 40), -1);
    CHECK(strstr(run_bit(OP_SHL64, I(1), I(-1), &t, &v), "crashed: badshift") != NULL);
    CHECK(strstr(run_bit(OP_SHR64, L(1LL << 40), L(min), &t, &v), "crashed: badshift") != NULL);
    CHECK(strstr(run_bit(OP_XOR64, (BOp){0, BSTR}, I(1), &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_bit(OP_SHR64, I(1), (BOp){0, BSTR}, &t, &v), "crashed: badcode") != NULL);
}
