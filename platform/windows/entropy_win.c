/* entropy_win.c: getentropy for the Windows port (platform/windows/include/
 * sys/random.h), which platform/unix/tls_unix.c uses for the TLS port's
 * random hook. BCryptGenRandom is the system CSPRNG; no seeding to do.
 */
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>

int getentropy(void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) {
        ULONG n = len > 256u ? 256u : (ULONG)len;
        if (BCryptGenRandom(NULL, (PUCHAR)p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}
