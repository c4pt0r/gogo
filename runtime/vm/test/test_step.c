/* Driving the scheduler step by step (interp.h): vm_start, vm_step's
 * RUNNABLE / IDLE / DONE, vm_next_deadline against a fake clock, vm_stop,
 * and vm_run's refusals. */
#include "test.h"
#include "test_host.h"
#include "actor.h"
#include "alloc.h"
#include "evq.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "port.h"
#include "term.h"
#include "timer.h"
#include <string.h>

extern void (*platform_unix_err_hook)(const char *, size_t);
static char err_buf[256]; static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}

static uint64_t fake_ms;
static uint64_t fake_clock(void) { return fake_ms; }

/* main: for i := 0; i < 150000; i++ {} (many time slices). */
#define BUSY_WORDS 30
static void busy_image(uint32_t *w, Image *img) {
    memset(w, 0, BUSY_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 11; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (4u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_LOADK, 0, 0, 0); w[15] = term_from_int(0);
    w[16] = OP_MAKE(OP_LOADK, 1, 0, 0); w[17] = term_from_int(150000);
    w[18] = OP_MAKE(OP_LOADK, 2, 0, 0); w[19] = term_from_int(1);
    w[20] = OP_MAKE(OP_ADD, 0, 0, 2);
    w[21] = OP_MAKE(OP_LT, 3, 0, 1);
    w[22] = OP_MAKE(OP_JMPT, 3, 0, 0); w[23] = 6;
    w[24] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, img_seal_main(w)), IMG_OK);
}

/* main: time.Sleep(ms) (ms < 0: returns at once). */
#define SLEEP_WORDS 28
static void sleep_image(uint32_t *w, Image *img, int32_t ms) {
    memset(w, 0, SLEEP_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 5; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (2u << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_LOADK, 0, 0, 0); w[15] = term_from_int(ms);
    w[16] = OP_MAKE(OP_BUILTIN, 1, 0, 1); w[17] = B_SLEEP;
    w[18] = OP_MAKE(OP_RET, 0, 0, 0);
    if (ms < 0) w[16] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, img_seal_main(w)), IMG_OK);
}

static void init(Vm *vm, const Image *img) {
    va_reset(vm);
    CHECK_EQ(test_vm_init(vm, img, 8), 0);
}

/* With budget 1 a busy actor gets one slice per step and is still
 * runnable after it; budget 0 runs it to the end. */
TEST(step_busy_budget) {
    uint32_t w[BUSY_WORDS]; Image img; Vm vm;
    busy_image(w, &img);
    init(&vm, &img);
    CHECK_EQ(vm_start(&vm), 0);
    CHECK_EQ(vm_step(&vm, 1), VM_RUNNABLE);
    CHECK(vm.runq_head != NULL);
    CHECK_EQ(vm_step(&vm, 1), VM_RUNNABLE);
    CHECK_EQ(vm_step(&vm, 3), VM_RUNNABLE);
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);
    CHECK_EQ(vm_step(&vm, 1), VM_DONE);
    CHECK_EQ(vm.live, 0u);
    vm_stop(&vm);
}

/* The budget runs out on main's last slice: DONE at once, not RUNNABLE. */
TEST(step_budget_last_slice) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    sleep_image(w, &img, -1);
    init(&vm, &img);
    CHECK_EQ(vm_start(&vm), 0);
    CHECK_EQ(vm_step(&vm, 1), VM_DONE);
    vm_stop(&vm);
}

/* A sleeping main: IDLE until the timer's deadline, which
 * vm_next_deadline reports in the Vm's epoch (vm_host_ms: the host's). */
TEST(step_timer_deadline) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    sleep_image(w, &img, 50);
    init(&vm, &img);
    fake_ms = 1000;
    test_host_set_clock(fake_clock);
    CHECK_EQ(vm_start(&vm), 0);
    uint64_t dl = 0;
    CHECK(!vm_next_deadline(&vm, &dl));          /* main has not run yet */
    CHECK_EQ(vm_step(&vm, 0), VM_IDLE);
    CHECK(vm_next_deadline(&vm, &dl));
    CHECK_EQ(dl, 50);
    CHECK_EQ(vm_host_ms(&vm, dl), 1050);
    fake_ms = 1049;
    CHECK_EQ(vm_step(&vm, 0), VM_IDLE);
    CHECK_EQ(vm_step(&vm, 1), VM_IDLE);
    fake_ms = 1050;                              /* due: never IDLE now */
    CHECK_EQ(vm_step(&vm, 1), VM_DONE);          /* fired; main's last slice */
    CHECK(!vm_next_deadline(&vm, &dl));
    vm_stop(&vm);
    test_host_set_clock(NULL);
}

/* An extra source: live while `live`, due while `left` > 0 (each drain is
 * one slice of its work, like a sliced TLS handshake); the first drain
 * after `push_once` is set pushes a ring event, as an ISR could mid-pass. */
static struct { bool live, push_once; int left, drains; } src;
static bool s_pending(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return src.left > 0; }
static void s_drain(Vm *vm, void *ctx, uint64_t now) {
    (void)ctx; (void)now;
    src.drains++;
    src.left--;
    if (src.push_once) {
        PortEvent e = { GOGO_MAX_PORTS, 0, 1, 2, 0 };    /* no such port: dropped on drain */
        src.push_once = false;
        CHECK(evq_push(vm, &e));
    }
}
static bool s_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return src.live; }
static bool s_deadline(const Vm *vm, void *ctx, uint64_t *d) { (void)vm; (void)ctx; (void)d; return false; }
static const PortSource step_src = { s_pending, s_drain, s_live, s_deadline, NULL };

static void src_setup(Vm *vm, uint32_t *w, Image *img) {
    sleep_image(w, img, -1);                     /* main returns at once */
    init(vm, img);
    memset(&src, 0, sizeof src);
    src.live = true;
    CHECK(port_add_source(vm, &step_src, NULL));
    CHECK_EQ(vm_start(vm), 0);
    CHECK_EQ(vm_step(vm, 0), VM_IDLE);           /* main done; the source keeps it alive */
}

/* Never IDLE while an event is queued: an event pushed during a pass is
 * drained by another pass before vm_step reports IDLE. */
TEST(step_no_idle_with_event) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    src_setup(&vm, w, &img);
    PortEvent e = { GOGO_MAX_PORTS, 0, 1, 2, 0 };
    CHECK(evq_push(&vm, &e));
    CHECK_EQ(vm_step(&vm, 0), VM_IDLE);
    CHECK(evq_empty(&vm));
    src.left = 1;
    src.push_once = true;
    CHECK_EQ(vm_step(&vm, 0), VM_IDLE);
    CHECK_EQ(src.drains, 1);
    CHECK(evq_empty(&vm));                       /* the pushed event was drained too */
    CHECK(!port_due_now(&vm));
    src.left = 1;
    src.push_once = true;
    CHECK_EQ(vm_step(&vm, 1), VM_RUNNABLE);      /* the event is still due */
    CHECK(!evq_empty(&vm));
    CHECK_EQ(vm_step(&vm, 1), VM_IDLE);
    CHECK(evq_empty(&vm));
    src.live = false;
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);
    vm_stop(&vm);
    port_remove_source(&vm, &step_src);
}

/* A source due for N slices: with budget 1 each step runs exactly one
 * slice and reports RUNNABLE while the source is still due after it (the
 * first N-1 steps), IDLE once it is not; budget 0 runs all N in one step. */
TEST(step_busy_source) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    src_setup(&vm, w, &img);
    enum { N = 5 };
    src.left = N;
    for (int i = 1; i <= N; i++) {
        CHECK_EQ(vm_step(&vm, 1), i < N ? VM_RUNNABLE : VM_IDLE);
        CHECK_EQ(src.drains, i);
    }
    src.left = N;
    CHECK_EQ(vm_step(&vm, 0), VM_IDLE);
    CHECK_EQ(src.drains, 2 * N);
    src.live = false;
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);
    vm_stop(&vm);
    port_remove_source(&vm, &step_src);
}

/* DONE once main has exited and nothing else is live; a finished or
 * stopped Vm stays DONE and refuses vm_start and vm_run. */
TEST(step_done_and_reentry) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    sleep_image(w, &img, -1);
    init(&vm, &img);
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);          /* not started */
    CHECK_EQ(vm_start(&vm), 0);
    CHECK_EQ(vm_start(&vm), 1);                  /* already running */
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);
    CHECK_EQ(vm_step(&vm, 1), VM_DONE);
    CHECK_EQ(vm_start(&vm), 1);
    vm_stop(&vm);
    vm_stop(&vm);                                /* repeatable */
    CHECK(vm.reserve == NULL);
    CHECK_EQ(vm_start(&vm), 1);
    CHECK_EQ(vm_run(&vm), 1);
    CHECK_EQ(vm_step(&vm, 0), VM_DONE);
    CHECK_EQ(vm.live, 0u);
}

/* vm_run needs host->wait; without it, it refuses before starting. */
TEST(step_run_needs_wait) {
    uint32_t w[SLEEP_WORDS]; Image img; Vm vm;
    sleep_image(w, &img, -1);
    init(&vm, &img);
    void (*wait)(void *, uint32_t) = test_host.wait;
    test_host.wait = NULL;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    CHECK_EQ(vm_run(&vm), 1);
    platform_unix_err_hook = NULL;
    test_host.wait = wait;
    CHECK(strstr(err_buf, "host has no wait") != NULL);
    CHECK_EQ(vm.run_state, VM_ST_NEW);
    CHECK_EQ(vm_run(&vm), 0);                    /* with wait: runs */
    CHECK_EQ(vm_run(&vm), 1);                    /* once */
}
