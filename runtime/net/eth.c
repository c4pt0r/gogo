/* Ethernet: the RX single-slot handoff, frame dispatch, send. */
#include "net_int.h"
#include "host.h"
#include <string.h>

const uint8_t net_bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Producer side (the platform's receive path, the only caller). */
void net_rx_offer(const uint8_t *frame, uint32_t len) {
    if (!net_s.attached) return;
    if (len > NET_FRAME_MAX) {
        net_s.st.rx_oversize++;
        return;
    }
    if (net_s.rx_ready) {
        net_s.st.rx_busy++;
        return;
    }
    memcpy(net_s.rx, frame, len);
    net_s.rx_len = len;
    vm_barrier();                    /* frame visible before the flag */
    net_s.rx_ready = 1;
    vm_wake(net_s.vm);
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
void net_link_event(bool up) {
    if (!net_s.attached) return;
    net_s.link_state = up ? 1u : 0u;
    vm_barrier();
    if (!up) {
        net_s.down_seq++;
        vm_barrier();
    }
    net_s.link_pending = 1;
    vm_wake(net_s.vm);
}

void eth_send(const uint8_t dst[6], uint16_t type, uint32_t len) {
    uint8_t *f = net_s.tx;
    memcpy(f, dst, 6);
    memcpy(f + 6, net_s.mac, 6);
    wr16(f + 12, type);
    if (net_s.nif->send(net_s.nif->ctx, f, ETH_HLEN + len) == 0) net_s.st.tx_frames++;
    else net_s.st.tx_fail++;
}

void net_rx_frame(struct Vm *vm, const uint8_t *f, uint32_t len) {
    net_s.st.rx_frames++;
    if (len < ETH_HLEN) {
        net_s.st.eth_bad++;
        return;
    }
    if (memcmp(f, net_s.mac, 6) != 0 && memcmp(f, net_bcast_mac, 6) != 0) {
        net_s.st.eth_filtered++;     /* another host's, or multicast */
        return;
    }
    if (f[6] & 1u) {                 /* a group (multicast/broadcast) source: forged */
        net_s.st.eth_bad++;
        return;
    }
    switch (rd16(f + 12)) {
    case ETH_ARP:
        arp_rx(f + ETH_HLEN, len - ETH_HLEN);
        break;
    case ETH_IPV4:
        ip_rx(vm, f, len);
        break;
    default:
        net_s.st.eth_unknown++;
        break;
    }
}
