/* ARP (RFC 826) for Ethernet/IPv4: answers requests for our address and
 * keeps a GOGO_NET_ARP-entry cache, least recently used replaced. */
#include "net_int.h"
#include <string.h>

#define ARP_LEN 28u

static ArpEntry *arp_find(uint32_t ip) {
    for (uint32_t i = 0; i < GOGO_NET_ARP; i++) {
        if (net_s.arp[i].ip == ip) return &net_s.arp[i];
    }
    return NULL;
}

/* Records ip -> mac: updates an existing entry; with add, makes one
 * (a free entry or the least recently used, never the configured
 * gateway's: busy local peers must not push out the one everything
 * routed goes through). */
void arp_learn(uint32_t ip, const uint8_t mac[6], bool add) {
    if (ip == 0 || ip == IP_BCAST || (mac[0] & 1u)) return;
    ArpEntry *e = arp_find(ip);
    if (!e) {
        if (!add) return;
        for (uint32_t i = 0; i < GOGO_NET_ARP; i++) {
            ArpEntry *c = &net_s.arp[i];
            if (!c->ip) { e = c; break; }
            if (net_s.gw && c->ip == net_s.gw) continue;
            if (!e || net_s.arp_clock - c->used > net_s.arp_clock - e->used) e = c;
        }
        if (!e) return;              /* GOGO_NET_ARP 1, holding the gateway */
        e->ip = ip;
    }
    memcpy(e->mac, mac, 6);
    e->used = ++net_s.arp_clock;
}

/* Builds an ARP packet in the TX buffer and sends it to eth dst. */
static void arp_send(uint16_t op, const uint8_t *dst, const uint8_t *tha, uint32_t tpa) {
    uint8_t *p = net_s.tx + ETH_HLEN;
    wr16(p, 1);                      /* Ethernet */
    wr16(p + 2, ETH_IPV4);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, net_s.mac, 6);
    wr32(p + 14, net_s.ip);
    memcpy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    eth_send(dst, ETH_ARP, ARP_LEN);
}

void arp_request(uint32_t ip) {
    static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    arp_send(1, net_bcast_mac, zero, ip);
}

const uint8_t *arp_resolve(uint32_t ip) {
    ArpEntry *e = arp_find(ip);
    if (e && ip) {
        e->used = ++net_s.arp_clock;
        return e->mac;
    }
    arp_request(ip);
    return NULL;
}

void arp_rx(const uint8_t *p, uint32_t len) {
    if (len < ARP_LEN || rd16(p) != 1 || rd16(p + 2) != ETH_IPV4 || p[4] != 6 || p[5] != 4) {
        net_s.st.arp_bad++;
        return;
    }
    uint16_t op = rd16(p + 6);
    const uint8_t *sha = p + 8;
    uint32_t spa = rd32(p + 14), tpa = rd32(p + 24);
    if ((op != 1 && op != 2) || (sha[0] & 1u)) {
        net_s.st.arp_bad++;
        return;
    }
    if (!ip_configured() || tpa != net_s.ip) {
        /* Not for us: only refresh what we already know (RFC 826). */
        if (ip_configured()) arp_learn(spa, sha, false);
        net_s.st.arp_ignored++;
        return;
    }
    arp_learn(spa, sha, ip_on_link(spa));
    if (op == 1) arp_send(2, sha, sha, spa);
}
