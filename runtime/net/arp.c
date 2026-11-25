/* ARP (RFC 826) for Ethernet/IPv4: answers requests for our address and
 * keeps a GOGO_NET_ARP-entry cache, least recently used replaced. */
#include "net_int.h"
#include <string.h>

#define ARP_LEN 28u

static ArpEntry *arp_find(NetState *ns, uint32_t ip) {
    for (uint32_t i = 0; i < GOGO_NET_ARP; i++) {
        if (ns->arp[i].ip == ip) return &ns->arp[i];
    }
    return NULL;
}

/* Records ip -> mac: updates an existing entry; with add, makes one
 * (a free entry or the least recently used, never the configured
 * gateway's: busy local peers must not push out the one everything
 * routed goes through). */
void arp_learn(NetState *ns, uint32_t ip, const uint8_t mac[6], bool add) {
    if (ip == 0 || ip == IP_BCAST || (mac[0] & 1u)) return;
    ArpEntry *e = arp_find(ns, ip);
    if (!e) {
        if (!add) return;
        for (uint32_t i = 0; i < GOGO_NET_ARP; i++) {
            ArpEntry *c = &ns->arp[i];
            if (!c->ip) { e = c; break; }
            if (ns->gw && c->ip == ns->gw) continue;
            if (!e || ns->arp_clock - c->used > ns->arp_clock - e->used) e = c;
        }
        if (!e) return;              /* GOGO_NET_ARP 1, holding the gateway */
        e->ip = ip;
    }
    memcpy(e->mac, mac, 6);
    e->used = ++ns->arp_clock;
}

/* Builds an ARP packet in the TX buffer and sends it to eth dst. */
static void arp_send(NetState *ns, uint16_t op, const uint8_t *dst, const uint8_t *tha, uint32_t tpa) {
    uint8_t *p = ns->tx + ETH_HLEN;
    wr16(p, 1);                      /* Ethernet */
    wr16(p + 2, ETH_IPV4);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, ns->mac, 6);
    wr32(p + 14, ns->ip);
    memcpy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    eth_send(ns, dst, ETH_ARP, ARP_LEN);
}

void arp_request(NetState *ns, uint32_t ip) {
    static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    arp_send(ns, 1, net_bcast_mac, zero, ip);
}

const uint8_t *arp_resolve(NetState *ns, uint32_t ip) {
    ArpEntry *e = arp_find(ns, ip);
    if (e && ip) {
        e->used = ++ns->arp_clock;
        return e->mac;
    }
    arp_request(ns, ip);
    return NULL;
}

void arp_rx(NetState *ns, const uint8_t *p, uint32_t len) {
    if (len < ARP_LEN || rd16(p) != 1 || rd16(p + 2) != ETH_IPV4 || p[4] != 6 || p[5] != 4) {
        ns->st.arp_bad++;
        return;
    }
    uint16_t op = rd16(p + 6);
    const uint8_t *sha = p + 8;
    uint32_t spa = rd32(p + 14), tpa = rd32(p + 24);
    if ((op != 1 && op != 2) || (sha[0] & 1u)) {
        ns->st.arp_bad++;
        return;
    }
    if (!ip_configured(ns) || tpa != ns->ip) {
        /* Not for us: only refresh what we already know (RFC 826). */
        if (ip_configured(ns)) arp_learn(ns, spa, sha, false);
        ns->st.arp_ignored++;
        return;
    }
    arp_learn(ns, spa, sha, ip_on_link(ns, spa));
    if (op == 1) arp_send(ns, 2, sha, sha, spa);
}
