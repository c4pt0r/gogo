//go:build cgo && libgogo

/* The host table and port driver of the binding: memory, the default
 * clocks and entropy in C, everything else in Go (vm.go, port.go). */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "bind.h"
#include "_cgo_export.h"
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

typedef struct BindCtx {
    uintptr_t h;    /* the Go state (a cgo.Handle) */
    int64_t live;   /* bytes allocated and not yet freed, atomic */
} BindCtx;

static void b_write(void *ctx, int stream, const char *s, size_t n) {
    bindWrite(((BindCtx *)ctx)->h, stream, (char *)s, n);
}

static void *b_alloc(void *ctx, size_t n) {
    void *p = malloc(n ? n : 1u);
    if (p) __atomic_add_fetch(&((BindCtx *)ctx)->live, (int64_t)n, __ATOMIC_RELAXED);
    return p;
}

static void b_free(void *ctx, void *p, size_t n) {
    if (!p) return;
    __atomic_sub_fetch(&((BindCtx *)ctx)->live, (int64_t)n, __ATOMIC_RELAXED);
    free(p);
}

uint64_t bind_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t b_now_os(void *ctx) { (void)ctx; return bind_now(); }
static uint64_t b_now_go(void *ctx) { return bindNow(((BindCtx *)ctx)->h); }

static void b_wake(void *ctx) { bindWake(((BindCtx *)ctx)->h); }

static int64_t b_unix_os(void *ctx) {
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec <= 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int b_set_unix_os(void *ctx, int64_t ms) { (void)ctx; (void)ms; return GOGO_EINVAL; }   /* the OS owns it */
static int64_t b_unix_go(void *ctx) { return bindUnixTime(((BindCtx *)ctx)->h); }
static int b_set_unix_go(void *ctx, int64_t ms) { return bindSetUnixTime(((BindCtx *)ctx)->h, ms); }

static uint32_t b_random_os(void *ctx) {
    (void)ctx;
    uint32_t x = 0;
    if (getentropy(&x, sizeof x) != 0) abort();   /* as gogo_host_posix: never guessable bits */
    return x;
}

static uint32_t b_random_go(void *ctx) { return bindRandom(((BindCtx *)ctx)->h); }

static int32_t b_config_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    return bindConfigGet(((BindCtx *)ctx)->h, (char *)k, kl, buf, cap);
}

static void b_destroy(void *ctx) {
    BindCtx *c = ctx;
    bindDestroy(c->h, __atomic_load_n(&c->live, __ATOMIC_RELAXED));
    free(c);
}

int bind_new(uintptr_t h, unsigned hooks, const gogo_config *cfg, gogo_vm **out) {
    BindCtx *c = malloc(sizeof *c);
    if (!c) {
        *out = NULL;
        return GOGO_ENOMEM;
    }
    c->h = h;
    c->live = 0;
    gogo_host t = GOGO_HOST_INIT;
    t.write = b_write;
    t.alloc = b_alloc;
    t.free = b_free;
    t.now_ms = hooks & BIND_NOW ? b_now_go : b_now_os;
    t.wake = b_wake;
    t.unix_time_ms = hooks & BIND_UNIX_TIME ? b_unix_go : b_unix_os;
    t.set_unix_time_ms = hooks & BIND_UNIX_TIME ? b_set_unix_go : b_set_unix_os;
    t.random = hooks & BIND_RANDOM ? b_random_go : b_random_os;
    t.config_get = hooks & BIND_CONFIG ? b_config_get : NULL;
    t.destroy = b_destroy;
    int rc = gogo_new(&t, c, cfg, out);
    if (rc != GOGO_OK) free(c);   /* no destroy when gogo_new fails */
    return rc;
}

static int p_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    (void)vm;
    return bindPortOpen((uintptr_t)ctx, port, arg, (uint8_t *)data, len);
}

static void p_command(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len) {
    (void)vm;
    bindPortCommand((uintptr_t)ctx, port, kind, (uint8_t *)data, len);
}

static void p_close(void *ctx, gogo_vm *vm, gogo_port port) {
    (void)vm;
    bindPortClose((uintptr_t)ctx, port);
}

static const gogo_port_driver bind_driver = {
    .size = sizeof(gogo_port_driver),
    .open = p_open,
    .command = p_command,
    .close = p_close,
};

int bind_register_port(gogo_vm *vm, const char *name, size_t len, uintptr_t h) {
    return gogo_register_port(vm, name, len, &bind_driver, (void *)h);
}
