/* gogo_posix.h: the parts of the embedding API that need POSIX: the
 * default host (malloc, clocks, getentropy, stdout and stderr, a latched
 * condition-variable wait) and a VM run on a thread of its own. Include
 * it instead of, or after, gogo.h; libgogo.a has both. A host on an RTOS
 * or other system without POSIX uses gogo.h alone with a host table of
 * its own. */
#ifndef GOGO_POSIX_H
#define GOGO_POSIX_H

#include "gogo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The POSIX default host: malloc, CLOCK_MONOTONIC, CLOCK_REALTIME as the
 * wall clock (setting it is refused), getentropy (random aborts if it
 * fails), stdout/stderr, and a latched condition-variable wait and wake.
 * Its state lives in a context: gogo_posix_ctx_new, or gogo_new_posix,
 * which makes a VM that owns one (freed with the VM's last reference, as
 * a destroy hook would). */
GOGO_API const gogo_host *gogo_host_posix(void);
GOGO_API void *gogo_posix_ctx_new(void);         /* NULL if out of memory */
GOGO_API void gogo_posix_ctx_free(void *ctx);
GOGO_API int gogo_new_posix(const gogo_config *cfg, gogo_vm **out);   /* as gogo_new */

/* gogo_run on a new thread (POSIX threads): the VM then belongs to that
 * thread until gogo_join, which returns gogo_run's result. Meanwhile the
 * thread that started it may only call gogo_poster_new, gogo_join and
 * gogo_free (which stops the thread at its next step, joins it and frees
 * the VM); other threads post through posters. GOGO_EINVAL without
 * wait, GOGO_ESTATE if a thread is already running, GOGO_ENOMEM. */
GOGO_API GOGO_EXPERIMENTAL int gogo_run_thread(gogo_vm *vm);
GOGO_API GOGO_EXPERIMENTAL int gogo_join(gogo_vm *vm);

#ifdef __cplusplus
}
#endif

#endif
