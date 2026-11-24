/* IPv4: receive checks and demultiplexing, send with routing. No options
 * sent, options received skipped, fragments dropped (no reassembly). */
#include "net_int.h"
#include <string.h>

uint32_t net_sum(uint32_t s, const uint8_t *p, uint32_t n) {
    uint32_t i = 0;
    for (; i + 1u < n; i += 2u) s += (uint32_t)((p[i] << 8) | p[i + 1u]);
    if (i < n) s += (uint32_t)p[i] << 8;
    return s;
}

uint16_t net_fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)s;
}

bool ip_configured(void) { return net_s.ip != 0; }

bool ip_on_link(uint32_t ip) {
    return ip_configured() && ((ip ^ net_s.ip) & net_s.mask) == 0;
}

/* The directed broadcast of our subnet (none for a /31 or /32). */
static bool subnet_bcast(uint32_t ip) {
    uint32_t m = net_s.mask;
    return ip_configured() && m && ~m > 1u && ip == (net_s.ip | ~m);
}

void ip_rx(struct Vm *vm, const uint8_t *f, uint32_t len) {
    const uint8_t *p = f + ETH_HLEN;
    len -= ETH_HLEN;
    if (len < IP_HLEN) {
        net_s.st.ip_bad++;
        return;
    }
    uint32_t hl = (p[0] & 0xFu) * 4u;
    if ((p[0] >> 4) != 4u || hl < IP_HLEN) {
        net_s.st.ip_bad++;
        return;
    }
    uint32_t tl = rd16(p + 2);
    if (tl < hl || tl > len || tl > 1500u || net_fold(net_sum(0, p, hl)) != 0xFFFFu) {
        net_s.st.ip_bad++;
        return;
    }
    if (rd16(p + 6) & 0x3FFFu) {     /* MF or a fragment offset */
        net_s.st.ip_frag++;
        return;
    }
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    if (src == IP_BCAST || (src >> 28) == 0xEu || (ip_configured() && (src == net_s.ip || subnet_bcast(src)))) {
        net_s.st.ip_filtered++;      /* no host sends from these: forged (or our own echoed) */
        return;
    }
    uint8_t proto = p[9];
    const uint8_t *pl = p + hl;
    uint32_t pn = tl - hl;
    if (!ip_configured()) {
        /* Only DHCP (UDP 67 -> 68, any destination: offers may be unicast). */
        if (proto != IP_UDP) {
            net_s.st.ip_filtered++;
            return;
        }
        udp_rx(vm, src, dst, pl, pn);
        return;
    }
    bool unicast = dst == net_s.ip;
    if (!unicast && dst != IP_BCAST && !subnet_bcast(dst)) {
        net_s.st.ip_filtered++;
        return;
    }
    if (unicast && ip_on_link(src)) arp_learn(src, f + 6, true);
    switch (proto) {
    case IP_ICMP:
        if (unicast) icmp_rx(f + 6, src, pl, pn);
        else net_s.st.ip_filtered++;
        break;
    case IP_UDP:
        udp_rx(vm, src, dst, pl, pn);
        break;
    case IP_TCP:
        if (unicast) tcp_rx(vm, src, dst, pl, pn);
        else net_s.st.ip_filtered++;
        break;
    default:
        net_s.st.ip_proto++;
        break;
    }
}

bool ip_send(uint32_t src, uint32_t dst, uint8_t proto, uint32_t len, const uint8_t *dst_mac) {
    if (!dst_mac) {
        if (dst == IP_BCAST || subnet_bcast(dst)) {
            dst_mac = net_bcast_mac;
        } else {
            uint32_t hop = ip_on_link(dst) ? dst : net_s.gw;
            if (!ip_configured() || hop == 0 || dst == net_s.ip) {
                net_s.st.tx_noroute++;
                return false;
            }
            dst_mac = arp_resolve(hop);
            if (!dst_mac) {
                net_s.st.tx_arp_miss++;
                return false;
            }
        }
    }
    uint8_t mac[6];
    memcpy(mac, dst_mac, 6);         /* arp_resolve's entry; the TX buffer is rewritten below */
    uint8_t *p = net_s.tx + ETH_HLEN;
    p[0] = 0x45;
    p[1] = 0;
    wr16(p + 2, IP_HLEN + len);
    wr16(p + 4, net_s.ip_id++);
    wr16(p + 6, 0x4000u);            /* DF */
    p[8] = 64;
    p[9] = proto;
    wr16(p + 10, 0);
    wr32(p + 12, src);
    wr32(p + 16, dst);
    wr16(p + 10, (uint16_t)~net_fold(net_sum(0, p, IP_HLEN)));
    eth_send(mac, ETH_IPV4, IP_HLEN + len);
    return true;
}
