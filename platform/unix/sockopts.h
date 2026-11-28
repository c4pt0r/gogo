/* Socket options of the host's TCP connections (hostnet.c): no Nagle,
 * and the kernel's keepalive at the MCU stack's timings
 * (runtime/net/net_keepalive.h), where the system has the per-socket
 * options (Linux: TCP_KEEPIDLE; macOS: TCP_KEEPALIVE; both TCP_KEEPINTVL
 * and TCP_KEEPCNT); elsewhere the system's own keepalive timings. With
 * GOGO_NET_KEEPALIVE_IDLE_MS 0 keepalive stays off. */
#ifndef GOGO_SOCKOPTS_H
#define GOGO_SOCKOPTS_H

/* Sets the options on connected (or connecting) socket fd. */
void hostnet_sock_opts(int fd);

#endif
