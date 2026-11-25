/* embed-c: run a gogo image inside a C program (libgogo.a, gogo.h).
 *
 *   main [--flash DIR] [--quota BYTES] [--allow-load] IMAGE.gogo
 *       one VM on the POSIX host, run to the end with gogo_run
 *   main --two A.gogo B.gogo
 *       two VMs stepped alternately by one loop on this thread, which
 *       sleeps in poll() on a wake pipe until the nearer deadline; each
 *       VM's lines are prefixed with "a| " or "b| "
 *
 * Build an image with `gogo build examples/many_idle -o many_idle.gogo`,
 * and this program with `make example-embed` (build/embed-c/main). */
#define _POSIX_C_SOURCE 200809L
#include "gogo.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Reads a whole file; NULL on failure. */
static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0, k;
    do {
        if (n == cap) {
            char *nb = realloc(buf, cap = cap ? cap * 2 : 65536);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        k = fread(buf + n, 1, cap - n, f);
        n += k;
    } while (k > 0);
    fclose(f);
    *len = n;
    return buf;
}

/* A new VM on host h with the image at path loaded; NULL (reported) on failure. */
static gogo_vm *load_vm(const gogo_host *h, void *ctx, const gogo_config *cfg, const char *path) {
    size_t len;
    void *img = read_file(path, &len);
    if (!img) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    gogo_vm *vm = h ? gogo_new(h, ctx, cfg) : gogo_new_posix(cfg);
    if (!vm) { fprintf(stderr, "gogo_new failed\n"); free(img); return NULL; }
    int rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);   /* copied: img can go */
    free(img);
    if (rc != GOGO_OK) {
        fprintf(stderr, "gogo_load %s: error %d\n", path, rc);
        gogo_free(vm);
        return NULL;
    }
    return vm;
}

/* ---- one VM: the POSIX host and gogo_run ---- */

static int run_one(const char *path, const gogo_config *cfg) {
    gogo_vm *vm = load_vm(NULL, NULL, cfg, path);
    if (!vm) return 1;
    int rc = gogo_run(vm);   /* starts main, runs until every actor is done */
    gogo_free(vm);
    return rc == GOGO_OK ? 0 : 1;
}

/* ---- two VMs, one loop: a host of our own with a wake pipe ---- */

/* Each VM's host context: its line prefix and the loop's wake pipe. */
typedef struct Ctx {
    const char *tag;   /* "a| " */
    int bol[2];        /* stdout, stderr: at the beginning of a line */
    int wake_fd;       /* the pipe's write end, shared by both VMs */
} Ctx;

static void w_write(void *c, int stream, const char *s, size_t n) {
    Ctx *x = c;
    FILE *f = stream == 2 ? stderr : stdout;
    for (size_t i = 0; i < n; i++) {
        int *bol = &x->bol[stream == 2];
        if (*bol) fputs(x->tag, f);
        fputc(s[i], f);
        *bol = s[i] == '\n';
    }
}
static void *w_alloc(void *c, size_t n) { (void)c; return malloc(n ? n : 1); }
static void w_free(void *c, void *p, size_t n) { (void)c; (void)n; free(p); }
static uint64_t w_now(void *c) {
    struct timespec ts;
    (void)c;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
/* Any thread: a byte in the pipe is the latch. It stays there until the
 * loop drains the pipe, so a wake that comes after a step has returned
 * GOGO_IDLE makes the next poll() return at once. */
static void w_wake(void *c) {
    char b = 1;
    ssize_t r = write(((Ctx *)c)->wake_fd, &b, 1);   /* full pipe: already latched */
    (void)r;
}

/* No wait hook: this loop sleeps in poll() itself. */
static const gogo_host pipe_host = {
    w_write, w_alloc, w_free, w_now, w_wake, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static int run_two(const char *pa, const char *pb) {
    int fds[2];
    if (pipe(fds) != 0) return 1;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    Ctx ctx[2] = { { "a| ", { 1, 1 }, fds[1] }, { "b| ", { 1, 1 }, fds[1] } };
    gogo_vm *vm[2];
    vm[0] = load_vm(&pipe_host, &ctx[0], NULL, pa);
    vm[1] = vm[0] ? load_vm(&pipe_host, &ctx[1], NULL, pb) : NULL;
    int live = 0, rc = vm[0] && vm[1] ? 0 : 1;   /* an image that cannot be read or loaded */
    for (int i = 0; i < 2 && vm[1]; i++)
        if (gogo_start(vm[i]) == GOGO_OK) live |= 1 << i;
        else rc = 1;
    while (live) {
        /* Consume the latch before stepping, never between an idle
         * result and the poll below. */
        char buf[64];
        while (read(fds[0], buf, sizeof buf) > 0) {}
        int busy = 0;
        int64_t next = GOGO_NEVER;
        for (int i = 0; i < 2; i++) {
            if (!(live & 1 << i)) continue;
            int r = gogo_step(vm[i], 64);   /* a slice of work, then the other VM */
            if (r == GOGO_DONE || r < 0) { live &= ~(1 << i); if (r < 0) rc = 1; continue; }
            if (r == GOGO_RUNNABLE) { busy = 1; continue; }
            int64_t d = gogo_next_deadline(vm[i]);   /* GOGO_IDLE */
            if (d != GOGO_NEVER && (next == GOGO_NEVER || d < next)) next = d;
        }
        if (busy || !live) continue;
        int timeout = -1;   /* no deadline: only a wake ends the wait */
        if (next != GOGO_NEVER) {
            int64_t now = (int64_t)w_now(NULL);
            timeout = next <= now ? 0 : next - now > 60000 ? 60000 : (int)(next - now);
        }
        struct pollfd p = { fds[0], POLLIN, 0 };
        if (poll(&p, 1, timeout) < 0 && errno != EINTR) { rc = 1; break; }
    }
    fflush(stdout);
    gogo_free(vm[0]);
    gogo_free(vm[1]);
    close(fds[0]);
    close(fds[1]);
    return rc;
}

int main(int argc, char **argv) {
    gogo_config cfg;
    memset(&cfg, 0, sizeof cfg);   /* no caps, no memory cap, no /flash */
    int i = 1;
    if (argc == 4 && strcmp(argv[1], "--two") == 0) return run_two(argv[2], argv[3]);
    for (; i < argc - 1; i++) {
        if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc - 1) cfg.fs_root = argv[++i];
        else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc - 1) cfg.fs_quota = strtoll(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--allow-load") == 0) cfg.allow_code_load = 1;
        else break;
    }
    if (i != argc - 1) {
        fprintf(stderr, "usage: %s [--flash DIR] [--quota BYTES] [--allow-load] IMAGE.gogo\n"
                        "       %s --two A.gogo B.gogo\n", argv[0], argv[0]);
        return 2;
    }
    return run_one(argv[i], &cfg);
}
