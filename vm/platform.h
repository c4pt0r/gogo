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
 * so this is time since VM start). */
uint64_t platform_now_ms(void);

/* Idle wait. Called by the scheduler when nothing is runnable: returns after
 * timeout_ms, or earlier when an event may have been pushed to the event
 * ring (spurious early returns are fine; the scheduler re-checks).
 * UINT32_MAX means no timeout.
 *
 * Latched wakeup (required): any platform_notify() since the previous
 * platform_wait returned makes the next platform_wait return immediately,
 * and a notify during a wait ends it. The scheduler drains the event ring
 * and then waits; an event pushed in that gap must not be slept through. */
void platform_wait(uint32_t timeout_ms);

/* Called by event producers after evq_push. ISR-safe on MCUs. Latches a
 * pending wakeup for platform_wait. */
void platform_notify(void);

/* Memory barrier ordering the event ring's slot and index accesses between
 * interrupt and scheduler context. */
void platform_barrier(void);

/* The port drivers this platform provides (port.h), NULL-terminated. */
struct PortDriver;
const struct PortDriver *const *platform_port_drivers(void);

#endif
