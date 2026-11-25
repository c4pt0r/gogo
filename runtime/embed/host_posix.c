/* The POSIX default host (gogo_host_posix) and gogo_run_thread. All state
 * is in a context (PosixCtx) or the gogo_vm: nothing is global. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "embed.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

/* The latched wait: wake sets latch under the lock and signals; wait
 * returns at once while latch is set and clears it on the way out. */
typedef struct PosixCtx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int latch;
} PosixCtx;

void *gogo_posix_ctx_new(void) {
    PosixCtx *c = malloc(sizeof *c);
    if (!c) return NULL;
    c->latch = 0;
    if (pthread_mutex_init(&c->mu, NULL) != 0) {
        free(c);
        return NULL;
    }
    int rc;
#if defined(__APPLE__)
    rc = pthread_cond_init(&c->cv, NULL);   /* waits are relative (below) */
#else
    pthread_condattr_t a;
    rc = pthread_condattr_init(&a);
    if (rc == 0) {
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        rc = pthread_cond_init(&c->cv, &a);
        pthread_condattr_destroy(&a);
    }
#endif
    if (rc != 0) {
        pthread_mutex_destroy(&c->mu);
        free(c);
        return NULL;
    }
    return c;
}

void gogo_posix_ctx_free(void *ctx) {
    PosixCtx *c = ctx;
    if (!c) return;
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

static void p_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    int fd = stream == 2 ? 2 : 1;
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return;
        s += k;
        n -= (size_t)k;
    }
}

static void *p_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1u); }
static void p_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }

static uint64_t p_now(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void p_wake(void *ctx) {
    PosixCtx *c = ctx;
    pthread_mutex_lock(&c->mu);
    c->latch = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static void p_wait(void *ctx, uint32_t ms) {
    PosixCtx *c = ctx;
    pthread_mutex_lock(&c->mu);
    if (!c->latch) {
        if (ms == UINT32_MAX) {
            while (!c->latch) pthread_cond_wait(&c->cv, &c->mu);
        } else {
            /* One timed wait: an early (spurious) return is allowed. */
#if defined(__APPLE__)
            struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
            pthread_cond_timedwait_relative_np(&c->cv, &c->mu, &d);
#else
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            t.tv_sec += (time_t)(ms / 1000u);
            t.tv_nsec += (long)(ms % 1000u) * 1000000L;
            if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&c->cv, &c->mu, &t);
#endif
        }
    }
    c->latch = 0;
    pthread_mutex_unlock(&c->mu);
}

static int64_t p_unix(void *ctx) {
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec <= 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int p_set_unix(void *ctx, int64_t ms) { (void)ctx; (void)ms; return 0; }   /* the OS owns it */

static int p_random_bytes(void *ctx, uint8_t *buf, size_t n) {
    (void)ctx;
    while (n) {
        size_t k = n > 256u ? 256u : n;   /* getentropy's limit */
        if (getentropy(buf, k) != 0) return -1;
        buf += k;
        n -= k;
    }
    return 0;
}

/* random promises bits a peer cannot predict: with no entropy source
 * left, stop rather than return something guessable. */
static uint32_t p_random(void *ctx) {
    uint32_t x = 0;
    if (p_random_bytes(ctx, (uint8_t *)&x, sizeof x) != 0) abort();
    return x;
}

static const gogo_host posix_host = {
    p_write, p_alloc, p_free, p_now, p_wake, p_wait, p_unix, p_set_unix,
    p_random, p_random_bytes, NULL, NULL, NULL
};

const gogo_host *gogo_host_posix(void) { return &posix_host; }

gogo_vm *gogo_new_posix(const gogo_config *cfg) {
    void *ctx = gogo_posix_ctx_new();
    if (!ctx) return NULL;
    gogo_vm *g = gogo_new(&posix_host, ctx, cfg);
    if (!g) {
        gogo_posix_ctx_free(ctx);
        return NULL;
    }
    g->ctx_free = gogo_posix_ctx_free;   /* at the last release */
    return g;
}

/* ---- gogo_run_thread ---- */

struct EmbedThread {
    pthread_t t;
    int rc;
};

static void *run_main(void *arg) {
    gogo_vm *g = arg;
    g->thread->rc = gogo_run(g);
    return NULL;
}

int gogo_run_thread(gogo_vm *g) {
    if (!g || !g->uh->wait) return GOGO_EINVAL;
    if (g->thread) return GOGO_ESTATE;
    struct EmbedThread *t = g->uh->alloc(g->uctx, sizeof *t);
    if (!t) return GOGO_ENOMEM;
    t->rc = 0;
    g->thread = t;
    if (pthread_create(&t->t, NULL, run_main, g) != 0) {
        g->thread = NULL;
        g->uh->free(g->uctx, t, sizeof *t);
        return GOGO_ENOMEM;
    }
    return GOGO_OK;
}

int gogo_join(gogo_vm *g) {
    if (!g || !g->thread) return GOGO_ESTATE;
    struct EmbedThread *t = g->thread;
    pthread_join(t->t, NULL);
    int rc = t->rc;
    g->thread = NULL;
    g->uh->free(g->uctx, t, sizeof *t);
    return rc;
}

void embed_thread_stop(gogo_vm *g) {
    /* The state is already EMBED_DEAD: the run loop stops at its next
     * check, and the wake ends an idle wait (latched). */
    if (g->uh->wake) g->uh->wake(g->uctx);
    gogo_join(g);
}
