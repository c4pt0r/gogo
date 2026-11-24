#define _POSIX_C_SOURCE 199309L   /* clock_gettime, nanosleep */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

void platform_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
/* Flush program output first so diagnostics interleave in order. */
void (*platform_unix_err_hook)(const char *, size_t);  /* test-only capture */
void platform_write_err(const char *s, size_t n) {
    if (platform_unix_err_hook) { platform_unix_err_hook(s, n); return; }
    fflush(stdout);
    fwrite(s, 1, n, stderr);
}

void *platform_chunk_alloc(size_t bytes) { return malloc(bytes); }
void platform_chunk_free(void *p, size_t bytes) { (void)bytes; free(p); }

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t platform_now_ms(void) {
    static uint64_t start;
    static int started;
    uint64_t now = mono_ms();
    if (!started) {
        start = now;
        started = 1;
    }
    return now - start;
}

void platform_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}   /* resume after a signal */
}
