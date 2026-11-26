/* Many VMs in one process: 8 VMs on 4 threads, each thread stepping its
 * two VMs alternately and sleeping in one latched wait between them. Each
 * VM runs an examples/ program that needs no hardware (many_idle,
 * httpparse, hotload with its modules in a /flash directory), its output
 * captured through the write hook's ctx and compared, after the threads
 * are joined, with the program's expected.out (the CLI's golden). Every
 * VM gives back all the host memory it took. Run under ThreadSanitizer by
 * make test-embed-tsan and checked for leaks by make test-embed-leaks.
 * GOGO_EMBED_MULTI_N sets the number of rounds (default 2). */
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "gogo.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define M_THREADS 4
#define M_VMS 8
#define M_OUT 4096

typedef struct MWorker MWorker;

/* One VM's host ctx: its captured output, its memory count, its thread. */
typedef struct MJob {
    const char *prog;
    MWorker *w;
    gogo_vm *vm;
    char out[M_OUT];
    size_t out_len;
    int out_over;            /* output longer than out[] */
    long bytes, blocks;      /* atomic: host memory outstanding */
    int load_rc, start_rc, end_rc;
    uint64_t steps;
} MJob;

struct MWorker {
    pthread_t t;
    void *px;                /* the thread's latched wait (a POSIX host ctx) */
    MJob *job[M_VMS / M_THREADS];
};

static void m_write(void *c, int stream, const char *s, size_t n) {
    MJob *j = c;
    if (stream != 1) return;   /* crash reports (hotload's planned crash) */
    if (n > M_OUT - 1 - j->out_len) { n = M_OUT - 1 - j->out_len; j->out_over = 1; }
    memcpy(j->out + j->out_len, s, n);
    j->out_len += n;
    j->out[j->out_len] = 0;
}

static void *m_alloc(void *c, size_t n) {
    MJob *j = c;
    void *p = malloc(n ? n : 1);
    if (p) {
        __atomic_fetch_add(&j->bytes, (long)n, __ATOMIC_RELAXED);
        __atomic_fetch_add(&j->blocks, 1, __ATOMIC_RELAXED);
    }
    return p;
}

static void m_free(void *c, void *p, size_t n) {
    MJob *j = c;
    __atomic_fetch_sub(&j->bytes, (long)n, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&j->blocks, 1, __ATOMIC_RELAXED);
    free(p);
}

static uint64_t m_now(void *c) {
    struct timespec ts;
    (void)c;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Both VMs of a thread wake its one wait. */
static void m_wake(void *c) { gogo_host_posix()->wake(((MJob *)c)->w->px); }

/* No wait hook: the thread waits for its two VMs itself. */
static const gogo_host mhost = {
    m_write, m_alloc, m_free, m_now, m_wake, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void *m_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = (size_t)n; }
    return b;
}

static const char *images_dir(void) {
    const char *d = getenv("GOGO_EMBED_IMAGES");
    return d ? d : "build/embed";
}

static void *m_worker(void *arg) {
    MWorker *w = arg;
    enum { N = M_VMS / M_THREADS };
    char flash[512];
    snprintf(flash, sizeof flash, "%s/hotload-flash", images_dir());
    int live = 0;
    for (int i = 0; i < N; i++) {
        MJob *j = w->job[i];
        gogo_config cfg;
        memset(&cfg, 0, sizeof cfg);
        if (strcmp(j->prog, "hotload") == 0) {
            cfg.fs_root = flash;          /* read only: the VMs may share it */
            cfg.allow_code_load = 1;
        }
        j->vm = gogo_new(&mhost, j, &cfg);
        j->load_rc = j->start_rc = GOGO_ESTATE;
        if (!j->vm) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s.ggb", images_dir(), j->prog);
        size_t len = 0;
        void *img = m_read(path, &len);
        j->load_rc = img ? gogo_load(j->vm, img, len, GOGO_LOAD_COPY) : GOGO_EINVAL;
        free(img);
        if (j->load_rc == GOGO_OK) j->start_rc = gogo_start(j->vm);
        if (j->start_rc == GOGO_OK) live |= 1 << i;
    }
    while (live) {
        int busy = 0;
        int64_t next = GOGO_NEVER;
        for (int i = 0; i < N; i++) {
            if (!(live & 1 << i)) continue;
            MJob *j = w->job[i];
            int r = gogo_step(j->vm, 16);
            j->steps++;
            if (r == GOGO_RUNNABLE) { busy = 1; continue; }
            if (r != GOGO_IDLE) { j->end_rc = r; live &= ~(1 << i); continue; }
            int64_t d = gogo_next_deadline(j->vm);
            if (d != GOGO_NEVER && (next == GOGO_NEVER || d < next)) next = d;
        }
        if (busy || !live) continue;
        uint32_t ms = UINT32_MAX;
        if (next != GOGO_NEVER) {
            int64_t now = (int64_t)m_now(NULL);
            ms = next <= now ? 0u : (uint32_t)(next - now);
        }
        /* Latched: a wake since the last wait (from a step above) returns
         * at once; the latch is consumed here, before the next steps. */
        gogo_host_posix()->wait(w->px, ms);
    }
    for (int i = 0; i < N; i++) {
        gogo_free(w->job[i]->vm);
        w->job[i]->vm = NULL;
    }
    return NULL;
}

TEST(embed_multi_vm_threads) {
    static const char *const progs[] = { "many_idle", "httpparse", "hotload" };
    enum { NP = sizeof progs / sizeof progs[0] };
    const char *ex = getenv("GOGO_EMBED_EXAMPLES");
    char *want[NP];
    for (int p = 0; p < NP; p++) {
        char path[512];
        size_t n;
        snprintf(path, sizeof path, "%s/%s/expected.out", ex ? ex : "examples", progs[p]);
        want[p] = m_read(path, &n);
        CHECK(want[p] != NULL);
        if (!want[p]) { printf("  cannot read %s\n", path); return; }
    }
    const char *env = getenv("GOGO_EMBED_MULTI_N");
    int rounds = env ? atoi(env) : 2;
    static MJob jobs[M_VMS];
    static MWorker ws[M_THREADS];
    for (int round = 0; round < rounds; round++) {
        memset(jobs, 0, sizeof jobs);
        memset(ws, 0, sizeof ws);
        for (int k = 0; k < M_VMS; k++) {
            /* Rotate, so each thread gets a different pair each round. */
            jobs[k].prog = progs[(k + round) % NP];
            MWorker *w = &ws[k % M_THREADS];
            jobs[k].w = w;
            w->job[k / M_THREADS] = &jobs[k];
        }
        int started = 0;
        for (int t = 0; t < M_THREADS; t++) {
            ws[t].px = gogo_posix_ctx_new();
            CHECK(ws[t].px != NULL);
            if (ws[t].px && pthread_create(&ws[t].t, NULL, m_worker, &ws[t]) == 0) started |= 1 << t;
        }
        for (int t = 0; t < M_THREADS; t++) {
            if (started & 1 << t) pthread_join(ws[t].t, NULL);
            else CHECK(0);
            gogo_posix_ctx_free(ws[t].px);
        }
        for (int k = 0; k < M_VMS; k++) {
            MJob *j = &jobs[k];
            int p = 0;
            while (strcmp(progs[p], j->prog) != 0) p++;
            CHECK_EQ(j->load_rc, GOGO_OK);
            CHECK_EQ(j->start_rc, GOGO_OK);
            CHECK_EQ(j->end_rc, GOGO_DONE);
            CHECK(!j->out_over);
            CHECK(strcmp(j->out, want[p]) == 0);
            if (strcmp(j->out, want[p]) != 0) printf("  round %d vm %d (%s) printed:\n%s", round, k, j->prog, j->out);
            CHECK_EQ(__atomic_load_n(&j->bytes, __ATOMIC_ACQUIRE), 0);
            CHECK_EQ(__atomic_load_n(&j->blocks, __ATOMIC_ACQUIRE), 0);
        }
    }
    for (int p = 0; p < NP; p++) free(want[p]);
}
