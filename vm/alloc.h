/* Small-block allocator: size classes of 8 bytes up to 256, carved from
 * 4 KB chunks; larger requests go straight to the platform. No per-block
 * header, so callers must pass the original size to va_free. */
#ifndef GOGO_ALLOC_H
#define GOGO_ALLOC_H

#include <stddef.h>

void *va_alloc(size_t bytes);              /* NULL on failure or over limit */
void va_free(void *p, size_t bytes);
size_t va_used_bytes(void);                /* handed out, rounded to class size */
void va_set_limit(size_t bytes);           /* 0 = unlimited (for OOM tests) */
void va_reset(void);                       /* tests only: forget all state */

#endif
