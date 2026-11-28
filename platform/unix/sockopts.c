/* hostnet's socket options (sockopts.h). */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "sockopts.h"
#include "net_keepalive.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* Seconds for the kernel: at least 1. */
static int secs(unsigned long ms) { return ms < 1000u ? 1 : (int)(ms / 1000u); }

void hostnet_sock_opts(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (!GOGO_NET_KEEPALIVE_IDLE_MS) return;
    int idle = secs(GOGO_NET_KEEPALIVE_IDLE_MS), intvl = secs(GOGO_NET_KEEPALIVE_INTVL_MS),
        cnt = (int)GOGO_NET_KEEPALIVE_PROBES;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#if defined(TCP_KEEPIDLE)
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#elif defined(TCP_KEEPALIVE)
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);   /* macOS */
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
#endif
#ifdef TCP_KEEPCNT
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
    (void)idle; (void)intvl; (void)cnt;
}
