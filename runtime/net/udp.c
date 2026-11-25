/* UDP and the "udp" port. */
#include "net_int.h"
#include "heap.h"
#include "bytes.h"
#include "opcodes.h"
#include <string.h>

#define EPH_FIRST 49152u
/* Headroom delivery leaves: the largest Up (Up{IP, Gateway, DNS}, each boxed). */
#define UP_RESERVE msg_footprint_words(11)

void udp_reset(void) {
    memset(net_s.udp, 0, sizeof net_s.udp);
    net_s.eph = EPH_FIRST;
}

static UdpPcb *pcb_of(const Port *p) {
    uint32_t i = p->state[0];
    return i < GOGO_NET_MAX_UDP && net_s.udp[i].port == p ? &net_s.udp[i] : NULL;
}

static bool port_used(uint32_t lport) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_UDP; i++) {
        if (net_s.udp[i].port && net_s.udp[i].lport == lport) return true;
    }
    return false;
}

static int u_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)index;
    if (!net_s.attached || n != 1u || cfg[0] < 0 || cfg[0] > 65535) return -1;
    UdpPcb *pcb = NULL;
    uint32_t i;
    for (i = 0; i < GOGO_NET_MAX_UDP; i++) {
        if (!net_s.udp[i].port) { pcb = &net_s.udp[i]; break; }
    }
    if (!pcb) return -1;
    uint32_t lport = (uint32_t)cfg[0];
    if (lport == 0) {
        /* At most GOGO_NET_MAX_UDP ports are taken, so this ends quickly. */
        do {
            lport = net_s.eph;
            net_s.eph = (uint16_t)(net_s.eph == 65535u ? EPH_FIRST : net_s.eph + 1u);
        } while (port_used(lport));
    } else if (port_used(lport)) {
        return -1;
    }
    memset(pcb, 0, sizeof *pcb);
    pcb->port = p;
    pcb->lport = (uint16_t)lport;
    p->state[0] = i;
    p->charge_words = UDP_PCB_WORDS;
    return 0;
}

static void u_close(struct Vm *vm, Port *p) {
    (void)vm;
    UdpPcb *pcb = pcb_of(p);
    if (pcb) memset(pcb, 0, sizeof *pcb);
}

static bool u_can_event(const Port *p) { (void)p; return true; }

/* SendTo{Addr{IP, Port}, Data}: the payload goes straight from the
 * sender's Bytes into the TX buffer. */
static void u_command(Vm *vm, const Actor *from, Port *p, term msg) {
    UdpPcb *pcb = pcb_of(p);
    const uint32_t *o = obj_of(vm, from, msg);
    if (!pcb || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_NET_SENDTO || hdr_words(o[0]) != 4u) {
        net_s.st.tx_bad++;
        return;
    }
    const uint32_t *ad = obj_of(vm, from, o[2]);
    int32_t ip, rport;
    const uint8_t *data;
    uint32_t len;
    if (!ad || hdr_kind(ad[0]) != K_STRUCT || ad[1] != TYPE_NET_ADDR || hdr_words(ad[0]) != 4u ||
        !int_val(vm, from, ad[2], &ip) || !int_val(vm, from, ad[3], &rport) ||
        rport <= 0 || rport > 65535 || !bytes_view(vm, from, o[3], &data, &len) ||
        len > UDP_MAX_PAYLOAD) {
        net_s.st.tx_bad++;
        return;
    }
    uint32_t dst = (uint32_t)ip, src = net_s.ip;
    if (dst == 0 || (dst >> 28) == 0xEu) {   /* no multicast; 0.0.0.0 is no destination */
        net_s.st.tx_bad++;
        return;
    }
    if (!ip_configured() && !(pcb->lport == DHCP_CLIENT && rport == (int32_t)DHCP_SERVER && dst == IP_BCAST)) {
        net_s.st.tx_noroute++;
        return;
    }
    uint8_t *u = net_s.tx + ETH_HLEN + IP_HLEN;
    wr16(u, pcb->lport);
    wr16(u + 2, (uint32_t)rport);
    wr16(u + 4, UDP_HLEN + len);
    wr16(u + 6, 0);
    if (len) memcpy(u + UDP_HLEN, data, len);
    /* A BOOTP request from the DHCP client: the interface MAC goes into
     * chaddr (programs do not know it). */
    if (pcb->lport == DHCP_CLIENT && rport == (int32_t)DHCP_SERVER && len >= BOOTP_CHADDR + 6u && data[0] == 1u)
        memcpy(u + UDP_HLEN + BOOTP_CHADDR, net_s.mac, 6);
    uint8_t ph[12];
    wr32(ph, src);
    wr32(ph + 4, dst);
    wr16(ph + 8, IP_UDP);
    wr16(ph + 10, UDP_HLEN + len);
    uint16_t c = (uint16_t)~net_fold(net_sum(net_sum(0, ph, 12), u, UDP_HLEN + len));
    wr16(u + 6, c ? c : 0xFFFFu);
    ip_send(src, dst, IP_UDP, UDP_HLEN + len, NULL);
}

/* Datagram{From: Addr{IP, Port}, Data} as a message block:
 * [0..3] Datagram, [4..7] Addr, then the IP's K_INT box if it needs one,
 * then the payload as a heap K_BYTES (none if empty). */
static void deliver(Vm *vm, UdpPcb *pcb, uint32_t src, uint32_t sport, const uint8_t *data, uint32_t len) {
    Actor *o = port_owner(vm, pcb->port);
    int32_t ip = (int32_t)src;
    uint32_t words = 8u + net_int_words(ip) + (len ? 2u + (len + 3u) / 4u : 0u);
    Msg *m = o ? net_msg_new(vm, words) : NULL;
    if (!m) {
        pcb->drops++;
        net_s.st.udp_quota++;
        return;
    }
    uint32_t *d = m->data, at = 8;
    d[0] = hdr_make(4, K_STRUCT);
    d[1] = TYPE_NET_DATAGRAM;
    d[2] = term_from_ref(4);
    d[4] = hdr_make(4, K_STRUCT);
    d[5] = TYPE_NET_ADDR;
    d[6] = net_put_int(d, &at, ip);
    d[7] = term_from_int((int32_t)sport);
    if (len) {
        uint32_t bw = 2u + (len + 3u) / 4u;
        d[at] = hdr_make(bw, K_BYTES);
        d[at + 1] = len;
        d[at + bw - 1u] = 0;             /* padding */
        memcpy(&d[at + 2], data, len);
        d[3] = term_from_ref(at);
    } else {
        d[3] = T_EMPTYBYTES;
    }
    /* Leave room for one Up, so the stack's own next message never fails
     * for want of it. */
    if (net_deliver(vm, o, m, UP_RESERVE)) {
        net_s.st.udp_rx++;
        return;
    }
    msg_free(vm, m);
    pcb->drops++;
    net_s.st.udp_quota++;
}

void udp_rx(Vm *vm, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t len) {
    uint32_t ul = len >= UDP_HLEN ? rd16(p + 4) : 0u;
    if (ul < UDP_HLEN || ul > len) {
        net_s.st.udp_bad++;
        return;
    }
    if (rd16(p + 6)) {
        uint8_t ph[12];
        wr32(ph, src);
        wr32(ph + 4, dst);
        wr16(ph + 8, IP_UDP);
        wr16(ph + 10, ul);
        if (net_fold(net_sum(net_sum(0, ph, 12), p, ul)) != 0xFFFFu) {
            net_s.st.udp_bad++;
            return;
        }
    }
    uint32_t sport = rd16(p), dport = rd16(p + 2);
    bool dhcp = dport == DHCP_CLIENT && sport == DHCP_SERVER;
    /* Without an address only DHCP passes; DHCP replies only for our
     * chaddr. */
    if ((!ip_configured() && !dhcp) ||
        (dhcp && (ul < UDP_HLEN + BOOTP_CHADDR + 6u || memcmp(p + UDP_HLEN + BOOTP_CHADDR, net_s.mac, 6) != 0))) {
        net_s.st.ip_filtered++;
        return;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_UDP; i++) {
        UdpPcb *pcb = &net_s.udp[i];
        if (pcb->port && pcb->lport == dport) {
            deliver(vm, pcb, src, sport, p + UDP_HLEN, ul - UDP_HLEN);
            return;
        }
    }
    net_s.st.udp_noport++;
}

const PortDriver net_udp_driver = {
    "udp", TYPE_NET_UDP, TYPE_NET_DATAGRAM, u_open, NULL, u_close, u_can_event, NULL, u_command, NULL
};
