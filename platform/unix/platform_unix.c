#define _POSIX_C_SOURCE 200112L   /* clock_gettime, nanosleep, poll, pipe */
#include "platform.h"
#include "config.h"
#include "gpio_unix.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

void platform_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
/* Flush program output first so diagnostics interleave in order. */
void (*platform_unix_err_hook)(const char *, size_t);  /* test-only capture */
void platform_write_err(const char *s, size_t n) {
    if (platform_unix_err_hook) { platform_unix_err_hook(s, n); return; }
    fflush(stdout);
    fwrite(s, 1, n, stderr);
}

/* Bytes handed out by platform_chunk_alloc and not yet freed; a nonzero cap
 * makes the allocator fail past it, like a small MCU arena. */
static size_t arena_used;
static size_t arena_cap = GOGO_ARENA_BYTES;

void platform_unix_set_arena_cap(size_t bytes) { arena_cap = bytes; }   /* test-only */
size_t platform_unix_arena_used(void) { return arena_used; }             /* test-only */
/* Test-only: forgets memory earlier tests leaked (va_reset abandons chunks),
 * so each test starts with the whole cap. Later frees of it saturate at 0. */
void platform_unix_arena_forget(void) { arena_used = 0; }

void *platform_chunk_alloc(size_t bytes) {
    if (arena_cap && bytes > arena_cap - arena_used) return NULL;
    void *p = malloc(bytes);
    if (p) arena_used += bytes;
    return p;
}
void platform_chunk_free(void *p, size_t bytes) {
    if (!p) return;
    arena_used = bytes > arena_used ? 0 : arena_used - bytes;
    free(p);
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Test-only: replaces the clock (NULL = the real one), e.g. to count reads. */
static uint64_t (*clock_hook)(void);
void platform_unix_set_clock(uint64_t (*fn)(void)) { clock_hook = fn; }

static uint64_t time_start;
static int time_started;

uint64_t platform_now_ms(void) {
    if (clock_hook) return clock_hook();
    uint64_t now = mono_ms();
    if (!time_started) {
        time_start = now;
        time_started = 1;
    }
    return now - time_start;
}

void platform_time_reset(void) {
    time_start = mono_ms();
    time_started = 1;
}

void platform_barrier(void) { __sync_synchronize(); }

/* Wakeup latch: a self-pipe. platform_notify writes a byte (async-signal-
 * safe, so usable from a signal handler or another thread); platform_wait
 * polls the read end, so a notify before OR during a wait ends it at once,
 * and the byte persists until a wait consumes it. The pipe is created
 * before main (constructor), so platform_notify never has to allocate. */
static int wake_fd[2] = {-1, -1};

__attribute__((constructor)) static void wake_init(void) {
    if (pipe(wake_fd) != 0) {
        fputs("gogo: cannot create wakeup pipe\n", stderr);
        exit(1);
    }
    for (int i = 0; i < 2; i++) {
        fcntl(wake_fd[i], F_SETFL, fcntl(wake_fd[i], F_GETFL) | O_NONBLOCK);
        fcntl(wake_fd[i], F_SETFD, FD_CLOEXEC);
    }
}

void platform_notify(void) {
    char c = 1;
    int saved = errno;
    ssize_t r = write(wake_fd[1], &c, 1);   /* full pipe = already latched */
    (void)r;
    errno = saved;
}

/* Returns after the timeout or once notified; consumes the latch. */
void platform_wait(uint32_t ms) {
    if (gpio_unix_wait(ms)) {             /* an input line was consumed: rescan */
        char buf[64];
        while (read(wake_fd[0], buf, sizeof buf) > 0) {}
        return;
    }
    struct pollfd p[2];
    char buf[64];
    int n = 1;
    p[0].fd = wake_fd[0]; p[0].events = POLLIN; p[0].revents = 0;
    int sfd = gpio_unix_poll_fd();        /* tty stdin, or -1 */
    if (sfd >= 0) { p[1].fd = sfd; p[1].events = POLLIN; p[1].revents = 0; n = 2; }
    int rc = poll(p, (nfds_t)n, ms == UINT32_MAX ? -1 : (ms > 2000000000u ? 2000000000 : (int)ms));
    if (rc > 0 && n == 2 && p[1].revents) gpio_unix_read_tty();
    while (read(wake_fd[0], buf, sizeof buf) > 0) {}   /* clear the latch */
}

const struct PortDriver *const *platform_port_drivers(void) {
    static const struct PortDriver *const list[] = { &gpio_unix_driver, NULL };
    return list;
}
