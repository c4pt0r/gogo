/* unix net link (--net-fd / --sim-clock, platform_unix.c). */
#ifndef GOGO_NET_UNIX_H
#define GOGO_NET_UNIX_H

#include <stdint.h>
#include <stdbool.h>

/* Link message types: [u16 len, big-endian][u8 type][len bytes]. */
enum {
    NET_MSG_FRAME = 0,     /* a raw Ethernet frame (both ways) */
    NET_MSG_ADVANCE = 1,   /* simulator -> VM: u32 ms (big-endian) */
    NET_MSG_ACK = 2,       /* VM -> simulator: idle at the advanced time */
    NET_MSG_LINK = 3       /* simulator -> VM: u8, 1 = link up, 0 = down */
};

/* Uses fd as the link; with sim, platform_now_ms is the simulated clock
 * (starting at 0) that only advance messages move. */
void platform_unix_net_open(int fd, bool sim);

/* Tests: forget the link and the simulated clock; divert received frames
 * to fn instead of net_rx_offer (NULL = net_rx_offer); frames received. */
void platform_unix_net_close(void);
void platform_unix_net_set_sink(void (*fn)(const uint8_t *, uint32_t));
uint32_t platform_unix_net_rx_count(void);
/* Tests: frames sent go to fn instead of the link (NULL = the link); with
 * fn set, platform_net_mac and platform_net_link_up succeed. */
void platform_unix_net_set_tx(int (*fn)(const uint8_t *, uint32_t));

#endif
