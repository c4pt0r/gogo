/* sys/random.h (Windows): just getentropy, which the shared
 * platform/unix/tls_unix.c includes this header for. Implemented in
 * platform/windows/entropy_win.c over BCryptGenRandom. */
#ifndef GOGO_WIN_SYS_RANDOM_H
#define GOGO_WIN_SYS_RANDOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills buf with len cryptographically random bytes; 0, or -1 on failure. */
int getentropy(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GOGO_WIN_SYS_RANDOM_H */
