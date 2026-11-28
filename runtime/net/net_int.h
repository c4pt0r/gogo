/* net/ internals shared by its files (not a public interface). */
#ifndef GOGO_NET_INT_H
#define GOGO_NET_INT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "net.h"
#include "actor.h"
#include "msg.h"
#include "stream.h"
#include "tls/tls.h"
#include "../include/gogo_errors.h"

#if (GOGO_NET_RX_BUFS) != 1u
#error "GOGO_NET_RX_BUFS: only 1 is implemented"
#endif

#define ETH_HLEN 14u
#define IP_HLEN 20u
#define UDP_HLEN 8u
#define ETH_IPV4 0x0800u
#define ETH_ARP 0x0806u
#define IP_ICMP 1u
#define IP_UDP 17u
#define IP_TCP 6u
#define TCP_HLEN 20u
/* Our MSS and receive window (one segment). */
#define TCP_MSS 1460u
#define IP_BCAST 0xFFFFFFFFu
/* Largest UDP payload: an IPv4 MTU of 1500. */
#define UDP_MAX_PAYLOAD (1500u - IP_HLEN - UDP_HLEN)
#define DHCP_CLIENT 68u
#define DHCP_SERVER 67u
#define BOOTP_CHADDR 28u   /* offset of chaddr in a BOOTP message */

typedef struct ArpEntry {
    uint32_t ip;            /* 0: free */
    uint32_t used;          /* NetState.arp_clock at the last use or update */
    uint8_t mac[6];
    uint8_t pad[2];
} ArpEntry;

/* A UDP port's control block (charged to the owner: UDP_PCB_WORDS). */
typedef struct UdpPcb {
    Port *port;             /* NULL: free */
    uint16_t lport;
    uint16_t pad;
    uint32_t drops;         /* datagrams not delivered (quota) */
} UdpPcb;

/* TCP states (with CLOSING, for a simultaneous close). */
enum {
    TCP_CLOSED = 0, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT
};

/* A TCP connection's control block, in the fixed table and charged to the
 * port's owner (TCP_PCB_WORDS). Sequence space: one segment in flight,
 * [snd_una, snd_nxt) = the SYN, or `seg` data bytes of the head Out (the
 * port's StreamOut; an orphan's wr[0]) from wr_off plus the FIN if TF_FIN
 * and not yet acknowledged. */
typedef struct TcpPcb {
    Port *port;             /* the "tcp" port; NULL: half-open, orphan or TIME_WAIT */
    uint32_t rip;           /* remote IP; state TCP_CLOSED: free */
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint32_t deadline;      /* timer, low 32 bits of vm_now (TF_TIMER) */
    uint32_t until;         /* TF_UNTIL: connect deadline (SYN_SENT), with an upper layer also its
                             * handshake's (until tcp_until_clear); an orphan's close deadline */
    uint32_t wr_off;        /* bytes of the head Out acknowledged (TF_UPPER: taken by the layer) */
    uint32_t rx_seq;        /* Seq of the last Chunk delivered */
    term wr[2];             /* an orphan's Outs left (message references), 0 none */
    uint16_t lport, rport;
    uint16_t mss;           /* the peer's MSS */
    uint16_t seg;           /* data bytes in flight */
    uint16_t wnd;           /* the peer's window */
    uint8_t state, flags;
    uint8_t retries;        /* retransmissions of what is in flight; with nothing
                             * in flight, keepalive probes sent */
    uint8_t listener;       /* SYN_RCVD: index of its listener */
    uint8_t stall;          /* the peer's data (or FIN) refused: STALL_* (tcp.c) */
} TcpPcb;

/* An upper layer over one TCP connection (runtime/net/tls/tls.c): a pcb with TF_UPPER
 * hands its stream to NetState.upper instead of to the port's owner. The
 * owner then gets only what the layer posts (tcp_post, tcp_chunk_send,
 * tcp_write_done) and the end of the connection (Closed from TCP: reset,
 * timeout, no route, ...). At most one
 * layer is installed per stack (NetState.upper); every pcb with TF_UPPER uses
 * it. The layer keeps its per-stack state in the stack's NetState (given, or
 * the Vm's vm->net). The callbacks run inside the stack (TCP input or the
 * drain): they must not free or end the connection themselves (only drain
 * may call tcp_abort).
 *   rx:        in-order bytes; true = taken (ACKed), false = refused (not
 *              ACKed: the peer resends);
 *   fin:       the peer's FIN after all rx; true = taken, false = refused;
 *   connected: the TCP handshake completed (TF_UNTIL stays set: TimeoutMs
 *              bounds the layer's handshake too, until tcp_until_clear);
 *   acked:     n bytes from the start of what tx offered were ACKed;
 *   tx:        the bytes to send (*len of them, possibly 0), stable in
 *              memory until acknowledged (acked);
 *   ended:     the connection is gone (any reason): free the layer's state;
 *              it posts nothing (TCP posts Closed if there is one to post);
 *   pending, drain, next_deadline: folded into the stack's port source
 *              (next_deadline may be NULL), under PortSource's rule
 *              (runtime/vm/port.h): pending only when drain will make
 *              progress now, time-gated work in next_deadline;
 *   closed:    true once the layer has posted the connection's terminal
 *              Closed (any reason but Normal) itself: TCP then posts none
 *              of its own when the connection ends (may be NULL).
 * Exactly one terminal Closed per connection: a layer posts its own only
 * while closed() is false, then reports true; a terminal Closed replaces a
 * Normal still held in the port (tcp_post). */
#define TF_UPPER 128u
typedef struct TcpUpper {
    const struct PortDriver *drv;  /* the layer's port driver (held events are retried for it) */
    bool (*rx)(Vm *vm, TcpPcb *c, const uint8_t *p, uint32_t n);
    bool (*fin)(Vm *vm, TcpPcb *c);
    void (*connected)(Vm *vm, TcpPcb *c);
    void (*acked)(Vm *vm, TcpPcb *c, uint32_t n);
    const uint8_t *(*tx)(const NetState *ns, const TcpPcb *c, uint32_t *len);
    void (*ended)(NetState *ns, TcpPcb *c);
    bool (*pending)(const NetState *ns);
    void (*drain)(Vm *vm);
    bool (*next_deadline)(const NetState *ns, uint64_t *deadline);
    bool (*closed)(const NetState *ns, const TcpPcb *c);
} TcpUpper;

typedef struct TcpListen {
    Port *port;             /* NULL: free */
    uint16_t lport;
    uint16_t pad;
} TcpListen;

/* One stack instance: everything the stack keeps. The board supplies the
 * storage (static on the ESP) and hands it to net_install; the stack reaches
 * it through vm->net (drivers, the source) or as a parameter (internals, the
 * board's net_rx_offer / net_link_event). */
struct NetState {
    uint8_t rx[NET_FRAME_MAX];
    uint8_t tx[NET_FRAME_MAX];
    /* RX slot: producer net_rx_offer, consumer the drain.
     * rx_len is written before rx_ready is set; the drain clears rx_ready
     * after the frame is processed. */
    volatile uint32_t rx_len;
    volatile uint8_t rx_ready;
    /* Link slot: latest state wins. */
    volatile uint8_t link_pending;
    volatile uint8_t link_state;
    volatile uint32_t down_seq;      /* Downs offered; written by the producer only */
    uint32_t down_seen;              /* Downs the drain has handled; consumer only */
    uint8_t attached;       /* a Vm with a netif that has a MAC */
    uint8_t link_up;        /* as last drained */
    uint8_t is_up;          /* as last reported to subscribers */
    uint8_t dirty;          /* Up/Down may need reporting */
    uint8_t mac[6];
    uint16_t ip_id;
    uint16_t eph;           /* next ephemeral UDP port */
    uint32_t ip, mask, gw, dns;
    uint32_t up_ip, up_gw, up_dns;   /* as last reported */
    uint32_t arp_clock;
    Port *netif;
    struct Vm *vm;          /* the attached Vm (net_install) */
    const NetIf *nif;       /* its board's netif */
    term subs[GOGO_NET_MAX_SUBS];
    ArpEntry arp[GOGO_NET_ARP];
    UdpPcb udp[GOGO_NET_MAX_UDP];
    TcpPcb tcp[GOGO_NET_MAX_TCP];
    TcpListen lis[GOGO_NET_MAX_LISTEN];
    uint32_t isn;           /* ISN counter */
    uint16_t tcp_eph;       /* next ephemeral TCP port */
    uint8_t accepting;      /* 1 + index of the connection being accepted, or 0 */
    uint8_t tcp_held;       /* some tcp port holds events, or a refused receive is
                             * watched (tcp.c HELD_*): retry at held_at */
    uint8_t sub_owed;       /* bit i: subs[i] is owed the current Up/Down (allocation failed) */
    uint8_t pad_sub;
    uint32_t held_at;
    uint32_t sub_retry_at;  /* when sub_owed is retried */
    const TcpUpper *upper;  /* the upper layer (TF_UPPER pcbs), or NULL */
    NetStats st;
    TlsState tls;           /* the "tls" port's connection and counters (tls.c) */
};

#define UDP_PCB_WORDS ((uint16_t)((sizeof(UdpPcb) + 3u) / 4u))
#define TCP_PCB_WORDS ((uint16_t)((sizeof(TcpPcb) + 3u) / 4u))
#define TCP_LISTEN_WORDS ((uint16_t)((sizeof(TcpListen) + 3u) / 4u))

/* The memory budgets hold at compile time, on any target. */
typedef char net_budget_fixed[sizeof(NetState) <= NET_FIXED_LIMIT ? 1 : -1];
typedef char net_budget_tcb[sizeof(TcpPcb) <= 64u ? 1 : -1];
typedef char net_budget_udp[sizeof(UdpPcb) <= 32u ? 1 : -1];

/* ---- byte order ---- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- eth.c ---- */
extern const uint8_t net_bcast_mac[6];
/* Sends ns->tx[0..ETH_HLEN + len) after filling in its Ethernet header. */
void eth_send(NetState *ns, const uint8_t dst[6], uint16_t type, uint32_t len);
void net_rx_frame(NetState *ns, const uint8_t *f, uint32_t len);

/* ---- arp.c ---- */
void arp_rx(NetState *ns, const uint8_t *p, uint32_t len);
/* The MAC for next hop ip, or NULL (an ARP request is then sent). */
const uint8_t *arp_resolve(NetState *ns, uint32_t ip);
void arp_learn(NetState *ns, uint32_t ip, const uint8_t mac[6], bool add);
void arp_request(NetState *ns, uint32_t ip);

/* ---- ip.c ---- */
uint32_t net_sum(uint32_t s, const uint8_t *p, uint32_t n);
uint16_t net_fold(uint32_t s);
bool ip_configured(const NetState *ns);
bool ip_on_link(const NetState *ns, uint32_t ip);
void ip_rx(NetState *ns, const uint8_t *f, uint32_t len);
/* Sends ns->tx's IPv4 payload (len bytes at ETH_HLEN + IP_HLEN) from src
 * to dst; next hop by route and ARP unless dst_mac is given. */
bool ip_send(NetState *ns, uint32_t src, uint32_t dst, uint8_t proto, uint32_t len, const uint8_t *dst_mac);

/* ---- icmp.c ---- */
void icmp_rx(NetState *ns, const uint8_t *eth_src, uint32_t src, const uint8_t *p, uint32_t len);

/* ---- udp.c ---- */
void udp_rx(NetState *ns, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t len);
void udp_reset(NetState *ns);

/* ---- tcp.c ---- */
void tcp_rx(NetState *ns, uint32_t src, uint32_t dst, const uint8_t *p, uint32_t len);
void tcp_reset(NetState *ns);
/* Timers: any due at now (cheap when none is set); fire the due ones;
 * the earliest deadline; orphans still closing (keep the VM running). */
bool tcp_due(const NetState *ns);
void tcp_poll(struct Vm *vm);
bool tcp_next_deadline(const NetState *ns, uint64_t *deadline);
bool tcp_any_orphan(const NetState *ns);
/* The interface went down: every connection ends (Closed{NoRoute}). */
void tcp_down(struct Vm *vm);

/* Close reasons (net.Normal ...) are the shared codes (gogo_errors.h):
 * GOGO_E_OK (Normal), _RESET, _TIMEOUT, _REFUSED and _NOROUTE; GOGO_E_NOTIME,
 * _BADCERT and _TLS are the tls port's. */
/* Headroom a Chunk delivery leaves for control events: two Dones, two
 * Closed (each a 4-word struct; a Seq from 2^30 up takes a 2-word box
 * more, and such an event is then held and retried instead). */
#define TCP_EVENT_RESERVE (4u * msg_footprint_words(4))

/* A "tcp" (or upper layer's) port's Outs: Port.state[0..3]. */
static inline StreamOut *tcp_stream(Port *p) { return (StreamOut *)(void *)p->state; }

/* For a driver built on the "tcp" port (an upper layer's):
 *  - tcp_dial: the active open of Dial{IP, Port, TimeoutMs} (cfg[0..3)) for
 *    port p, as "tcp" does (p charged TCP_PCB_WORDS); NULL if refused;
 *  - tcp_pcb_of / tcp_close_port / tcp_port_can_event: the "tcp" port's
 *    lookup, close and can_event;
 *  - tcp_post: Connected / Closed{From, reason} to the owner, held in
 *    the port (in order, owed Dones between them) while it has no room,
 *    never killing it;
 *  - tcp_flush: delivers p's held events and owed Dones; true when none is
 *    left (a Chunk goes out only after them);
 *  - tcp_hold_closed: Closed{reason} to p's owner soon (p has no
 *    connection: state[0] none);
 *  - tcp_output: sends what is ready (the layer's tx) if nothing is in
 *    flight; tcp_shutdown: FIN after it (Shutdown);
 *  - tcp_abort: ends c (RST if rst; Closed{reason} unless reason < 0);
 *  - tcp_until_clear: drops TimeoutMs's deadline (the layer's handshake is
 *    done);
 *  - tcp_out: an Out command (msg) into p's StreamOut, refused with
 *    Done{-1} when c is NULL (no connection, or the layer says no),
 *    after Shutdown, or by the stream rules; output is left to the
 *    caller; false if refused;
 *  - tcp_fail_outs: the connection is ending: the Outs in flight get
 *    Done{-1} (call it before posting the terminal Closed);
 *  - tcp_write_done: the head Out is fully sent (wr_off restarts at 0):
 *    released and answered with Done, the next one moved up;
 *  - tcp_chunk_new: a Chunk for c's port with room for n Data bytes at
 *    *data, Seq the next one, once p's held events are out (NULL: held
 *    events, no owner, no memory); tcp_chunk_send queues it leaving
 *    TCP_EVENT_RESERVE (counting its Seq) or frees it: false if it did
 *    not fit (the caller refuses the bytes). */
TcpPcb *tcp_dial(NetState *ns, Port *p, const int32_t *cfg, uint32_t n);
TcpPcb *tcp_pcb_of(const NetState *ns, const Port *p);
void tcp_close_port(struct Vm *vm, Port *p);
bool tcp_port_can_event(const struct Vm *vm, const Port *p);
void tcp_post(struct Vm *vm, const TcpPcb *c, uint32_t type, int32_t reason);
bool tcp_flush(struct Vm *vm, Port *p);
void tcp_hold_closed(NetState *ns, Port *p, int32_t reason);
void tcp_output(struct Vm *vm, TcpPcb *c);
void tcp_shutdown(struct Vm *vm, TcpPcb *c);
void tcp_abort(struct Vm *vm, TcpPcb *c, int32_t reason, bool rst);
void tcp_until_clear(const NetState *ns, TcpPcb *c);
bool tcp_out(struct Vm *vm, const Actor *from, Port *p, const TcpPcb *c, term msg);
void tcp_write_done(struct Vm *vm, TcpPcb *c);
void tcp_fail_outs(struct Vm *vm, const TcpPcb *c);
Msg *tcp_chunk_new(struct Vm *vm, const TcpPcb *c, uint32_t n, uint8_t **data);
bool tcp_chunk_send(struct Vm *vm, TcpPcb *c, Msg *m);

/* ---- ports.c ---- */
/* Puts int v at d[*at] boxed (K_INT) if it does not fit a small int;
 * returns its term. */
term net_put_int(uint32_t *d, uint32_t *at, int32_t v);
/* Words net_put_int takes for v beyond the field itself. */
static inline uint32_t net_int_words(int32_t v) { return term_int_fits(v) ? 0u : 2u; }
/* A message block of `words` words (root at 0), or NULL. */
Msg *net_msg_new(struct Vm *vm, uint32_t words);
/* Queues m to o (never killing it) if that leaves `reserve` words of o's
 * limit free, again once a waiting o's heap slack is collected; false
 * (m still the caller's) otherwise. */
bool net_deliver(Vm *vm, Actor *o, Msg *m, uint32_t reserve);
/* Whether o can take `words` more words leaving `reserve` free (a waiting
 * o's heap slack collected first if needed): net_deliver's check. */
bool net_room(Vm *vm, Actor *o, uint32_t words, uint32_t reserve);

#endif
