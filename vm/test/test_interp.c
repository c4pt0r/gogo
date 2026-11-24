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
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);
    CHECK(r->mbox_head == NULL);
    actor_free(&vm, r);
    actor_free(&vm, s);
}

/* A message too big for the receiver's heap crashes the receiver with oom;
 * the message stays queued until actor_free, and other actors go on. */
TEST(receive_oom_crashes_only_receiver) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    va_reset();
    build_receiver(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 4), 0);
    size_t base = va_used_bytes();

    /* The receiver keeps 200 live heap words (r0) besides its frame. */
    Actor *r = start(&vm, &img, 256);
    term keep = big_struct(r, 200);
    r->mem[r->fp] = keep;

    /* 65500-word message: fits a mailbox, not next to 200 live words. */
    Actor *s = actor_new(&vm, 65500);
    CHECK(s != NULL);
    CHECK_EQ(msg_send(&vm, s, r->id, big_struct(s, 65500)), 0);

    Actor *ok = start(&vm, &img, 32);
    CHECK_EQ(msg_send(&vm, s, ok->id, term_from_int(1)), 0);

    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(interp_run(&vm, r), RUN_EXIT);       /* crashed: oom */
    platform_unix_err_hook = NULL;
    CHECK(strstr(err_buf, "crashed: oom") != NULL);
    CHECK(r->mbox_head != NULL);                  /* still queued */
    CHECK_EQ(interp_run(&vm, ok), RUN_EXIT);      /* unaffected */
    CHECK(ok->mbox_head == NULL);

    actor_free(&vm, r);                           /* frees the message */
    actor_free(&vm, ok);
    actor_free(&vm, s);
    CHECK_EQ(vm.live, 0);
    CHECK_EQ(va_used_bytes(), base);
}
