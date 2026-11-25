/* Ethernet: the RX single-slot handoff, frame dispatch, send. */
#include "net_int.h"
#include "host.h"
#include <string.h>

const uint8_t net_bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Producer side (the platform's receive path, the only caller for ns). */
void net_rx_offer(NetState *ns, const uint8_t *frame, uint32_t len) {
    if (!ns->attached) return;
    if (len > NET_FRAME_MAX) {
        ns->st.rx_oversize++;
        return;
    }
    if (ns->rx_ready) {
        ns->st.rx_busy++;
        return;
    }
    memcpy(ns->rx, frame, len);
    ns->rx_len = len;
    vm_barrier();                    /* frame visible before the flag */
    ns->rx_ready = 1;
    vm_wake(ns->vm);
}

/* link_pending is one latch, so events between two drains collapse to the
 * last state; a Down must still be reported. down_seq counts Downs and has
 * one writer (here); the drain keeps its own copy, down_seen, and reports a
 * Down whenever they differ. Order: link_state is written first, then (a
 * barrier later) down_seq. The drain reads link_state, a barrier, down_seq,
 * and, if down_seq changed, link_state again and uses that value.
 *  - It sees an Up that followed a Down: the Down's increment is visible too
 *    (nothing is lost).
 *  - It sees a Down's state before its increment: it reports that Down; the
 *    increment later finds the interface down already (a no-op).
 *  - It read an old Up, then a whole Down completes before it reads down_seq:
 *    the re-read returns the Down's state (written before the increment it
 *    saw), so one Down is reported and no stale Up. */
void net_link_event(NetState *ns, bool up) {
    if (!ns->attached) return;
    ns->link_state = up ? 1u : 0u;
    vm_barrier();
    if (!up) {
        ns->down_seq++;
        vm_barrier();
    }
    ns->link_pending = 1;
    vm_wake(ns->vm);
}

void eth_send(NetState *ns, const uint8_t dst[6], uint16_t type, uint32_t len) {
    uint8_t *f = ns->tx;
    memcpy(f, dst, 6);
    memcpy(f + 6, ns->mac, 6);
    wr16(f + 12, type);
    if (ns->nif->send(ns->nif->ctx, f, ETH_HLEN + len) == 0) ns->st.tx_frames++;
    else ns->st.tx_fail++;
}

void net_rx_frame(NetState *ns, const uint8_t *f, uint32_t len) {
    ns->st.rx_frames++;
    if (len < ETH_HLEN) {
        ns->st.eth_bad++;
        return;
    }
    if (memcmp(f, ns->mac, 6) != 0 && memcmp(f, net_bcast_mac, 6) != 0) {
        ns->st.eth_filtered++;     /* another host's, or multicast */
        return;
    }
    if (f[6] & 1u) {                 /* a group (multicast/broadcast) source: forged */
        ns->st.eth_bad++;
        return;
    }
    switch (rd16(f + 12)) {
    case ETH_ARP:
        arp_rx(ns, f + ETH_HLEN, len - ETH_HLEN);
        break;
    case ETH_IPV4:
        ip_rx(ns, f, len);
        break;
    default:
        ns->st.eth_unknown++;
        break;
    }
}
