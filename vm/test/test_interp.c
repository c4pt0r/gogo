/* Interpreter-level actor tests on a hand-built image. */
#include "test.h"
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
#define IMG_WORDS 17

static void build_receiver(uint32_t *w) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = 0x4F474F47; w[1] = 1; w[2] = 1; w[3] = 0;
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
    va_reset();
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
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
    va_reset();
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes();

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

    va_set_limit(va_used_bytes());                /* no new blocks */
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);       /* crashed: oom, freed */
    platform_unix_err_hook = NULL;
    va_set_limit(0);
    CHECK(strstr(err_buf, "crashed: oom in main") != NULL);
    CHECK_EQ(interp_run(&vm, ok), RUN_EXIT);      /* unaffected */

    actor_free(&vm, s);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(), base);              /* r's message was freed */
}

TEST(make_ref_allocates_serials) {
    uint32_t w[IMG_WORDS + 1]; Image img; Vm vm;
    va_reset();
    build_receiver(w);
    w[15] = B_MAKE_REF;
    w[16] = OP_MAKE(OP_BUILTIN, 0, 0, 0);      /* then block in RECEIVE */
    w[17] = B_RECEIVE;
    w[5] = 4;
    CHECK_EQ(image_load(&img, w, IMG_WORDS + 1), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
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
    va_reset();
    build_receiver(w);
    w[15] = B_LINK;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
    Actor *a = start(&vm, &img, 32);
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    platform_unix_err_hook = NULL;
    CHECK(strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(vm.live, 0);
}

/* One function "main" (0 params, 3 regs) with two K_INT64 consts x, y:
 *   LOADK r0, x; LOADK r1, y; <op> r2, r0, r1 (c ignored by NEG);
 *   RECEIVE r0 (blocks, so the result can be inspected)
 * x_small != 0 loads x as the small int x_small instead. */
#define I64_WORDS 27

static void build_int64_op(uint32_t *w, uint32_t op, int64_t x, int64_t y, int32_t x_small) {
    memset(w, 0, I64_WORDS * sizeof(uint32_t));
    w[0] = 0x4F474F47; w[1] = 1; w[2] = 1; w[3] = 0;
    w[4] = 9; w[5] = 7; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (3u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = hdr_make(3, K_INT64); w[15] = (uint32_t)(uint64_t)x; w[16] = (uint32_t)((uint64_t)x >> 32);
    w[17] = hdr_make(3, K_INT64); w[18] = (uint32_t)(uint64_t)y; w[19] = (uint32_t)((uint64_t)y >> 32);
    w[20] = OP_MAKE(OP_LOADK, 0, 0, 0); w[21] = x_small ? term_from_int(x_small) : term_from_const(3);
    w[22] = OP_MAKE(OP_LOADK, 1, 0, 0); w[23] = term_from_const(6);
    w[24] = OP_MAKE(op, 2, 0, op == OP_NEG ? 0 : 1);
    w[25] = OP_MAKE(OP_BUILTIN, 0, 0, 0);
    w[26] = B_RECEIVE;
}

/* Runs the op; returns the crash text ("" if none) and the result in *out. */
static const char *run_int64_op(uint32_t op, int64_t x, int64_t y, int32_t x_small, term *out, int64_t *val) {
    static uint32_t w[I64_WORDS]; static Image img; static Vm vm;
    va_reset();
    build_int64_op(w, op, x, y, x_small);
    CHECK_EQ(image_load(&img, w, I64_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
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
    CHECK_EQ(run_int64_op(OP_DIV, min, -1, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_REM, min, -1, 0, &t, &v)[0], 0); CHECK(v == 0 && term_is_ref(t));
    CHECK_EQ(run_int64_op(OP_NEG, min, 0, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_ADD, max, 1, 0, &t, &v)[0], 0); CHECK(v == min);
    CHECK_EQ(run_int64_op(OP_SUB, min, 1, 0, &t, &v)[0], 0); CHECK(v == max);
    CHECK_EQ(run_int64_op(OP_MUL, max, max, 0, &t, &v)[0], 0); CHECK(v == 1);
    CHECK_EQ(run_int64_op(OP_REM, -7, 2, 0, &t, &v)[0], 0); CHECK(v == -1);
    CHECK_EQ(run_int64_op(OP_LT, -1, 0, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_LE, 1LL << 40, 1, 0, &t, &v)[0], 0); CHECK(t == T_FALSE);
    CHECK_EQ(run_int64_op(OP_EQ, 1LL << 40, 1LL << 40, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK_EQ(run_int64_op(OP_NE, 1LL << 40, (1LL << 40) + 1, 0, &t, &v)[0], 0); CHECK(t == T_TRUE);
    CHECK(strstr(run_int64_op(OP_DIV, 5, 0, 0, &t, &v), "crashed: divzero") != NULL);
    CHECK(strstr(run_int64_op(OP_REM, 5, 0, 0, &t, &v), "crashed: divzero") != NULL);
    /* Mixed int / int64 operands are malformed code. */
    CHECK(strstr(run_int64_op(OP_ADD, 0, 5, 3, &t, &v), "crashed: badcode") != NULL);
    CHECK(strstr(run_int64_op(OP_LT, 0, 5, 3, &t, &v), "crashed: badcode") != NULL);
}

TEST(int64_conv) {
    term t; int64_t v;
    /* CONV r2, r0, 1: int -> int64 sign-extends (r0 small int -3). */
    CHECK_EQ(run_int64_op(OP_CONV, 0, 0, -3, &t, &v)[0], 0); CHECK(v == -3 && term_is_ref(t));
    /* CONV r2, r1, 0 is built by patching: int64 -> int truncates. */
    static uint32_t w[I64_WORDS]; Image img; Vm vm;
    va_reset();
    build_int64_op(w, OP_CONV, 0, -0x180000000LL, 0);
    w[24] = OP_MAKE(OP_CONV, 2, 1, 0);
    CHECK_EQ(image_load(&img, w, I64_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
    Actor *a = actor_new(&vm, 32);
    CHECK(heap_push_frame(&vm, a, 3, -1));
    a->pc = img.code;
    CHECK_EQ(interp_run(&vm, a), RUN_WAIT);
    term r = a->mem[a->fp + 2];
    CHECK(term_is_ref(r));
    CHECK_EQ(a->mem[term_ref_off(r)], hdr_make(2, K_INT));
    CHECK_EQ((int32_t)a->mem[term_ref_off(r) + 1], INT32_MIN);
    actor_free(&vm, a);
    /* CONV of a non-int is badcode. */
    va_reset();
    build_int64_op(w, OP_CONV, 0, 0, 0);
    w[21] = T_TRUE;
    CHECK_EQ(image_load(&img, w, I64_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
    a = actor_new(&vm, 32);
    CHECK(heap_push_frame(&vm, a, 3, -1));
    a->pc = img.code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, a), RUN_EXIT);
    platform_unix_err_hook = NULL;
    CHECK(strstr(err_buf, "crashed: badcode") != NULL);
    CHECK_EQ(vm.live, 0);
}
