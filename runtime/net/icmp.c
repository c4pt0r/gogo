/* ICMP: echo requests to our unicast address are answered; everything
 * else is ignored. */
#include "net_int.h"
#include <string.h>

void icmp_rx(NetState *ns, const uint8_t *eth_src, uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < 8u || net_fold(net_sum(0, p, len)) != 0xFFFFu) {
        ns->st.icmp_bad++;
        return;
    }
    if (p[0] != 8u || p[1] != 0u) return;          /* not an echo request */
    /* The reply goes back to the sender's MAC (the request's last hop). */
    uint8_t *q = ns->tx + ETH_HLEN + IP_HLEN;
    memcpy(q, p, len);
    q[0] = 0;
    wr16(q + 2, 0);
    wr16(q + 2, (uint16_t)~net_fold(net_sum(0, q, len)));
    ns->st.icmp_echo++;
    ip_send(ns, ns->ip, src, IP_ICMP, len, eth_src);
}
