/* The unix TLS hooks (platform/unix/tls_unix.c). */
#ifndef GOGO_TLS_UNIX_H
#define GOGO_TLS_UNIX_H
#include <stddef.h>
#include "tls.h"
/* gogovm's VmBoard.tls. */
extern const TlsHw unix_tls_hw;
/* Bytes unix_tls_hw.alloc has outstanding now, and their peak. */
void platform_unix_tls_heap(size_t *now, size_t *peak);
#endif
