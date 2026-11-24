/* Platform interface: the only I/O the VM core needs. Implemented per
 * target (e.g. platform/unix/). */
#ifndef GOGO_PLATFORM_H
#define GOGO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

void platform_write(const char *s, size_t n);      /* program output (stdout) */
void platform_write_err(const char *s, size_t n);  /* diagnostics (stderr) */

/* Raw memory for the allocator (unix: malloc/free). bytes is always the
 * size originally requested. Returns NULL on failure. */
void *platform_chunk_alloc(size_t bytes);
void platform_chunk_free(void *p, size_t bytes);

/* Monotonic milliseconds since the first call (vm_run calls it at start,
 * so this is time since VM start), and a sleep for timers. */
uint64_t platform_now_ms(void);
void platform_sleep_ms(uint32_t ms);

#endif
