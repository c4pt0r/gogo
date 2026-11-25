/* Board network interface: what the net stack (net/) needs from a board
 * (VmBoard.netif, host.h), and the two entry points a board calls into the
 * stack. Raw Ethernet frames, no FCS.
 *
 * Receive is a single-slot handoff, not the gpio event ring: the platform's
 * receive path (the only producer per stack) calls net_rx_offer with the
 * stack it feeds (the NetState the board gave net_install), which copies
 * the frame into that stack's RX buffer if its ready flag is clear (else
 * drops and counts it), sets the flag with a release barrier and wakes that
 * stack's VM (vm_wake); the scheduler (the only consumer) drains it through
 * the port layer's extra source (port_add_source). Link changes go through
 * net_link_event the same way (latest state wins). Before net_install (or
 * after net_uninstall) both drop what they are given. */
#ifndef GOGO_NETIF_H
#define GOGO_NETIF_H

#include <stdint.h>
#include <stdbool.h>

/* One stack instance (net_int.h); the board owns its storage. */
typedef struct NetState NetState;

/* ---- provided by the board (every hook gets ctx) ---- */
typedef struct NetIf {
    /* The interface's MAC address; false if the board has no network. */
    bool (*mac)(void *ctx, uint8_t mac[6]);
    /* Sends one frame synchronously (the caller's buffer is free on
     * return). 0 on success, -1 if it could not be sent (no link, error). */
    int (*send)(void *ctx, const uint8_t *frame, uint32_t len);
    /* Current link state (polled once at stack start; changes arrive
     * through net_link_event). */
    bool (*link_up)(void *ctx);
    /* The interface went up (an address on a live link; again when the
     * address changes) or down: the stack calls this from the scheduler
     * (the VM task) with the same values its net.Up / net.Down carry, for
     * a status line. ip, gw, dns: IPv4, host order; 0 when down. */
    void (*report)(void *ctx, bool up, uint32_t ip, uint32_t gw, uint32_t dns);
    void *ctx;
} NetIf;

/* ---- provided by the net stack ---- */

/* A received frame for stack ns (any context the platform receives in;
 * copies it). */
void net_rx_offer(NetState *ns, const uint8_t *frame, uint32_t len);

/* Stack ns's link went up or down. */
void net_link_event(NetState *ns, bool up);

#endif
