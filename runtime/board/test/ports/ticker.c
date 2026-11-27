/* "test.ticker": a port fed from a thread of its own. Its open (Arg n)
 * makes a poster and starts a thread that posts n events Kind 1..n with
 * the Data "tick" through gogo_poster_port_post, as a driver's own task
 * does on a board. One port at a time.
 *
 * The close rule (book 22.7): close runs on the VM's thread, the only one
 * that drains the posts, so it must never wait for a thread that may be
 * waiting for room (GOGO_EFULL). close sets stop first; the thread checks
 * it before every post and while it waits, then ends, and only then does
 * close join it. */
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "gogo.h"

typedef struct Ticker {
    gogo_poster *poster;
    gogo_port port;
    int32_t n;
    pthread_t thread;
    int running;
    int stop;                    /* atomic: set by close */
} Ticker;

static int stopped(Ticker *t) { return __atomic_load_n(&t->stop, __ATOMIC_ACQUIRE); }

static void *tick(void *arg) {
    Ticker *t = arg;
    static const struct timespec ms = { 0, 1000000 };
    for (int32_t k = 1; k <= t->n && !stopped(t); k++) {
        while (gogo_poster_port_post(t->poster, t->port, k, "tick", 4) == GOGO_EFULL && !stopped(t))
            nanosleep(&ms, NULL);   /* a pause, not a spin: the VM drains meanwhile */
    }
    gogo_poster_free(t->poster);   /* this thread's poster: freed here, once */
    return NULL;
}

static int ticker_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    Ticker *t = ctx;
    (void)data, (void)len;
    if (t->running) return GOGO_EBUSY;
    if (arg < 1) return GOGO_EINVAL;
    if (!(t->poster = gogo_poster_new(vm))) return GOGO_ENOMEM;
    t->port = port;
    t->n = arg;
    __atomic_store_n(&t->stop, 0, __ATOMIC_RELEASE);
    if (pthread_create(&t->thread, NULL, tick, t) != 0) {
        gogo_poster_free(t->poster);
        return GOGO_ENOMEM;
    }
    t->running = 1;
    return 0;
}

static void ticker_close(void *ctx, gogo_vm *vm, gogo_port port) {
    Ticker *t = ctx;
    (void)vm, (void)port;
    if (!t->running) return;
    __atomic_store_n(&t->stop, 1, __ATOMIC_RELEASE);   /* first: the thread may be waiting for room */
    pthread_join(t->thread, NULL);
    t->running = 0;
}

static const gogo_port_driver ticker = { .size = sizeof(gogo_port_driver), .open = ticker_open, .close = ticker_close };
static Ticker ticker_state;

GOGO_BOARD_PORT("test.ticker", &ticker, &ticker_state);
