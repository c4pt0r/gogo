/* TCP keepalive timings, shared by the MCU stack (tcp.c) and the host's
 * real sockets (platform/unix/sockopts.c). Plain macros: no other
 * includes, so a platform file can take them alone. */
#ifndef GOGO_NET_KEEPALIVE_H
#define GOGO_NET_KEEPALIVE_H

/* Keepalive (RFC 1122 4.2.3.6): a connection with nothing in flight that
 * has heard nothing from the peer for GOGO_NET_KEEPALIVE_IDLE_MS is probed
 * every GOGO_NET_KEEPALIVE_INTVL_MS; when GOGO_NET_KEEPALIVE_PROBES probes
 * go unanswered it is reset and its owner gets Closed{Timeout} (tcp.c).
 * IDLE_MS 0: no keepalive. */
#ifndef GOGO_NET_KEEPALIVE_IDLE_MS
#define GOGO_NET_KEEPALIVE_IDLE_MS 120000u
#endif
#ifndef GOGO_NET_KEEPALIVE_INTVL_MS
#define GOGO_NET_KEEPALIVE_INTVL_MS 10000u
#endif
#ifndef GOGO_NET_KEEPALIVE_PROBES
#define GOGO_NET_KEEPALIVE_PROBES 3u
#endif
#endif
