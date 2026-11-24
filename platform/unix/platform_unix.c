#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

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
