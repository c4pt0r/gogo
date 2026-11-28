/* hostnet_sock_opts (platform/unix/sockopts.c): the options read back
 * with getsockopt on a fresh TCP socket. Built twice by make test
 * (test-sockopts): with the default keepalive timings, and with
 * GOGO_NET_KEEPALIVE_IDLE_MS 0 (keepalive left off). */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include "sockopts.h"
#include "net_keepalive.h"

static int fails;

static void expect(int fd, int level, int name, const char *what, int want) {
    int v = -1;
    socklen_t len = sizeof v;
    if (getsockopt(fd, level, name, &v, &len) != 0) {
        printf("  FAIL %s: getsockopt failed\n", what);
        fails++;
    } else if ((want != 0) != (v != 0) || (want > 1 && v != want)) {
        printf("  FAIL %s: %d, want %d\n", what, v, want);
        fails++;
    }
}

int main(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("  FAIL socket\n");
        return 1;
    }
    hostnet_sock_opts(fd);
    expect(fd, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY", 1);
    int on = GOGO_NET_KEEPALIVE_IDLE_MS != 0;
    expect(fd, SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE", on);
    if (on) {
#if defined(TCP_KEEPIDLE)
        expect(fd, IPPROTO_TCP, TCP_KEEPIDLE, "TCP_KEEPIDLE", (int)(GOGO_NET_KEEPALIVE_IDLE_MS / 1000u));
#elif defined(TCP_KEEPALIVE)
        expect(fd, IPPROTO_TCP, TCP_KEEPALIVE, "TCP_KEEPALIVE", (int)(GOGO_NET_KEEPALIVE_IDLE_MS / 1000u));
#endif
#ifdef TCP_KEEPINTVL
        expect(fd, IPPROTO_TCP, TCP_KEEPINTVL, "TCP_KEEPINTVL", (int)(GOGO_NET_KEEPALIVE_INTVL_MS / 1000u));
#endif
#ifdef TCP_KEEPCNT
        expect(fd, IPPROTO_TCP, TCP_KEEPCNT, "TCP_KEEPCNT", (int)GOGO_NET_KEEPALIVE_PROBES);
#endif
    }
    close(fd);
    printf("sockopts (keepalive %s): %s\n", on ? "on" : "off", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
