/* Platform network interface (spec M5 §5.1, §9.9): what the net stack
 * (net/) needs from a platform, and the two entry points a platform calls
 * into the stack. Raw Ethernet frames, no FCS.
 *
 * Receive is a single-slot handoff, not the gpio event ring: the platform's
 * receive path (the only producer) calls net_rx_offer, which copies the
 * frame into the stack's RX buffer if its ready flag is clear (else drops
 * and counts it), sets the flag with a release barrier and calls
 * platform_notify; the scheduler (the only consumer) drains it through the
 * port layer's extra source (port_set_source). Link changes go through
 * net_link_event the same way (latest state wins). */
#ifndef GOGO_NETIF_H
#define GOGO_NETIF_H

#include <stdint.h>
#include <stdbool.h>

/* ---- provided by the platform ---- */

/* The interface's MAC address; false if the platform has no network. */
bool platform_net_mac(uint8_t mac[6]);

/* Sends one frame synchronously (the caller's buffer is free on return).
 * 0 on success, -1 if it could not be sent (no link, error). */
int platform_net_send(const uint8_t *frame, uint32_t len);

/* Current link state (polled once at stack start; changes arrive through
 * net_link_event). */
bool platform_net_link_up(void);

/* The interface went up (an address on a live link; again when the
 * address changes) or down: the stack calls this from the scheduler
 * (the VM task) with the same values its net.Up / net.Down carry, for a
 * platform status line. ip, gw, dns: IPv4, host order; 0 when down. */
void platform_net_report(bool up, uint32_t ip, uint32_t gw, uint32_t dns);

/* ---- provided by the net stack ---- */

/* A received frame (any context the platform receives in; copies it). */
void net_rx_offer(const uint8_t *frame, uint32_t len);

/* The link went up or down. */
void net_link_event(bool up);

#endif
