/* The gogo network stack: Ethernet, ARP, IPv4, ICMP echo and
 * UDP over one RX and one TX frame buffer, driven from the scheduler's
 * drain step through the port layer's extra source (port_add_source). Pure
 * C99, chip-independent: it uses only vm/'s public interfaces and the
 * platform network interface (netif.h).
 *
 * Ports:
 *  - "netif", cfg net.IfConfig{}: one owner at a time (the DHCP actor);
 *    command net.SetAddr{IP, Mask, Gateway, DNS} (owner only; IP 0 clears
 *    the address). Closing it clears the address too.
 *  - "udp", cfg net.UDP{Port} (0 = an ephemeral port): command
 *    net.SendTo{Addr, Data}; event net.Datagram{From, Data} to the owner,
 *    the payload a heap K_BYTES inside the message block,
 *    delivered with msg_try_enqueue: over the owner's quota the datagram
 *    is dropped and counted, the owner is never killed. Delivery leaves
 *    room for one net.Up (and first collects a waiting owner's heap
 *    slack, as TCP does). A Call reply to an owner whose mailbox net data
 *    filled is an ordinary message and can still kill it (M2 rule).
 *  - net.Subscribe() (builtin): the caller gets net.Up{IP, Gateway, DNS}
 *    whenever the interface comes up (link up and an address set; again on
 *    an address change) and net.Down{} when it goes down, and net.Up at
 *    once if it is up already. At most GOGO_NET_MAX_SUBS live subscribers.
 *    A report that cannot be allocated, or does not fit the subscriber's
 *    quota, is retried (every 50 ms, with the state current then), so no
 *    subscriber misses an Up or Down for want of memory and none is killed
 *    by one.
 *
 *  - "tcp", cfg net.Dial{IP, Port, TimeoutMs}, and "tcp_listen", cfg
 *    net.Listen{Port}: TCP (tcp.c).
 *  - "tls", cfg net.DialTLS{IP, Port, TimeoutMs, Host}: TLS over a "tcp"
 *    connection, in runtime/net/tls/ (not linked here): a TCP connection marked
 *    TF_UPPER hands its stream to the one installed TcpUpper (net_int.h).
 *
 * Unconfigured (no address), the stack passes only UDP from local port 68
 * to remote port 67 (sent from 0.0.0.0 to 255.255.255.255) and UDP to
 * port 68 from port 67: what DHCP needs. Configured or not,
 * a BOOTP request from port 68 to 67 gets the interface MAC stamped into
 * chaddr (programs cannot know it), and BOOTP replies to port 68 for
 * another chaddr (or too short to have one) are dropped (ip_filtered).
 *
 * ARP: requests for our address are answered; senders of ARP packets for
 * us and of IP packets to our unicast address on our subnet are learned
 * into a GOGO_NET_ARP-entry cache (least recently used replaced, never the
 * configured gateway's). A packet
 * whose next hop is not in the cache is dropped (counted) and an ARP
 * request sent instead: the sender retries (setting an address pre-resolves
 * the gateway). Malformed frames of every layer are dropped and counted. */
#ifndef GOGO_NET_H
#define GOGO_NET_H

#include <stdint.h>
#include <stdbool.h>
#include "port.h"
#include "netif.h"

#ifndef GOGO_NET_MAX_UDP
#define GOGO_NET_MAX_UDP 4u
#endif
#ifndef GOGO_NET_MAX_TCP
#define GOGO_NET_MAX_TCP 4u
#endif
#ifndef GOGO_NET_MAX_LISTEN
#define GOGO_NET_MAX_LISTEN 2u
#endif
#ifndef GOGO_NET_MAX_SUBS
#define GOGO_NET_MAX_SUBS 4u
#endif
#ifndef GOGO_NET_ARP
#define GOGO_NET_ARP 4u
#endif
/* An orphan connection's whole life: reset when it is up. */
#ifndef GOGO_NET_ORPHAN_MS
#define GOGO_NET_ORPHAN_MS 20000u
#endif
#ifndef GOGO_NET_RX_BUFS
#define GOGO_NET_RX_BUFS 1u
#endif
/* Largest frame (no FCS) the RX and TX buffers hold. */
#define NET_FRAME_MAX 1536u
/* Budget for the whole stack's fixed state. */
#define NET_FIXED_LIMIT 4096u

/* Drop and traffic counters (each written by one side only). */
typedef struct NetStats {
    uint32_t rx_frames;     /* frames taken from the RX slot */
    uint32_t rx_busy;       /* offered while the slot was full (producer) */
    uint32_t rx_oversize;   /* offered longer than NET_FRAME_MAX (producer) */
    uint32_t eth_bad;       /* shorter than an Ethernet header */
    uint32_t eth_filtered;  /* not to our MAC or broadcast */
    uint32_t eth_unknown;   /* EtherType neither IPv4 nor ARP */
    uint32_t arp_bad;       /* short or not Ethernet/IPv4 ARP, bad op/sender */
    uint32_t arp_ignored;   /* not for us (or unconfigured) */
    uint32_t ip_bad;        /* short, version, lengths, header checksum */
    uint32_t ip_frag;       /* fragments (no reassembly) */
    uint32_t ip_filtered;   /* not to us, or not allowed unconfigured */
    uint32_t ip_proto;      /* unknown protocol */
    uint32_t icmp_bad;      /* short, checksum */
    uint32_t icmp_echo;     /* echo requests answered */
    uint32_t udp_bad;       /* short, length, checksum */
    uint32_t udp_noport;    /* no port bound to the destination port */
    uint32_t udp_quota;     /* not delivered: owner's quota or memory */
    uint32_t udp_rx;        /* datagrams delivered */
    uint32_t tx_frames;     /* frames sent */
    uint32_t tx_fail;       /* platform_net_send failed */
    uint32_t tx_arp_miss;   /* dropped: next hop not resolved (ARP sent) */
    uint32_t tx_noroute;    /* dropped: no address, no gateway, not allowed */
    uint32_t tx_bad;        /* malformed SendTo/Write/Shutdown, too large */
    uint32_t tcp_bad;       /* short, header length, checksum */
    uint32_t tcp_noconn;    /* no connection or listener: RST sent (or none for a RST) */
    uint32_t tcp_drop;      /* out of order, old, duplicate, unacceptable */
    uint32_t tcp_quota;     /* segment not delivered (owner's quota): not ACKed */
    uint32_t tcp_syn_drop;  /* SYN to a listener with no free connection slot */
    uint32_t tcp_retx;      /* retransmissions */
    uint32_t tcp_rx;        /* data segments delivered */
} NetStats;

/* The port drivers (platform_port_drivers lists them). */
extern const PortDriver net_netif_driver;
extern const PortDriver net_udp_driver;
extern const PortDriver net_tcp_driver;
extern const PortDriver net_tcp_listen_driver;

/* Starts the stack: resets its state, registers it as vm's extra port
 * source and reads the link state (platform_net_link_up). Call after
 * vm_boot (vm_init clears the source list) and before vm_run on a platform
 * with a network; without it the ports refuse to open and received frames
 * are dropped. */
void net_attach(struct Vm *vm);
/* Unregisters it (tests). */
void net_detach(struct Vm *vm);

/* Bytes of fixed state (RX/TX buffers, tables, counters); <= NET_FIXED_LIMIT. */
uint32_t net_fixed_bytes(void);
const NetStats *net_stats(void);

#endif
