/* Unix TLS hooks' extras (platform/unix/tls_unix.c). */
#ifndef GOGO_TLS_UNIX_H
#define GOGO_TLS_UNIX_H
#include <stddef.h>
/* Bytes platform_tls_alloc has outstanding now, and their peak. */
void platform_unix_tls_heap(size_t *now, size_t *peak);
#endif
