/* The embedding API (include/gogo.h) against real images (testdata/,
 * compiled by `make test-embed` into $GOGO_EMBED_IMAGES): posts, the inbox
 * under threads, the latched wake, freeing while posting, the code.Load
 * switch, the memory cap, host ports (and the examples/hostecho golden),
 * /flash on a host directory (fs_root, fs_quota) and that gogo_free gives
 * back every byte. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "test.h"
#include "gogo.h"
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* test_main.c calls it before each test (the VM tests' arena). */
void test_host_arena_forget(void) {}

/* ---- a test host: the POSIX one, with counted memory and captured output ---- */

typedef struct TCtx {
    void *px;               /* a POSIX context: wake and wait */
    long bytes, blocks;     /* atomic: host memory outstanding */
    long fail_after;        /* atomic: allocations left before they fail (-1 = never) */
    char out[8192];         /* the start of stream 1 (VM thread) */
    size_t out_len;
    char err[2048];         /* the start of stream 2 */
    size_t err_len;
    long lines;             /* atomic: newlines on stream 1 */
    int delay;              /* wait first spins a random while (the race test) */
    uint32_t rnd;           /* its generator (VM thread) */
    gogo_vm *reenter;       /* write steps this VM again (the re-entry test) */
    int reenter_rc;
    long wakes;             /* atomic: wake calls */
} TCtx;

static const gogo_host *px(void) { return gogo_host_posix(); }

static void t_write(void *c, int s, const char *p, size_t n) {
    TCtx *t = c;
    char *buf = s == 2 ? t->err : t->out;
    size_t cap = s == 2 ? sizeof t->err : sizeof t->out, *len = s == 2 ? &t->err_len : &t->out_len;
    size_t k = n < cap - 1 - *len ? n : cap - 1 - *len;
    memcpy(buf + *len, p, k);
    *len += k;
    buf[*len] = 0;
    if (s == 1)
        for (size_t i = 0; i < n; i++)
            if (p[i] == '\n') __atomic_fetch_add(&t->lines, 1, __ATOMIC_RELEASE);
    if (t->reenter) t->reenter_rc = gogo_step(t->reenter, 1);
}

static void *t_alloc(void *c, size_t n) {
    TCtx *t = c;
    if (__atomic_load_n(&t->fail_after, __ATOMIC_RELAXED) >= 0 &&
        __atomic_fetch_sub(&t->fail_after, 1, __ATOMIC_RELAXED) <= 0) return NULL;
    void *p = malloc(n);
    if (p) {
        __atomic_fetch_add(&t->bytes, (long)n, __ATOMIC_RELAXED);
        __atomic_fetch_add(&t->blocks, 1, __ATOMIC_RELAXED);
    }
    return p;
}

static void t_free(void *c, void *p, size_t n) {
    TCtx *t = c;
    __atomic_fetch_sub(&t->bytes, (long)n, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&t->blocks, 1, __ATOMIC_RELAXED);
    free(p);
}

static uint64_t t_now(void *c) { return px()->now_ms(((TCtx *)c)->px); }
static void t_wake(void *c) {
    __atomic_fetch_add(&((TCtx *)c)->wakes, 1, __ATOMIC_RELAXED);
    px()->wake(((TCtx *)c)->px);
}

static void t_wait(void *c, uint32_t ms) {
    TCtx *t = c;
    if (t->delay) {
        /* Widen the window between the step's last check and the wait: a
         * post landing here must still end the wait (the latch). */
        t->rnd = t->rnd * 1103515245u + 12345u;
        for (uint32_t k = (t->rnd >> 16) % 48u; k; k--) sched_yield();
    }
    px()->wait(t->px, ms);
}

static const gogo_host thost = {
    t_write, t_alloc, t_free, t_now, t_wake, t_wait, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void tctx_init(TCtx *t) {
    memset(t, 0, sizeof *t);
    t->px = gogo_posix_ctx_new();
    t->fail_after = -1;
    t->rnd = 1;
}

/* Everything the VM took is back. */
static void tctx_done(TCtx *t) {
    CHECK_EQ(__atomic_load_n(&t->bytes, __ATOMIC_ACQUIRE), 0);
    CHECK_EQ(__atomic_load_n(&t->blocks, __ATOMIC_ACQUIRE), 0);
    gogo_posix_ctx_free(t->px);
}

/* ---- images ---- */

typedef struct Img { uint32_t *w; size_t len; } Img;

static Img image(const char *name) {
    Img r = { NULL, 0 };
    const char *dir = getenv("GOGO_EMBED_IMAGES");
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ggb", dir ? dir : "build/embed", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); return r; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    r.w = malloc((size_t)n);
    if (fread(r.w, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    r.len = (size_t)n;
    return r;
}

static gogo_vm *new_loaded(TCtx *t, const char *prog, const gogo_config *cfg) {
    gogo_vm *vm = gogo_new(&thost, t, cfg);
    CHECK(vm != NULL);
    if (!vm) return NULL;
    Img im = image(prog);
    CHECK(im.w != NULL);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    return vm;
}

/* Steps until idle or done; returns that. */
static int settle(gogo_vm *vm) {
    int r;
    while ((r = gogo_step(vm, 0)) == GOGO_RUNNABLE) {}
    return r;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Waits (yielding) until t has printed `want` lines; false after 20 s. */
static bool await_lines(TCtx *t, long want) {
    uint64_t end = mono_ms() + 20000u;
    while (__atomic_load_n(&t->lines, __ATOMIC_ACQUIRE) < want) {
        if (mono_ms() > end) return false;
        sched_yield();
    }
    return true;
}

static int post_retry(gogo_vm *vm, const char *name, int32_t kind, const void *d, size_t n) {
    int r;
    while ((r = gogo_post(vm, name, kind, d, n)) == GOGO_EFULL) sched_yield();
    return r;
}

/* ---- tests ---- */

TEST(embed_post_roundtrip) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(gogo_next_deadline(vm), GOGO_NEVER);
    CHECK(strcmp(t.out, "ready\n") == 0);
    CHECK_EQ(gogo_post(vm, "echo", 7, "hello", 5), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "echo", -5, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "echo", 2000000000, "x", 1), GOGO_OK);   /* a boxed Kind */
    static uint8_t big[10000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    CHECK_EQ(gogo_post(vm, "echo", 3, big, sizeof big), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK(strcmp(t.out, "ready\npost 7 5 hello\npost -5 0 \npost 2000000000 1 x\npost 3 10000 0 15\n") == 0);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_delivered, 4);
    CHECK_EQ(st.posts_dropped, 0);
    CHECK(st.mem_used > 0 && st.mem_peak >= st.mem_used);
    /* Limits. */
    CHECK_EQ(gogo_post(vm, "echo", 1, big, GOGO_POST_DATA_MAX + 1u), GOGO_E2BIG);
    char longname[GOGO_POST_NAME_MAX + 2];
    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK_EQ(gogo_post(vm, longname, 1, NULL, 0), GOGO_E2BIG);
    CHECK_EQ(gogo_post(vm, NULL, 1, NULL, 0), GOGO_EINVAL);
    CHECK_EQ(gogo_post(vm, "echo", 1, NULL, 3), GOGO_EINVAL);
    /* The name keeps the VM alive; Kind -1 ends main and the VM. */
    CHECK_EQ(gogo_post(vm, "echo", -1, NULL, 0), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK_EQ(gogo_step(vm, 0), GOGO_DONE);
    CHECK(strstr(t.out, "bye\n") != NULL);
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_post_unknown_name) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(gogo_post(vm, "nobody", 1, "x", 1), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "", 2, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "echo", 3, NULL, 0), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_dropped, 2);
    CHECK_EQ(st.posts_delivered, 1);
    CHECK(strcmp(t.out, "ready\npost 3 0 \n") == 0);
    gogo_free(vm);   /* still running: its ports close, its memory goes back */
    tctx_done(&t);
}

/* A post sent before the program registered its name is dropped. */
TEST(embed_post_before_register) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_post(vm, "echo", 1, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_dropped, 1);
    CHECK(strcmp(t.out, "ready\n") == 0);
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_inbox_full) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);   /* not started: nothing drains */
    for (int i = 0; i < GOGO_POST_MAX; i++) CHECK_EQ(gogo_post(vm, "echo", i, "abc", 3), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "echo", 999, NULL, 0), GOGO_EFULL);
    CHECK_EQ(gogo_post(vm, "echo", 999, NULL, 0), GOGO_EFULL);
    gogo_free(vm);   /* frees the queued posts too */
    tctx_done(&t);
}

TEST(embed_inbox_full_then_drains) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    for (int i = 0; i < GOGO_POST_MAX; i++) CHECK_EQ(gogo_post(vm, "echo", i, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_post(vm, "echo", 999, NULL, 0), GOGO_EFULL);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(gogo_post(vm, "echo", 999, NULL, 0), GOGO_OK);   /* room again */
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(__atomic_load_n(&t.lines, __ATOMIC_ACQUIRE), 1 + GOGO_POST_MAX + 1);
    gogo_free(vm);
    tctx_done(&t);
}

/* ---- threads ---- */

typedef struct Producer {
    pthread_t th;
    gogo_vm *vm;
    int id, n;
    int ok, closed;
} Producer;

static void *produce(void *arg) {
    Producer *p = arg;
    for (int s = 1; s <= p->n; s++) {
        int r = post_retry(p->vm, "count", p->id * 1000000 + s, NULL, 0);
        if (r == GOGO_OK) p->ok++;
        else if (r == GOGO_ECLOSED) { p->closed++; break; }
    }
    return NULL;
}

/* 4 threads x 25000 posts to a VM running on its own thread. */
TEST(embed_post_threads) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "count", NULL);
    CHECK_EQ(gogo_run_thread(vm), GOGO_OK);
    CHECK_EQ(gogo_run_thread(vm), GOGO_ESTATE);
    CHECK(await_lines(&t, 1));
    Producer p[4];
    for (int i = 0; i < 4; i++) {
        p[i] = (Producer){ .vm = vm, .id = i, .n = 25000 };
        pthread_create(&p[i].th, NULL, produce, &p[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(p[i].th, NULL);
        CHECK_EQ(p[i].ok, 25000);
    }
    CHECK_EQ(post_retry(vm, "count", -1, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_join(vm), GOGO_OK);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_delivered + st.posts_dropped, 100001);
    char want[64];
    snprintf(want, sizeof want, "ready\ncount %llu bad 0\n", (unsigned long long)st.posts_delivered - 1u);
    CHECK(strcmp(t.out, want) == 0);
    if (strcmp(t.out, want) != 0) printf("  out: %s", t.out);
    CHECK_EQ(st.posts_dropped, 0);
    gogo_free(vm);
    tctx_done(&t);
}

/* The lost-wakeup race: each post lands while the VM thread goes idle (in
 * the window widened by t_wait's delay); a wake it slept through would
 * hang the handshake. */
TEST(embed_wake_race) {
    TCtx t; tctx_init(&t);
    t.delay = 1;
    const char *e = getenv("GOGO_EMBED_RACE_N");
    long n = e ? atol(e) : 100000;
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_run_thread(vm), GOGO_OK);
    CHECK(await_lines(&t, 1));
    uint32_t rnd = 7;
    long i;
    for (i = 1; i <= n; i++) {
        rnd = rnd * 1103515245u + 12345u;
        for (uint32_t k = (rnd >> 16) % 8u; k; k--) sched_yield();
        if (gogo_post(vm, "echo", (int32_t)i, NULL, 0) != GOGO_OK || !await_lines(&t, i + 1)) break;
    }
    CHECK_EQ(i, n + 1);
    CHECK_EQ(gogo_post(vm, "echo", -1, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_join(vm), GOGO_OK);
    gogo_free(vm);
    tctx_done(&t);
}

/* Producers holding a reference post while the owner frees the VM: no
 * crash, later posts get GOGO_ECLOSED, and every byte comes back once the
 * last reference goes. */
TEST(embed_post_while_freeing) {
    for (int round = 0; round < 20; round++) {
        TCtx t; tctx_init(&t);
        gogo_vm *vm = new_loaded(&t, "count", NULL);
        CHECK_EQ(gogo_run_thread(vm), GOGO_OK);
        CHECK(await_lines(&t, 1));
        Producer p[4];
        for (int i = 0; i < 4; i++) {
            gogo_retain(vm);
            p[i] = (Producer){ .vm = vm, .id = i, .n = 1000000 };
            pthread_create(&p[i].th, NULL, produce, &p[i]);
        }
        struct timespec d = { 0, (long)(round % 5) * 200000L };
        nanosleep(&d, NULL);
        gogo_free(vm);                 /* stops the VM thread, then frees */
        for (int i = 0; i < 4; i++) {
            pthread_join(p[i].th, NULL);
            CHECK_EQ(p[i].closed, 1);
            CHECK_EQ(gogo_post(vm, "count", 1, NULL, 0), GOGO_ECLOSED);
            gogo_release(vm);
        }
        tctx_done(&t);
    }
}

/* The same on a VM the owner steps itself: no run thread, so no join or
 * mutex orders the free against the posts; only the post handshake does.
 * Many short rounds, the free landing at varying points of a post. */
typedef struct Hammer {
    gogo_vm *vm;
    long ok, closed;
} Hammer;

static void *hammer(void *arg) {
    Hammer *h = arg;
    for (;;) {
        int r = gogo_post(h->vm, "echo", 1, "abcd", 4);
        if (r == GOGO_OK) __atomic_fetch_add(&h->ok, 1, __ATOMIC_RELEASE);
        else if (r == GOGO_ECLOSED) { h->closed++; break; }
        else sched_yield();   /* full */
    }
    return NULL;
}

TEST(embed_post_while_freeing_stepped) {
    const char *e = getenv("GOGO_EMBED_FREE_ROUNDS");
    long rounds = e ? atol(e) : 2000;
    gogo_config cfg = { 0 };
    cfg.max_actors = 16;
    for (long round = 0; round < rounds; round++) {
        TCtx t; tctx_init(&t);
        gogo_vm *vm = new_loaded(&t, "echo", &cfg);
        CHECK_EQ(gogo_start(vm), GOGO_OK);
        CHECK_EQ(settle(vm), GOGO_IDLE);
        Hammer h = { vm, 0, 0 };
        pthread_t th;
        gogo_retain(vm);
        pthread_create(&th, NULL, hammer, &h);
        long want = round % 4;   /* free before, right at, or after the first posts */
        while (__atomic_load_n(&h.ok, __ATOMIC_ACQUIRE) < want) sched_yield();
        if (round & 1) gogo_step(vm, 1);
        gogo_free(vm);
        pthread_join(th, NULL);
        CHECK_EQ(h.closed, 1);
        CHECK_EQ(gogo_post(vm, "echo", 1, NULL, 0), GOGO_ECLOSED);
        gogo_release(vm);
        tctx_done(&t);
    }
}

/* gogo_close_posts from another thread ends gogo_run on the owner thread,
 * though the program's actor holds a name and waits for posts. */
static void *close_later(void *arg) {
    gogo_vm *vm = arg;
    struct timespec d = { 0, 20000000L };
    nanosleep(&d, NULL);
    CHECK_EQ(gogo_post(vm, "echo", 5, NULL, 0), GOGO_OK);
    gogo_close_posts(vm);
    CHECK_EQ(gogo_post(vm, "echo", 6, NULL, 0), GOGO_ECLOSED);
    return NULL;
}

TEST(embed_close_posts_ends_run) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    pthread_t th;
    pthread_create(&th, NULL, close_later, vm);
    CHECK_EQ(gogo_run(vm), GOGO_OK);       /* returns: posts closed, main blocked */
    pthread_join(th, NULL);
    /* The post sent before the close was still delivered. */
    CHECK(strcmp(t.out, "ready\npost 5 0 \n") == 0);
    CHECK_EQ(gogo_post(vm, "echo", 7, NULL, 0), GOGO_ECLOSED);
    long wakes = __atomic_load_n(&t.wakes, __ATOMIC_RELAXED);
    gogo_close_posts(vm);                  /* again: no effect, no wake */
    CHECK_EQ(__atomic_load_n(&t.wakes, __ATOMIC_RELAXED), wakes);
    gogo_free(vm);
    tctx_done(&t);
}

/* Once the program has finished, posts are refused (posts close at
 * GOGO_DONE); one that raced the end is counted as dropped. */
TEST(embed_post_after_done_refused) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(gogo_post(vm, "echo", -1, NULL, 0), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK_EQ(gogo_post(vm, "echo", 4, "x", 1), GOGO_ECLOSED);
    CHECK_EQ(gogo_step(vm, 0), GOGO_DONE);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_delivered, 1);
    CHECK_EQ(st.posts_dropped, 0);
    gogo_free(vm);
    tctx_done(&t);
}

/* gogo_free stops a VM thread that is busy (never idle) too. */
TEST(embed_free_stops_busy_thread) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "spin", NULL);
    CHECK_EQ(gogo_run_thread(vm), GOGO_OK);
    struct timespec d = { 0, 2000000L };
    nanosleep(&d, NULL);
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_run_until_done) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "sleeper", NULL);
    CHECK_EQ(gogo_run(vm), GOGO_OK);
    CHECK(strcmp(t.out, "a\nb\n") == 0);
    CHECK_EQ(gogo_run(vm), GOGO_OK);   /* done stays done */
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_next_deadline) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "sleeper", NULL);
    CHECK_EQ(gogo_next_deadline(vm), GOGO_NEVER);   /* not started */
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    uint64_t t0 = mono_ms();
    CHECK_EQ(settle(vm), GOGO_IDLE);
    int64_t dl = gogo_next_deadline(vm);
    CHECK(dl != GOGO_NEVER);
    CHECK(dl >= (int64_t)t0 + 40 && dl <= (int64_t)mono_ms() + 50);
    while (mono_ms() < (uint64_t)dl) sched_yield();
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK(strcmp(t.out, "a\nb\n") == 0);
    CHECK_EQ(gogo_next_deadline(vm), GOGO_NEVER);
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_code_load_switch) {
    for (int allow = 0; allow <= 1; allow++) {
        TCtx t; tctx_init(&t);
        gogo_config cfg = { 0 };
        cfg.allow_code_load = allow;
        gogo_vm *vm = new_loaded(&t, "codeload", &cfg);
        CHECK_EQ(gogo_start(vm), GOGO_OK);
        CHECK_EQ(settle(vm), GOGO_DONE);
        /* refused: NoMemory (3); allowed: the bytes are BadImage (1) */
        CHECK(strcmp(t.out, allow ? "load 1\n" : "load 3\n") == 0);
        gogo_free(vm);
        tctx_done(&t);
    }
}

TEST(embed_mem_limit) {
    size_t peak[2];
    for (int k = 0; k < 2; k++) {
        TCtx t; tctx_init(&t);
        gogo_config cfg = { 0 };
        cfg.max_actors = 16;
        cfg.mem_limit = k ? 0 : 96u * 1024u;
        gogo_vm *vm = new_loaded(&t, "hog", &cfg);
        CHECK_EQ(gogo_start(vm), GOGO_OK);
        CHECK_EQ(settle(vm), GOGO_DONE);
        CHECK(strstr(t.err, "crashed: oom") != NULL);
        gogo_stats st;
        gogo_get_stats(vm, &st);
        peak[k] = st.mem_peak;
        gogo_free(vm);
        tctx_done(&t);
    }
    CHECK(peak[0] <= 96u * 1024u);
    CHECK(peak[1] > 96u * 1024u);   /* the actor's own limit (256 KB) */
}

TEST(embed_lifecycle_errors) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    CHECK(vm != NULL);
    CHECK_EQ(gogo_start(vm), GOGO_ESTATE);
    CHECK_EQ(gogo_step(vm, 0), GOGO_ESTATE);
    CHECK_EQ(gogo_join(vm), GOGO_ESTATE);
    uint32_t junk[16] = { 1, 2, 3 };
    CHECK_EQ(gogo_load(vm, junk, sizeof junk, 0), GOGO_EIMAGE);
    CHECK(strstr(t.err, "gogo: bad image: ") != NULL);
    CHECK_EQ(gogo_load(vm, junk, 7, 0), GOGO_EIMAGE);
    CHECK_EQ(gogo_load(vm, (const char *)junk + 1, 8, 0), GOGO_EINVAL);   /* borrowed, unaligned */
    Img im = image("echo");
    /* A failed load leaves the VM loadable; borrowed this time. */
    CHECK_EQ(gogo_load(vm, im.w, im.len, 0), GOGO_OK);
    CHECK_EQ(gogo_load(vm, im.w, im.len, 0), GOGO_ESTATE);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(gogo_start(vm), GOGO_ESTARTED);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_free(vm);
    free(im.w);
    tctx_done(&t);
}

/* The image format: the same major with a minor up to the VM's loads;
 * an older major or a newer minor is GOGO_EVERSION, naming both formats,
 * and the VM stays loadable. */
TEST(embed_image_version) {
    TCtx t; tctx_init(&t);
    Img im = image("echo");
    CHECK_EQ(im.w[1], 15u << 16);                    /* stamped 15.0 */
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    im.w[1] = 15u << 16 | 2u;
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_EVERSION);
    CHECK(strstr(t.err, "gogo: unsupported image format 15.2 (this VM runs 15.0 to 15.1)") != NULL);
    im.w[1] = 14u;
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_EVERSION);
    CHECK(strstr(t.err, "image format 0.14 ") != NULL);
    im.w[1] = 15u << 16;
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_free(vm);
    free(im.w);
    tctx_done(&t);
}

TEST(embed_no_main) {
    TCtx t; tctx_init(&t);
    /* codeload's image with its only export renamed: no "main". */
    Img im = image("codeload");
    uint8_t *b = (uint8_t *)im.w;
    int renamed = 0;
    for (size_t i = 0; i + 4 <= im.len; i++)
        if (memcmp(b + i, "main", 4) == 0) { b[i] = 'x'; renamed++; }
    CHECK(renamed > 0);
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    int r = gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY);
    if (r == GOGO_OK) {
        CHECK_EQ(gogo_start(vm), GOGO_ESTART);
        CHECK(strstr(t.err, "no main") != NULL);
        CHECK_EQ(gogo_start(vm), GOGO_ESTART);   /* still new: the same refusal */
    }
    gogo_free(vm);
    free(im.w);
    tctx_done(&t);
}

TEST(embed_reentry_refused) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    t.reenter = vm;
    t.reenter_rc = 1;
    CHECK_EQ(settle(vm), GOGO_IDLE);   /* "ready" is written from inside the step */
    CHECK_EQ(t.reenter_rc, GOGO_EBUSY);
    t.reenter = NULL;
    gogo_free(vm);
    tctx_done(&t);
}

TEST(embed_host_validation) {
    TCtx t; tctx_init(&t);
    gogo_host h = thost;
    h.alloc = NULL;
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    h = thost; h.free = NULL;
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    h = thost; h.now_ms = NULL;
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    h = thost; h.write = NULL;
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    h = thost; h.wake = NULL;                  /* wait without wake */
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    h.wait = NULL;                             /* a busy-loop host: fine */
    gogo_vm *vm = gogo_new(&h, &t, NULL);
    CHECK(vm != NULL);
    CHECK_EQ(gogo_run(vm), GOGO_EINVAL);
    CHECK_EQ(gogo_run_thread(vm), GOGO_EINVAL);
    gogo_free(vm);
    h = thost; h.code_free = t_free;           /* one of a pair */
    CHECK(gogo_new(&h, &t, NULL) == NULL);
    gogo_config cfg = { 0 };
    cfg.fs_root = "/nonexistent-gogo-root";    /* not a directory (embed_fs_root_bad) */
    CHECK(gogo_new(&thost, &t, &cfg) == NULL);
    cfg.fs_root = NULL;
    CHECK(gogo_new(NULL, NULL, NULL) == NULL);
    /* Out of memory at every step of gogo_new and gogo_load. */
    Img im = image("echo");
    for (long k = 0; k < 40; k++) {
        t.fail_after = k;
        gogo_vm *v = gogo_new(&thost, &t, NULL);
        if (v) {
            int r = gogo_load(v, im.w, im.len, GOGO_LOAD_COPY);
            CHECK(r == GOGO_OK || r == GOGO_ENOMEM);
            gogo_free(v);
        }
        t.fail_after = -1;
        CHECK_EQ(__atomic_load_n(&t.bytes, __ATOMIC_ACQUIRE), 0);
    }
    free(im.w);
    tctx_done(&t);
}

/* The default host on its own: a VM that owns its POSIX context. */
TEST(embed_posix_host) {
    gogo_vm *vm = gogo_new_posix(NULL);
    CHECK(vm != NULL);
    Img im = image("sleeper");
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    CHECK_EQ(gogo_run(vm), GOGO_OK);   /* prints a and b on stdout */
    gogo_free(vm);
    const gogo_host *h = gogo_host_posix();
    CHECK(h->unix_time_ms(NULL) > 1700000000000LL);
    CHECK_EQ(h->set_unix_time_ms(NULL, 1800000000000LL), 0);
    uint8_t b[300];
    memset(b, 0, sizeof b);
    CHECK_EQ(h->random_bytes(NULL, b, sizeof b), 0);
    int nz = 0;
    for (size_t i = 0; i < sizeof b; i++) nz += b[i] != 0;
    CHECK(nz > 200);
    /* A latched wake: the next wait returns at once. */
    void *c = gogo_posix_ctx_new();
    uint64_t t0 = h->now_ms(c);
    h->wake(c);
    h->wait(c, 5000);
    h->wait(c, 20);   /* the latch was consumed: this one times out */
    uint64_t dt = h->now_ms(c) - t0;
    CHECK(dt >= 15 && dt < 2000);
    gogo_posix_ctx_free(c);
}

/* ---- host ports (gogo_register_port) ---- */

/* A host port driver's record of its calls. Its open accepts Arg >= 0
 * (with echo set, it answers with an event Kind = Arg, Data = the cfg's
 * Data); a command is recorded and, with echo set, sent back to the owner
 * with Kind + 1. */
typedef struct HCtx {
    int echo;
    int opens, commands, closes;   /* atomic */
    gogo_port port;                /* the last opened (VM thread, read after a sync) */
    int32_t arg, kind;
    char data[64];
    size_t len;
    int emit_rc, emit_fails;
    int32_t open_kind;             /* nonzero: open emits (open_kind, open_data) */
    const char *open_data;
    int refuse;                    /* open refuses (after its emit) */
    int close_emit_rc;             /* an emit tried from close */
    gogo_vm *vm;                   /* set: re-entry tried from open */
    int reenter_step, reenter_reg, reenter_start;
} HCtx;

static int h_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    HCtx *h = ctx;
    if (arg < 0) return -1;
    h->port = port;
    h->arg = arg;
    h->len = len < sizeof h->data ? len : sizeof h->data;
    if (len) memcpy(h->data, data, h->len);
    if (h->vm) {
        static const gogo_port_driver none = { NULL, NULL, NULL };
        h->reenter_step = gogo_step(h->vm, 1);
        h->reenter_reg = gogo_register_port(h->vm, "late", &none, NULL);
        h->reenter_start = gogo_start(h->vm);
    }
    if (h->echo) h->emit_rc = gogo_port_emit(vm, port, arg, data, len);
    if (h->open_kind) h->emit_rc = gogo_port_emit(vm, port, h->open_kind, h->open_data, strlen(h->open_data));
    if (h->emit_rc) h->emit_fails++;
    if (h->refuse) return -1;
    __atomic_fetch_add(&h->opens, 1, __ATOMIC_RELEASE);
    return 0;
}

static void h_command(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len) {
    HCtx *h = ctx;
    h->kind = kind;
    h->len = len < sizeof h->data ? len : sizeof h->data;
    if (len) memcpy(h->data, data, h->len);
    if (h->echo) h->emit_rc = gogo_port_emit(vm, port, kind + 1, data, len);
    if (h->emit_rc) h->emit_fails++;
    __atomic_fetch_add(&h->commands, 1, __ATOMIC_RELEASE);
}

static void h_close(void *ctx, gogo_vm *vm, gogo_port port) {
    HCtx *h = ctx;
    h->close_emit_rc = gogo_port_emit(vm, port, 1000, NULL, 0);
    __atomic_fetch_add(&h->closes, 1, __ATOMIC_RELEASE);
}

static const gogo_port_driver hdrv = { h_open, h_command, h_close };

static gogo_vm *new_hostport(TCtx *t, HCtx *h) {
    memset(h, 0, sizeof *h);
    gogo_vm *vm = gogo_new(&thost, t, NULL);
    CHECK(vm != NULL);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, h), GOGO_OK);
    Img im = image("hostport");
    CHECK(im.w != NULL);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    return vm;
}

static int ctl(gogo_vm *vm, int32_t kind) {
    int r = gogo_post(vm, "ctl", kind, NULL, 0);
    return r ? r : settle(vm);
}

/* The golden: examples/hostecho under an echo host, its output compared
 * with examples/hostecho/embed.out. */
TEST(embed_hostecho_golden) {
    TCtx t; tctx_init(&t);
    HCtx h; memset(&h, 0, sizeof h);
    h.echo = 1;
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    CHECK_EQ(gogo_register_port(vm, "echo", &hdrv, &h), GOGO_OK);
    Img im = image("hostecho");
    CHECK(im.w != NULL);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    CHECK_EQ(gogo_run(vm), GOGO_OK);
    const char *dir = getenv("GOGO_EMBED_EXAMPLES");
    char path[512], want[2048];
    snprintf(path, sizeof path, "%s/hostecho/embed.out", dir ? dir : "examples");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    size_t n = f ? fread(want, 1, sizeof want - 1, f) : 0;
    if (f) fclose(f);
    want[n] = 0;
    CHECK(strcmp(t.out, want) == 0);
    if (strcmp(t.out, want) != 0) printf("  out:\n%s", t.out);
    CHECK_EQ(h.opens, 1);          /* Arg -1 was refused by the host */
    CHECK_EQ(h.commands, 4);       /* the stale Send never reached it */
    CHECK_EQ(h.closes, 1);
    CHECK_EQ(h.emit_rc, GOGO_OK);
    CHECK_EQ(h.emit_fails, 0);
    gogo_free(vm);
    tctx_done(&t);
}

/* Open with a config, a command in, an event out (from outside a step and
 * from inside the command), the Kind boxed and the Data large. */
TEST(embed_hostport_echo) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.opens, 1);
    CHECK_EQ(h.arg, 1);
    CHECK_EQ(h.len, 0);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1000, "hi", 2), GOGO_OK);
    static uint8_t big[5000];
    CHECK_EQ(gogo_port_emit(vm, h.port, 2000000000, big, sizeof big), GOGO_OK);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1001, NULL, 0), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(ctl(vm, 2), GOGO_IDLE);
    CHECK_EQ(h.commands, 1);
    CHECK_EQ(h.kind, 5);
    CHECK(h.len == 3 && memcmp(h.data, "cmd", 3) == 0);
    CHECK(strcmp(t.out, "ready\nopen true\nevent 1000 2\nevent 2000000000 5000\nevent 1001 0\n") == 0);
    /* Bad arguments. */
    CHECK_EQ(gogo_port_emit(vm, h.port, 1, NULL, 1), GOGO_EINVAL);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1, big, GOGO_POST_DATA_MAX + 1u), GOGO_E2BIG);
    gogo_port bad = { 99, 0 };
    CHECK_EQ(gogo_port_emit(vm, bad, 1, NULL, 0), GOGO_EINVAL);
    CHECK_EQ(gogo_port_post(vm, bad, 1, NULL, 0), GOGO_EINVAL);
    gogo_port other = { (uint16_t)(h.port.index + 1u), 0 };   /* no port there */
    CHECK_EQ(gogo_port_emit(vm, other, 1, NULL, 0), GOGO_ECLOSED);
    CHECK_EQ(h.closes, 0);
    CHECK_EQ(ctl(vm, -1), GOGO_DONE);   /* main's exit closes its port */
    CHECK_EQ(h.closes, 1);
    gogo_free(vm);
    CHECK_EQ(h.closes, 1);
    tctx_done(&t);
}

/* A closed port's value never names it again, nor the port opened next in
 * its slot; a Send to its stale PID never reaches the host. */
TEST(embed_hostport_stale) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    gogo_port old = h.port;
    CHECK_EQ(ctl(vm, 3), GOGO_IDLE);
    CHECK_EQ(h.closes, 1);
    CHECK_EQ(h.close_emit_rc, GOGO_ECLOSED);   /* no event after the close */
    CHECK_EQ(h.commands, 0);
    CHECK_EQ(gogo_port_emit(vm, old, 1000, NULL, 0), GOGO_ECLOSED);
    CHECK_EQ(gogo_port_post(vm, old, 1000, NULL, 0), GOGO_OK);   /* dropped when delivered */
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    CHECK_EQ(st.posts_dropped, 1);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.port.index, old.index);
    CHECK(h.port.gen != old.gen);
    CHECK_EQ(gogo_port_emit(vm, old, 1000, NULL, 0), GOGO_ECLOSED);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1000, NULL, 0), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK(strcmp(t.out, "ready\nopen true\nclosed\nopen true\nevent 1000 0\n") == 0);
    gogo_free(vm);                     /* the open port is closed: once */
    CHECK_EQ(h.closes, 2);
    tctx_done(&t);
}

/* The owner's exit closes the port, and the host's close runs once. */
TEST(embed_hostport_owner_exit) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    CHECK_EQ(ctl(vm, 5), GOGO_IDLE);
    CHECK_EQ(h.opens, 1);
    CHECK_EQ(h.arg, 2);
    CHECK_EQ(h.closes, 1);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1000, NULL, 0), GOGO_ECLOSED);
    CHECK(strcmp(t.out, "ready\nchild opened true\n") == 0);
    gogo_free(vm);
    CHECK_EQ(h.closes, 1);
    tctx_done(&t);
}

/* Handoff: later events go to the new owner, whose exit closes the port. */
TEST(embed_hostport_handoff) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(ctl(vm, 4), GOGO_IDLE);
    CHECK_EQ(gogo_port_emit(vm, h.port, 1000, "x", 1), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(h.closes, 0);
    CHECK_EQ(gogo_port_post(vm, h.port, 1999, "end", 3), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(h.closes, 1);
    CHECK(strcmp(t.out, "ready\nopen true\nhandoff true\nkid event 1000 x\nkid event 1999 end\n") == 0);
    gogo_free(vm);
    CHECK_EQ(h.closes, 1);
    tctx_done(&t);
}

/* Registration: the runtime's driver names, duplicates, the limit, after
 * the load, and from a hook (open's re-entry attempts). The embed board
 * has no drivers (test_hostport.c gives a VM a board that has). */
TEST(embed_hostport_register) {
    TCtx t; tctx_init(&t);
    HCtx h; memset(&h, 0, sizeof h);
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    CHECK_EQ(gogo_register_port(vm, "gpio", &hdrv, &h), GOGO_EEXIST);
    CHECK_EQ(gogo_register_port(vm, "tcp", &hdrv, &h), GOGO_EEXIST);
    CHECK_EQ(gogo_register_port(vm, "screen", &hdrv, &h), GOGO_EEXIST);
    CHECK_EQ(gogo_register_port(vm, "file", &hdrv, &h), GOGO_EEXIST);
    CHECK_EQ(gogo_register_port(vm, NULL, &hdrv, &h), GOGO_EINVAL);
    CHECK_EQ(gogo_register_port(vm, "", &hdrv, &h), GOGO_EINVAL);
    CHECK_EQ(gogo_register_port(vm, "x", NULL, &h), GOGO_EINVAL);
    char longname[GOGO_PORT_NAME_MAX + 2];
    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK_EQ(gogo_register_port(vm, longname, &hdrv, &h), GOGO_E2BIG);
    longname[GOGO_PORT_NAME_MAX] = 0;
    CHECK_EQ(gogo_register_port(vm, longname, &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_EEXIST);
    CHECK_EQ(gogo_register_port(vm, "c", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_register_port(vm, "d", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_register_port(vm, "e", &hdrv, &h), GOGO_EFULL);
    /* A failed load keeps the registrations. */
    uint32_t junk[4] = { 1, 2, 3, 4 };
    CHECK_EQ(gogo_load(vm, junk, sizeof junk, 0), GOGO_EIMAGE);
    Img im = image("hostport");
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    CHECK_EQ(gogo_register_port(vm, "f", &hdrv, &h), GOGO_ESTATE);    /* after the load */
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(gogo_register_port(vm, "e", &hdrv, &h), GOGO_ESTATE);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    h.vm = vm;
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.opens, 1);
    CHECK_EQ(h.reenter_step, GOGO_EBUSY);
    CHECK_EQ(h.reenter_reg, GOGO_EBUSY);
    CHECK_EQ(h.reenter_start, GOGO_EBUSY);
    gogo_free(vm);
    CHECK_EQ(h.closes, 1);
    tctx_done(&t);
}

/* With posts closed, an open host port no longer keeps the VM alive. */
TEST(embed_hostport_close_posts) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    gogo_close_posts(vm);
    CHECK_EQ(gogo_port_post(vm, h.port, 1000, NULL, 0), GOGO_ECLOSED);
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK_EQ(h.closes, 1);
    gogo_free(vm);
    tctx_done(&t);
}

/* Requirements: the hostport image opens "hp" by a constant name, so it
 * requires it. Unregistered, the load is GOGO_EREQUIRES naming "hp" (and
 * the VM stays loadable); registered before the load, it loads; a grant
 * that leaves "hp" out hides the registered port. */
TEST(embed_hostport_requirement) {
    TCtx t; tctx_init(&t);
    HCtx h; memset(&h, 0, sizeof h);
    Img im = image("hostport");
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_EREQUIRES);
    CHECK(strstr(t.err, "gogo: image requires hp, which this VM lacks\n") != NULL);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    gogo_free(vm);
    static const char *const only_ui[] = { "ui", NULL };
    static const char *const with_hp[] = { "ui", "hp", NULL };
    gogo_config cfg = { 0 };
    cfg.grant = only_ui;
    vm = gogo_new(&thost, &t, &cfg);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_EREQUIRES);
    gogo_free(vm);
    cfg.grant = with_hp;
    vm = gogo_new(&thost, &t, &cfg);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_OK);
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.opens, 1);
    gogo_free(vm);
    free(im.w);
    tctx_done(&t);
}

typedef struct PortProducer {
    pthread_t th;
    gogo_vm *vm;
    gogo_port port;
    int n, ok;                     /* ok: atomic */
} PortProducer;

static void *port_produce(void *arg) {
    PortProducer *p = arg;
    for (int s = 0; s < p->n; s++) {
        int r;
        while ((r = gogo_port_post(p->vm, p->port, 1000 + s, "data", 4)) == GOGO_EFULL) sched_yield();
        if (r == GOGO_OK) __atomic_fetch_add(&p->ok, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* Posts from another thread to a port that closes meanwhile: those
 * delivered after the close are dropped and counted; nothing is lost
 * from the accounting. */
TEST(embed_hostport_post_threads) {
    TCtx t; tctx_init(&t);
    HCtx h; memset(&h, 0, sizeof h);
    gogo_vm *vm = gogo_new(&thost, &t, NULL);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_OK);
    Img im = image("hostport");
    CHECK_EQ(gogo_load(vm, im.w, im.len, GOGO_LOAD_COPY), GOGO_OK);
    free(im.w);
    CHECK_EQ(gogo_run_thread(vm), GOGO_OK);
    CHECK(await_lines(&t, 1));
    CHECK_EQ(post_retry(vm, "ctl", 1, NULL, 0), GOGO_OK);
    CHECK(await_lines(&t, 2));                 /* "open true": the port is known */
    CHECK_EQ(__atomic_load_n(&h.opens, __ATOMIC_ACQUIRE), 1);
    PortProducer p = { .vm = vm, .port = h.port, .n = 3000 };
    pthread_create(&p.th, NULL, port_produce, &p);
    while (__atomic_load_n(&p.ok, __ATOMIC_ACQUIRE) < 1000) sched_yield();
    CHECK_EQ(post_retry(vm, "ctl", 3, NULL, 0), GOGO_OK);   /* closes mid-stream */
    pthread_join(p.th, NULL);
    CHECK_EQ(p.ok, 3000);
    uint64_t end = mono_ms() + 20000u;
    while (__atomic_load_n(&h.closes, __ATOMIC_ACQUIRE) < 1 && mono_ms() < end) sched_yield();
    CHECK_EQ(__atomic_load_n(&h.closes, __ATOMIC_ACQUIRE), 1);
    for (int i = 0; i < 20; i++) CHECK_EQ(gogo_port_post(vm, h.port, 3, NULL, 0), GOGO_OK);
    CHECK_EQ(post_retry(vm, "ctl", -1, NULL, 0), GOGO_OK);
    CHECK_EQ(gogo_join(vm), GOGO_OK);
    gogo_stats st;
    gogo_get_stats(vm, &st);
    long events = __atomic_load_n(&t.lines, __ATOMIC_ACQUIRE) - 4;   /* ready, open, closed, bye */
    /* 3 ctl posts and the events delivered; the rest dropped. */
    CHECK_EQ(st.posts_delivered, 3 + (uint64_t)events);
    CHECK_EQ(st.posts_delivered + st.posts_dropped, 3 + 3000 + 20);
    CHECK(events >= 1000);
    CHECK(st.posts_dropped >= 20);
    CHECK_EQ(h.closes, 1);
    gogo_free(vm);
    tctx_done(&t);
}

/* Events emitted from open wait for its outcome: a refused open's are
 * discarded (inline and off-heap alike), an accepted one's follow it. */
TEST(embed_hostport_open_emit) {
    TCtx t; tctx_init(&t);
    HCtx h;
    gogo_vm *vm = new_hostport(&t, &h);
    static const char big[] = "an event long enough to go off-heap as a Bytes handle, over sixty-four bytes";
    h.refuse = 1;
    h.open_kind = 4242;
    h.open_data = big;
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.emit_rc, GOGO_OK);
    h.open_data = "ghost";
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK_EQ(h.emit_rc, GOGO_OK);
    CHECK(strcmp(t.out, "ready\nopen false\nopen false\n") == 0);
    h.refuse = 0;
    h.open_kind = 1500;
    h.open_data = "hi";
    CHECK_EQ(ctl(vm, 1), GOGO_IDLE);
    CHECK(strcmp(t.out, "ready\nopen false\nopen false\nopen true\nevent 1500 2\n") == 0);
    CHECK_EQ(h.opens, 1);
    CHECK_EQ(h.closes, 0);          /* a refused open has no close */
    gogo_free(vm);
    CHECK_EQ(h.closes, 1);
    tctx_done(&t);
}

/* Registration is before gogo_load (a registered port satisfies the
 * image's requirement of its name): after it, GOGO_ESTATE. */
TEST(embed_hostport_register_after_load) {
    TCtx t; tctx_init(&t);
    HCtx h; memset(&h, 0, sizeof h);
    gogo_vm *vm = new_loaded(&t, "echo", NULL);
    CHECK_EQ(gogo_register_port(vm, "hp", &hdrv, &h), GOGO_ESTATE);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_IDLE);
    gogo_free(vm);
    tctx_done(&t);
}

/* ---- /flash (fs_root) ---- */

/* A fresh temp directory in buf (cap bytes). */
static void temp_dir(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/gogoembed-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(buf) != NULL);
}
static void rm_tree(const char *dir) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    CHECK_EQ(system(cmd), 0);
}
static long file_size(const char *dir, const char *rel) {
    char p[600];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    return lstat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* Two VMs with their own roots and quotas, stepped alternately: each
 * counts only its own files. */
TEST(embed_fs_root_quota_per_vm) {
    char ra[256], rb[256];
    temp_dir(ra, sizeof ra);
    temp_dir(rb, sizeof rb);
    TCtx ta, tb;
    tctx_init(&ta);
    tctx_init(&tb);
    gogo_config ca = { 0 }, cb = { 0 };
    ca.fs_root = ra;
    ca.fs_quota = 1000;
    cb.fs_root = rb;
    cb.fs_quota = 2500;
    gogo_vm *a = new_loaded(&ta, "fsuse", &ca);
    gogo_vm *b = new_loaded(&tb, "fsuse", &cb);
    CHECK_EQ(gogo_start(a), GOGO_OK);
    CHECK_EQ(gogo_start(b), GOGO_OK);
    int sa = GOGO_RUNNABLE, sb = GOGO_RUNNABLE;
    for (long i = 0; i < 1000000 && (sa == GOGO_RUNNABLE || sb == GOGO_RUNNABLE); i++) {
        if (sa == GOGO_RUNNABLE) sa = gogo_step(a, 4);
        if (sb == GOGO_RUNNABLE) sb = gogo_step(b, 4);
    }
    CHECK_EQ(sa, GOGO_DONE);
    CHECK_EQ(sb, GOGO_DONE);
    /* a.txt holds 5 bytes; big gets the whole 100-byte chunks that fit
     * and then the rest of the room. */
    static const char want_a[] = "write 0\nread hello 0\nstat 0\nmkdir 0\nreaddir 0\nbig 900 4\n"
                                 "usage 1000 1000 0\nrename 0\nremove 0\n";
    static const char want_b[] = "write 0\nread hello 0\nstat 0\nmkdir 0\nreaddir 0\nbig 2400 4\n"
                                 "usage 2500 2500 0\nrename 0\nremove 0\n";
    CHECK(strcmp(ta.out, want_a) == 0);
    CHECK(strcmp(tb.out, want_b) == 0);
    if (strcmp(ta.out, want_a) || strcmp(tb.out, want_b)) printf("  a:[%s]  b:[%s]\n", ta.out, tb.out);
    CHECK_EQ(ta.err_len, 0);
    CHECK_EQ(file_size(ra, "d/a.txt"), 5);
    CHECK_EQ(file_size(rb, "d/a.txt"), 5);
    CHECK_EQ(file_size(ra, "big"), -1);
    gogo_free(a);
    gogo_free(b);
    tctx_done(&ta);
    tctx_done(&tb);
    rm_tree(ra);
    rm_tree(rb);
}

/* No fs_root: there is no storage, and every gogo/fs call is NotMounted
 * (as on a board without the medium), never a bad-code crash. */
TEST(embed_fs_none_is_not_mounted) {
    TCtx t; tctx_init(&t);
    gogo_vm *vm = new_loaded(&t, "fsuse", NULL);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_DONE);
    static const char want[] = "write 3\nread  3\nstat 3\nmkdir 3\nreaddir 3\nbig 0 3\n"
                               "usage 0 0 3\nrename 3\nremove 3\n";
    CHECK(strcmp(t.out, want) == 0);
    if (strcmp(t.out, want)) printf("  out:\n%s  err:\n%s", t.out, t.err);
    CHECK_EQ(t.err_len, 0);
    gogo_free(vm);
    tctx_done(&t);
}

/* Through the VM: /flash/d is a symlink to a directory outside the
 * root. Mkdir sees it exists, Rename into it is BadPath (7), and nothing
 * outside changes. WriteFile of /flash/a.txt, a symlink to an outside
 * file, replaces the link (rename over it), never the target. */
TEST(embed_fs_root_symlink_refused) {
    char base[256], root[300], secret[300], link[400];
    temp_dir(base, sizeof base);
    snprintf(root, sizeof root, "%s/root", base);
    snprintf(secret, sizeof secret, "%s/secret", base);
    CHECK_EQ(mkdir(root, 0777), 0);
    FILE *f = fopen(secret, "w");
    CHECK(f != NULL);
    if (f) { fputs("secret", f); fclose(f); }
    snprintf(link, sizeof link, "%s/a.txt", root);
    CHECK_EQ(symlink(secret, link), 0);
    snprintf(link, sizeof link, "%s/d", root);
    CHECK_EQ(symlink(base, link), 0);
    TCtx t; tctx_init(&t);
    gogo_config c = { 0 };
    c.fs_root = root;                   /* fs_quota 0: none */
    gogo_vm *vm = new_loaded(&t, "fsuse", &c);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK(strncmp(t.out, "write 0\nread hello 0\nstat 0\nmkdir 2\n", 36) == 0);
    CHECK(strstr(t.out, "rename 7\n") != NULL);
    CHECK_EQ(file_size(base, "secret"), 6);
    CHECK_EQ(file_size(base, "a.txt"), -1);
    CHECK_EQ(file_size(root, "a.txt"), 5);   /* a file now, in the root */
    gogo_free(vm);
    tctx_done(&t);
    rm_tree(base);
}

/* An fs_root that is not a directory makes gogo_new fail, leaking nothing. */
TEST(embed_fs_root_bad) {
    char base[256], file[300], missing[300];
    temp_dir(base, sizeof base);
    snprintf(file, sizeof file, "%s/file", base);
    snprintf(missing, sizeof missing, "%s/missing", base);
    FILE *f = fopen(file, "w");
    if (f) fclose(f);
    TCtx t; tctx_init(&t);
    gogo_config c = { 0 };
    c.fs_root = file;
    CHECK(gogo_new(&thost, &t, &c) == NULL);
    c.fs_root = missing;
    CHECK(gogo_new(&thost, &t, &c) == NULL);
    c.fs_root = base;
    c.fs_quota = -1;                    /* negative: invalid (0 is none) */
    CHECK(gogo_new(&thost, &t, &c) == NULL);
    c.fs_root = NULL;
    CHECK(gogo_new(&thost, &t, &c) == NULL);
    c.fs_root = base;
    c.fs_quota = 0;
    t.fail_after = 1;                   /* the gogo_vm, not its FsDir */
    CHECK(gogo_new(&thost, &t, &c) == NULL);
    t.fail_after = -1;
    tctx_done(&t);
    rm_tree(base);
}

/* fs_quota 0 is no quota: the program fills past any small limit, and
 * Usage reports the host file system. */
TEST(embed_fs_quota_zero_is_unlimited) {
    char root[256];
    temp_dir(root, sizeof root);
    TCtx t; tctx_init(&t);
    gogo_config c = { 0 };
    c.fs_root = root;
    gogo_vm *vm = new_loaded(&t, "fsuse", &c);
    CHECK_EQ(gogo_start(vm), GOGO_OK);
    CHECK_EQ(settle(vm), GOGO_DONE);
    CHECK(strncmp(t.out, "write 0\nread hello 0\nstat 0\nmkdir 0\nreaddir 0\nbig 100000 0\nusage ", 65) == 0);
    if (strncmp(t.out, "write 0\nread hello 0\nstat 0\nmkdir 0\nreaddir 0\nbig 100000 0\nusage ", 65))
        printf("  out:[%s]\n", t.out);
    CHECK(strstr(t.out, "rename 0\nremove 0\n") != NULL);
    gogo_free(vm);
    tctx_done(&t);
    rm_tree(root);
}
