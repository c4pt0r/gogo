/* TCP and the "tcp" / "tcp_listen" ports (spec M5 §5.2, §5.3, §9.3, §9.5,
 * §9.6): one segment in flight each way, in-order receive only, data
 * acknowledged only once delivered into the owner's mailbox.
 *
 * Control blocks live in NetState.tcp (GOGO_NET_MAX_TCP of them), charged to
 * the port's owner. A half-open passive connection (SYN_RCVD) takes a slot
 * with no port; completing the handshake opens a paused "tcp" port for the
 * listener's owner and sends it Accepted{Conn}. A port whose connection
 * has ended keeps no slot: its commands are ignored until it is closed.
 * Inbound connections (TF_INBOUND) never take the last free slot: a SYN
 * that would is dropped (the peer retries), so the device's own Dial
 * always finds one (a free slot, else a TIME_WAIT or half-open one).
 *
 * Timers are the stack's own: one deadline per connection (retransmission,
 * connect timeout, keepalive, TIME_WAIT, an orphan's FIN_WAIT_2 limit and
 * absolute lifetime), reported to the scheduler through the port source's
 * next_deadline and fired from its drain (tcp_poll). Not the VM timer heap.
 *
 * Keepalive (RFC 1122 4.2.3.6): a connection in ESTABLISHED, CLOSE_WAIT or
 * (not an orphan) FIN_WAIT_2 with nothing in flight has its timer at
 * GOGO_NET_KEEPALIVE_IDLE_MS after the last acceptable segment from the
 * peer (or the last ACK of our data); then it sends a probe (an ACK with
 * seq snd_nxt - 1, which a live peer answers) every
 * GOGO_NET_KEEPALIVE_INTVL_MS, counted in `retries`, and after
 * GOGO_NET_KEEPALIVE_PROBES unanswered ones resets the connection:
 * Closed{Timeout}, as when retransmissions run out. Sending data hands the
 * timer to retransmission; everything acknowledged hands it back. While
 * the peer's data is being refused (the owner's quota: `stall`) nothing
 * the peer sends counts as an answer, so an owner that never drains its
 * mailbox loses the connection after the same time; one that has room
 * again when the timer fires gets a fresh idle period (the peer's next
 * retransmission will be taken).
 *
 * The port speaks the stream protocol (runtime/stream/stream.h): received
 * bytes go to the owner as port.Chunk{From, Seq, TimeMs, Dropped 0, Data},
 * port.Out{Data} sends, each Out answered by port.Done{From, Seq} once the
 * peer acknowledged all of it, and port.Closed{From, Reason} ends it. The
 * Outs live in the port's StreamOut (Port.state[0..3]): pinned on the
 * owner's behalf (a handle) or copied into a port-held handle (heap or
 * const Bytes), at most two in flight (a third, or one after Shutdown or
 * once the connection is gone, gets Done{-1}), and sent from there, the
 * first time and on every retransmission. When the connection ends, the
 * Outs still in flight get Done{-1} before the Closed. Closing a port
 * after Shutdown (also by its owner's exit) leaves an orphan that
 * finishes the close: it holds its unacknowledged data by message
 * references (bytes_msg_ref, in TcpPcb.wr), charged to no one, until
 * acknowledged, for at most GOGO_NET_ORPHAN_MS from the close: then it is
 * reset (RST) whatever its progress, so a peer that acknowledges a byte
 * at a time cannot keep it alive. Closing it otherwise sends RST.
 *
 * Nothing TCP sends its owner can kill it: every event goes
 * through msg_try_enqueue. Data is refused (and not acknowledged) unless
 * it leaves EVENT_RESERVE words of headroom (an owner waiting in Receive
 * is first collected without growth slack, heap_reclaim_waiting, since it
 * cannot collect itself until a message comes) for the control events that
 * may follow it; a control event that still does not fit is held in the
 * port (Connected and a Closed reason in StreamOut.user, Dones owed in
 * the StreamOut), delivered in order (Connected, Dones, Closed) before
 * anything else and retried every HELD_RETRY_MS until the owner has room.
 * While Connected or a Closed is held (StreamOut.user) the stream owes
 * every Done instead of sending it, and no data goes out while Connected
 * is held, so no Done overtakes the Connected. A refusal is answered at
 * once, so its Done{-1} may come before the Dones of Outs in flight. */
#include "net_int.h"
#include "heap.h"
#include "bytes.h"
#include "opcodes.h"
#include "host.h"
#include "signals.h"
#include "stream.h"
#include <string.h>

#define TH_FIN 0x01u
#define TH_SYN 0x02u
#define TH_RST 0x04u
#define TH_PSH 0x08u
#define TH_ACK 0x10u

#define TF_TIMER 1u         /* deadline set */
#define TF_UNTIL 2u         /* a deadline in `until`: connect (and an upper layer's handshake), or an orphan's close */
#define TF_PAUSED 4u        /* accepted, not yet handed off or resumed */
#define TF_SHUT 8u          /* Shutdown: FIN after the pending writes */
#define TF_FIN 16u          /* FIN sent (snd_nxt counts it) */
#define TF_ORPHAN 32u       /* port closed after Shutdown: wr[] by message references */
#define TF_INBOUND 64u      /* a passive open (a listener's) */

#define RTO_MS 1000u        /* initial; doubles up to 8 s */
#define RTO_MAX_SHIFT 3u
#define MAX_RETRIES 6u
#define TIME_WAIT_MS 2000u
#define ORPHAN_FW2_MS 4000u /* an orphan waiting for the peer's FIN */
#define DEFAULT_MSS 536u
#define EPH_FIRST 49152u
#define HELD_RETRY_MS 50u
/* StreamOut.user: the held events, Connected and 1 + a Closed reason. */
#define HELD_CONN 1u
#define HELD_CL_SHIFT 1u
/* A held reason + 1 must fit the 7 bits above HELD_CONN: every shared code
 * does (C99: a negative array size fails the build otherwise). */
typedef char tcp_held_reason_fits[((GOGO_E_SHARED_MAX + 1u) << HELD_CL_SHIFT) <= 0xFFu ? 1 : -1];
#define EVENT_RESERVE TCP_EVENT_RESERVE

/* The refusal check: an Out after Shutdown or with no connection. */
static bool refuse(const Port *p, const uint8_t *d, uint32_t len) { (void)p; (void)d; (void)len; return false; }

/* A received segment. */
typedef struct Seg {
    uint32_t seq, ack;
    const uint8_t *p;       /* payload */
    uint32_t n;             /* payload bytes */
    uint16_t wnd, mss;      /* mss: the option's value, 0 if none */
    uint8_t fl;
} Seg;

static uint32_t now32(const NetState *ns) { return (uint32_t)vm_now(ns->vm); }
static bool due(uint32_t t, uint32_t now) { return (int32_t)(now - t) >= 0; }

void tcp_reset(NetState *ns) {
    memset(ns->tcp, 0, sizeof ns->tcp);
    memset(ns->lis, 0, sizeof ns->lis);
    ns->tcp_eph = EPH_FIRST;
    ns->accepting = 0;
}

/* ---- timers ---- */

static uint32_t rto(const TcpPcb *c) {
    return RTO_MS << (c->retries < RTO_MAX_SHIFT ? c->retries : RTO_MAX_SHIFT);
}

/* TF_UNTIL (a connect deadline, or an orphan's end) caps every timer. */
static void timer_set(const NetState *ns, TcpPcb *c, uint32_t ms) {
    c->deadline = now32(ns) + ms;
    c->flags |= TF_TIMER;
    if ((c->flags & TF_UNTIL) && (int32_t)(c->deadline - c->until) > 0) c->deadline = c->until;
}

/* With TF_UNTIL the timer stays (or is) set, at `until`. */
static void timer_stop(TcpPcb *c) {
    if (c->flags & TF_UNTIL) {
        c->deadline = c->until;
        c->flags |= TF_TIMER;
    } else {
        c->flags &= (uint8_t)~TF_TIMER;
    }
}

/* Whether c is probed when idle: the states in which the peer may just
 * be quiet (an orphan's FIN_WAIT_2 has its own limit). */
static bool keepalive(const TcpPcb *c) {
    return GOGO_NET_KEEPALIVE_IDLE_MS && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT ||
                                          (c->state == TCP_FIN_WAIT_2 && !(c->flags & TF_ORPHAN)));
}

/* Nothing in flight any more: the keepalive's idle period, else no timer. */
static void idle(const NetState *ns, TcpPcb *c) {
    if (keepalive(c)) {
        c->retries = 0;
        timer_set(ns, c, GOGO_NET_KEEPALIVE_IDLE_MS);
    } else {
        timer_stop(c);
    }
}

/* The peer is alive: the idle period restarts (not while its data is
 * refused, nor with data in flight: the retransmission timer's). */
static void heard(const NetState *ns, TcpPcb *c) {
    if (keepalive(c) && !c->stall && c->snd_una == c->snd_nxt) idle(ns, c);
}

/* ---- output ---- */

static uint16_t tcp_sum(uint32_t src, uint32_t dst, const uint8_t *t, uint32_t len) {
    uint8_t ph[12];
    wr32(ph, src);
    wr32(ph + 4, dst);
    wr16(ph + 8, IP_TCP);
    wr16(ph + 10, len);
    return net_fold(net_sum(net_sum(0, ph, 12), t, len));
}

/* Sends a segment from lport to rip:rport (the SYN flag adds our MSS). */
static void out(NetState *ns, uint32_t rip, uint32_t lport, uint32_t rport, uint32_t seq, uint32_t ack, uint8_t fl,
                uint32_t wnd, const uint8_t *p, uint32_t len) {
    uint8_t *t = ns->tx + ETH_HLEN + IP_HLEN;
    uint32_t hl = (fl & TH_SYN) ? TCP_HLEN + 4u : TCP_HLEN;
    wr16(t, lport);
    wr16(t + 2, rport);
    wr32(t + 4, seq);
    wr32(t + 8, ack);
    t[12] = (uint8_t)(hl << 2);
    t[13] = fl;
    wr16(t + 14, wnd);
    wr32(t + 16, 0);                 /* checksum, urgent pointer */
    if (fl & TH_SYN) {
        t[20] = 2;
        t[21] = 4;
        wr16(t + 22, TCP_MSS);
    }
    if (len) memcpy(t + hl, p, len);
    wr16(t + 16, (uint16_t)~tcp_sum(ns->ip, rip, t, hl + len));
    ip_send(ns, ns->ip, rip, IP_TCP, hl + len, NULL);
}

static void seg_out(NetState *ns, const TcpPcb *c, uint32_t seq, uint8_t fl, const uint8_t *p, uint32_t len) {
    out(ns, c->rip, c->lport, c->rport, seq, (fl & TH_ACK) ? c->rcv_nxt : 0u, fl,
        (c->flags & TF_PAUSED) ? 0u : TCP_MSS, p, len);
}

static void send_ack(NetState *ns, const TcpPcb *c) { seg_out(ns, c, c->snd_nxt, TH_ACK, NULL, 0); }
static void send_rst(NetState *ns, const TcpPcb *c) { seg_out(ns, c, c->snd_nxt, TH_RST | TH_ACK, NULL, 0); }

/* The Out being sent (*len bytes) and how many are in flight: an orphan's
 * from wr[], else the port's StreamOut. */
static const uint8_t *head(Vm *vm, const TcpPcb *c, uint32_t *len, uint32_t *n_out) {
    *len = 0;
    if (c->flags & TF_ORPHAN) {
        *n_out = (c->wr[0] ? 1u : 0u) + (c->wr[1] ? 1u : 0u);
        return c->wr[0] ? bytes_data(vm, c->wr[0], len) : NULL;
    }
    *n_out = c->port ? tcp_stream(c->port)->n : 0u;
    return c->port ? stream_out_head(vm, tcp_stream(c->port), len) : NULL;
}

/* The bytes to send: the head Out from wr_off, or the upper layer's. */
static const uint8_t *tx_view(Vm *vm, const TcpPcb *c, uint32_t *len) {
    const NetState *ns = vm->net;
    if (c->flags & TF_UPPER) return ns->upper->tx(ns, c, len);
    uint32_t n, k;
    const uint8_t *d = head(vm, c, &n, &k);
    if (!d || c->wr_off > n) {
        *len = 0;
        return NULL;
    }
    *len = n - c->wr_off;
    return d + c->wr_off;
}

static bool fin_in_flight(const TcpPcb *c) {
    return (c->flags & TF_FIN) && c->snd_nxt - c->snd_una == (uint32_t)c->seg + 1u;
}

/* (Re)sends the segment in flight. */
static void xmit(Vm *vm, const TcpPcb *c) {
    NetState *ns = vm->net;
    switch (c->state) {
    case TCP_SYN_SENT:
        seg_out(ns, c, c->snd_una, TH_SYN, NULL, 0);
        return;
    case TCP_SYN_RCVD:
        seg_out(ns, c, c->snd_una, TH_SYN | TH_ACK, NULL, 0);
        return;
    default: {
        uint8_t fl = TH_ACK;
        const uint8_t *d = NULL;
        uint32_t len = 0;
        if (c->seg) {
            d = tx_view(vm, c, &len);
            if (!d || c->seg > len) return;   /* cannot happen */
            fl |= TH_PSH;
        }
        if (fin_in_flight(c)) fl |= TH_FIN;
        seg_out(ns, c, c->snd_una, fl, d, c->seg);
        return;
    }
    }
}

/* Sends the next segment (data and/or FIN) if none is in flight. */
static void output(Vm *vm, TcpPcb *c) {
    const NetState *ns = vm->net;
    if (c->snd_nxt != c->snd_una || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)) return;
    if (!(c->flags & TF_UPPER) && c->port && (tcp_stream(c->port)->user & HELD_CONN)) return;   /* Connected first */
    uint32_t left, hl, k = 0;
    (void)tx_view(vm, c, &left);
    if (!(c->flags & TF_UPPER)) (void)head(vm, c, &hl, &k);
    uint32_t n = left;
    if (n > c->mss) n = c->mss;
    if (n > c->wnd) n = c->wnd ? c->wnd : 1u;    /* zero window: a 1-byte probe */
    bool fin = (c->flags & TF_SHUT) && n == left && k <= 1u;   /* nothing queued behind */
    if (!n && !fin) return;
    c->seg = (uint16_t)n;
    if (fin) {
        c->flags |= TF_FIN;
        c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
    }
    c->snd_nxt = c->snd_una + n + (fin ? 1u : 0u);
    c->retries = 0;
    xmit(vm, c);
    timer_set(ns, c, rto(c));
}

/* ---- events to the owner ---- */

static Actor *owner(Vm *vm, const TcpPcb *c) { return c->port ? port_owner(vm, c->port) : NULL; }

void tcp_output(Vm *vm, TcpPcb *c) { output(vm, c); }

/* Queues Connected{} (type TYPE_NET_CONNECTED) or Closed{From, Reason} on
 * p's owner if it fits (never fatal). */
static bool post(Vm *vm, const Port *p, uint32_t type, int32_t reason) {
    int32_t v[2] = { (int32_t)p->id, reason };
    return port_deliver_ints(vm, p, type, v, type == TYPE_CLOSED ? 2u : 0u, PORT_RAW(0));
}

/* Delivers p's held events in order; true when none is left. */
static bool flush(Vm *vm, Port *p) {
    StreamOut *o = tcp_stream(p);
    if (o->user & HELD_CONN) {
        if (!post(vm, p, TYPE_NET_CONNECTED, 0)) return false;
        o->user = (uint8_t)(o->user & ~HELD_CONN);
    }
    if (!stream_out_flush(vm, p, o)) return false;
    if (o->user >> HELD_CL_SHIFT) {
        if (!post(vm, p, TYPE_CLOSED, (int32_t)(o->user >> HELD_CL_SHIFT) - 1)) return false;
        o->user = 0;
    }
    return true;
}

static bool held(const Port *p) {
    const StreamOut *o = (const StreamOut *)(const void *)p->state;
    return o->user || o->owed_n;
}

bool tcp_flush(Vm *vm, Port *p) { return flush(vm, p); }

static void retry_later(NetState *ns) {
    ns->tcp_held = 1;
    ns->held_at = now32(ns) + HELD_RETRY_MS;
}

/* Delivers p's held events now, else retries them later. */
static void settle(Vm *vm, Port *p) {
    if (!flush(vm, p)) retry_later(vm->net);
}

/* Control events (Connected, Closed other than the peer's FIN): held in
 * the port, after any already held, and delivered when they fit. */
void tcp_post(Vm *vm, const TcpPcb *c, uint32_t type, int32_t reason) {
    Port *p = c->port;
    if (!p) return;
    StreamOut *o = tcp_stream(p);
    uint32_t cl = (uint32_t)o->user >> HELD_CL_SHIFT;
    if (type == TYPE_NET_CONNECTED) o->user = (uint8_t)(o->user | HELD_CONN);
    else if (!cl || (cl == GOGO_E_OK + 1u && reason != GOGO_E_OK)) {   /* the first, or a terminal one after a held Normal */
        o->user = (uint8_t)((o->user & HELD_CONN) | (uint32_t)(reason + 1) << HELD_CL_SHIFT);
    }
    settle(vm, p);
}

/* An upper layer's connection: Connected is the layer's to send, and its
 * own terminal Closed stands for the connection's end. */
static void event(Vm *vm, TcpPcb *c, uint32_t type, int32_t reason) {
    const NetState *ns = vm->net;
    if (c->flags & TF_UPPER) {
        if (type == TYPE_NET_CONNECTED) {
            ns->upper->connected(vm, c);
            return;
        }
        if (type == TYPE_CLOSED && ns->upper->closed && ns->upper->closed(ns, c)) return;
    }
    tcp_post(vm, c, type, reason);
}

void tcp_hold_closed(NetState *ns, Port *p, int32_t reason) {
    tcp_stream(p)->user = (uint8_t)((uint32_t)(reason + 1) << HELD_CL_SHIFT);   /* and no connection */
    ns->tcp_held = 1;
    ns->held_at = now32(ns);
}

/* The peer's FIN's Closed{Normal}: acknowledged only once queued, so not
 * held; true if queued, or if there is no port (orphan). */
static bool try_event(Vm *vm, const TcpPcb *c, uint32_t type, int32_t reason) {
    if (!c->port) return true;
    return flush(vm, c->port) && post(vm, c->port, type, reason);
}

static uint32_t next_seq(const TcpPcb *c) { return c->rx_seq >= 0x7FFFFFFFu ? 1u : c->rx_seq + 1u; }

Msg *tcp_chunk_new(Vm *vm, const TcpPcb *c, uint32_t n, uint8_t **data) {
    return owner(vm, c) && flush(vm, c->port) ?
        stream_chunk_new(vm, c->port->id, next_seq(c), (uint32_t)vm_now(vm), 0, n, data) : NULL;
}

bool tcp_chunk_send(Vm *vm, TcpPcb *c, Msg *m) {
    Actor *o = owner(vm, c);
    if (o && net_deliver(vm, o, m, EVENT_RESERVE)) {
        c->rx_seq = next_seq(c);
        return true;
    }
    msg_free(vm, m);
    return false;
}

/* The payload as a Chunk (its Data a heap K_BYTES in the block). An
 * orphan discards it. */
static bool deliver(Vm *vm, TcpPcb *c, const uint8_t *p, uint32_t n) {
    if (!c->port) return true;
    if (c->flags & TF_UPPER) return vm->net->upper->rx(vm, c, p, n);
    uint8_t *w;
    Msg *m = tcp_chunk_new(vm, c, n, &w);
    if (!m) return false;
    memcpy(w, p, n);
    return tcp_chunk_send(vm, c, m);
}

/* Whether c's owner could take a full segment now (a refused receive
 * may go on). */
static bool owner_room(Vm *vm, const TcpPcb *c) {
    Actor *o = owner(vm, c);
    return o && net_room(vm, o, msg_footprint_words(8u + TCP_MSS / 4u), EVENT_RESERVE);
}

/* ---- control blocks ---- */

TcpPcb *tcp_pcb_of(const NetState *ns, const Port *p) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (ns->tcp[i].state != TCP_CLOSED && ns->tcp[i].port == p) return (TcpPcb *)&ns->tcp[i];
    }
    return NULL;
}

/* Releases an orphan's wr[k] (its message reference). */
static void drop_data(Vm *vm, TcpPcb *c, uint32_t k) {
    term h = c->wr[k];
    c->wr[k] = 0;
    if (term_is_handle(h)) bytes_msg_unref(vm, h);
}

void tcp_fail_outs(Vm *vm, const TcpPcb *c) {
    if (!c->port) return;
    StreamOut *o = tcp_stream(c->port);
    if (!o->n) return;
    while (stream_out_fail(vm, c->port, o)) {}
    settle(vm, c->port);
}

/* Releases c's data and detaches its port, which stays open with no
 * connection (its held events still go out) and no longer charged. */
static void detach(Vm *vm, TcpPcb *c) {
    if (c->flags & TF_UPPER) {
        c->flags &= (uint8_t)~TF_UPPER;
        vm->net->upper->ended(vm->net, c);
    }
    tcp_fail_outs(vm, c);
    drop_data(vm, c, 0);
    drop_data(vm, c, 1);
    if (c->port) {
        Actor *o = actor_lookup(vm, c->port->owner);
        if (o) actor_uncharge(o, c->port->charge_words);
        c->port->charge_words = 0;
    }
    c->port = NULL;
}

/* Frees the slot. */
static void pcb_free(Vm *vm, TcpPcb *c) {
    detach(vm, c);
    memset(c, 0, sizeof *c);
}

/* Ends the connection: RST if asked, Closed{reason} to the owner. */
static void abort_conn(Vm *vm, TcpPcb *c, int32_t reason, bool rst) {
    if (rst) send_rst(vm->net, c);
    tcp_fail_outs(vm, c);            /* their Dones before the Closed */
    if (reason >= 0) event(vm, c, TYPE_CLOSED, reason);
    pcb_free(vm, c);
}

void tcp_abort(Vm *vm, TcpPcb *c, int32_t reason, bool rst) { abort_conn(vm, c, reason, rst); }

void tcp_until_clear(const NetState *ns, TcpPcb *c) {
    c->flags &= (uint8_t)~TF_UNTIL;
    if (c->snd_una == c->snd_nxt) idle(ns, c);
    else timer_set(ns, c, rto(c));
}

/* A free slot, else the oldest TIME_WAIT's, else (for an active open) a
 * half-open connection's (reset); NULL if none. */
static TcpPcb *pcb_alloc(NetState *ns, bool dial) {
    TcpPcb *tw = NULL, *half = NULL;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &ns->tcp[i];
        if (c->state == TCP_CLOSED) return c;
        if (c->state == TCP_TIME_WAIT && (!tw || (int32_t)(c->deadline - tw->deadline) < 0)) tw = c;
        if (c->state == TCP_SYN_RCVD && (c->flags & TF_INBOUND) && !c->port && !half) half = c;
    }
    TcpPcb *v = tw ? tw : dial ? half : NULL;
    if (!v) return NULL;
    pcb_free(ns->vm, v);             /* neither holds data or a port; no RST (the
                                      * peer's next segment gets one) */
    return v;
}

/* Clock-driven (RFC 793) plus a counter stirred by our MAC and the
 * traffic seen so far: not a secret, only harder to guess blindly. */
static uint32_t isn(NetState *ns) {
    ns->isn = ns->isn * 1103515245u + 12345u + ns->st.rx_frames + rd32(ns->mac + 2);
    return now32(ns) * 250u + ns->isn;
}

static bool lport_used(const NetState *ns, uint32_t lport) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (ns->tcp[i].state != TCP_CLOSED && ns->tcp[i].lport == lport) return true;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        if (ns->lis[i].port && ns->lis[i].lport == lport) return true;
    }
    return false;
}

/* ---- the "tcp" port ---- */

/* The active open of Dial{IP, Port, TimeoutMs}. */
TcpPcb *tcp_dial(NetState *ns, Port *p, const int32_t *cfg, uint32_t n) {
    if (!ns || !ns->attached || n != 3u || !ip_configured(ns) || cfg[0] == 0 || (uint32_t)cfg[0] == ns->ip ||
        (uint32_t)cfg[0] == IP_BCAST || ((uint32_t)cfg[0] >> 28) == 0xEu ||
        cfg[1] <= 0 || cfg[1] > 65535 || cfg[2] < 0) return NULL;
    uint32_t rip = (uint32_t)cfg[0];
    if (!ns->link_up || (!ip_on_link(ns, rip) && !ns->gw)) return NULL;   /* no route */
    TcpPcb *c = pcb_alloc(ns, true);
    if (!c) return NULL;
    uint32_t lport;
    do {
        lport = ns->tcp_eph;
        ns->tcp_eph = (uint16_t)(ns->tcp_eph == 65535u ? EPH_FIRST : ns->tcp_eph + 1u);
    } while (lport_used(ns, lport));
    c->state = TCP_SYN_SENT;
    c->rip = rip;
    c->lport = (uint16_t)lport;
    c->rport = (uint16_t)cfg[1];
    c->snd_una = isn(ns);
    c->mss = DEFAULT_MSS;
    if (cfg[2]) {
        c->until = now32(ns) + (uint32_t)cfg[2];
        c->flags |= TF_UNTIL;
    }
    c->snd_nxt = c->snd_una;     /* the SYN goes out at the next poll, */
    timer_set(ns, c, 0);             /* once the port layer has accepted the open */
    c->port = p;
    p->charge_words = TCP_PCB_WORDS;
    return c;
}

/* Dial{IP, Port, TimeoutMs}; or, with no fields, the connection being
 * accepted (NetState.accepting, set by accept() below). */
static int t_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    NetState *ns = vm->net;
    (void)index;
    if (!ns) return GOGO_E_UNSUPPORTED;   /* a VM without a network (gogovm without --net-fd) */
    if (n != 0u) return tcp_dial(ns, p, cfg, n) ? 0 : -1;
    if (!ns || !ns->accepting) return -1;
    TcpPcb *c = &ns->tcp[ns->accepting - 1u];
    c->port = p;
    p->charge_words = TCP_PCB_WORDS;
    return 0;
}

/* The port layer releases the pins after this. */
void tcp_close_port(Vm *vm, Port *p) {
    NetState *ns = vm->net;
    TcpPcb *c = tcp_pcb_of(ns, p);
    if (!c) return;
    switch (c->state) {
    case TCP_SYN_RCVD:               /* accept failed: stays half-open */
        c->port = NULL;
        return;
    case TCP_ESTABLISHED: case TCP_CLOSE_WAIT: case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2: case TCP_CLOSING: case TCP_LAST_ACK:
        if (c->flags & TF_SHUT) {
            /* Orphan: it finishes the FIN exchange alone, holding the
             * Outs left by message references (the port layer releases
             * its pins after this). */
            const StreamOut *o = tcp_stream(p);
            for (uint32_t k = 0; k < o->n; k++) {
                c->wr[k] = o->held[k];
                bytes_msg_ref(vm, c->wr[k]);
            }
            c->flags = (uint8_t)((c->flags | TF_ORPHAN | TF_UNTIL) & ~TF_PAUSED);
            c->port = NULL;
            c->until = now32(ns) + GOGO_NET_ORPHAN_MS;
            if (c->state == TCP_FIN_WAIT_2) timer_set(ns, c, ORPHAN_FW2_MS);
            else if (!(c->flags & TF_TIMER) || (int32_t)(c->deadline - c->until) > 0) timer_stop(c);
            return;
        }
        send_rst(ns, c);
        break;
    default:
        break;
    }
    memset(c, 0, sizeof *c);         /* the port layer releases the Outs' data */
}

bool tcp_port_can_event(const struct Vm *vm, const Port *p) { return tcp_pcb_of(vm->net, p) != NULL || held(p); }

static void unpause(NetState *ns, TcpPcb *c) {
    if (!(c->flags & TF_PAUSED)) return;
    c->flags &= (uint8_t)~TF_PAUSED;
    send_ack(ns, c);                     /* window update */
}

static void t_handoff(Vm *vm, Port *p) {
    NetState *ns = vm->net;
    TcpPcb *c = tcp_pcb_of(ns, p);
    if (c) unpause(ns, c);
}

void tcp_write_done(Vm *vm, TcpPcb *c) {
    c->wr_off = 0;
    if (c->flags & TF_ORPHAN) {
        drop_data(vm, c, 0);
        c->wr[0] = c->wr[1];
        c->wr[1] = 0;
    } else if (c->port) {
        (void)stream_out_done(vm, c->port, tcp_stream(c->port));
        settle(vm, c->port);
    }
}

bool tcp_out(Vm *vm, const Actor *from, Port *p, const TcpPcb *c, term msg) {
    bool ok = c && !(c->flags & TF_SHUT) &&
              (c->state == TCP_SYN_SENT || c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT);
    int r = stream_out_accept(vm, from, p, tcp_stream(p), msg, ok ? NULL : refuse);
    settle(vm, p);
    return r >= 0;
}

void tcp_shutdown(Vm *vm, TcpPcb *c) {
    c->flags |= TF_SHUT;
    output(vm, c);
}

/* Out{Data}, Shutdown{}, Resume{}. */
static void t_command(Vm *vm, const Actor *from, Port *p, term msg) {
    NetState *ns = vm->net;
    TcpPcb *c = tcp_pcb_of(ns, p);
    const uint32_t *o = obj_of(vm, from, msg);
    if (!o || hdr_kind(o[0]) != K_STRUCT) {
        ns->st.tx_bad++;
        return;
    }
    if (o[1] == TYPE_OUT) {          /* refused (Done{-1}) once the connection has ended */
        if (tcp_out(vm, from, p, c, msg) && c) output(vm, c);
        return;
    }
    if (!c) return;                  /* the connection has ended */
    switch (o[1]) {
    case TYPE_NET_SHUTDOWN:
        if (hdr_words(o[0]) != 2u) break;
        tcp_shutdown(vm, c);
        return;
    case TYPE_NET_RESUME:
        if (hdr_words(o[0]) != 2u) break;
        unpause(ns, c);
        return;
    default:
        break;
    }
    ns->st.tx_bad++;
}

const PortDriver net_tcp_driver = {
    .name = PORT_NAME_TCP, .cfg_type = TYPE_NET_DIAL, .event_type = TYPE_CHUNK, .open = t_open,
    .close = tcp_close_port, .can_event = tcp_port_can_event, .handoff = t_handoff, .command_term = t_command
};

/* ---- the "tcp_listen" port ---- */

static int l_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    NetState *ns = vm->net;
    (void)index;
    if (!ns) return GOGO_E_UNSUPPORTED;   /* a VM without a network */
    if (!ns->attached || n != 1u || cfg[0] <= 0 || cfg[0] > 65535) return -1;
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        if (ns->lis[i].port && ns->lis[i].lport == (uint32_t)cfg[0]) return -1;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        TcpListen *l = &ns->lis[i];
        if (l->port) continue;
        l->port = p;
        l->lport = (uint16_t)cfg[0];
        p->state[0] = i;
        p->charge_words = TCP_LISTEN_WORDS;
        return 0;
    }
    return -1;
}

/* Resets its half-open connections; accepted ones are their own ports. */
static void l_close(struct Vm *vm, Port *p) {
    NetState *ns = vm->net;
    uint32_t i = p->state[0];
    if (i >= GOGO_NET_MAX_LISTEN || ns->lis[i].port != p) return;
    for (uint32_t k = 0; k < GOGO_NET_MAX_TCP; k++) {
        TcpPcb *c = &ns->tcp[k];
        if (c->state == TCP_SYN_RCVD && c->listener == i) {
            send_rst(ns, c);
            memset(c, 0, sizeof *c);
        }
    }
    memset(&ns->lis[i], 0, sizeof ns->lis[i]);
}

static bool l_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return true; }

const PortDriver net_tcp_listen_driver = {
    .name = PORT_NAME_TCP_LISTEN, .cfg_type = TYPE_NET_LISTEN, .event_type = TYPE_NET_ACCEPTED,
    .open = l_open, .close = l_close, .can_event = l_can_event
};

/* The handshake completed: opens a paused "tcp" port for the listener's
 * owner and sends it Accepted{Conn}. False (the connection stays half-open
 * and retries on its SYN-ACK retransmission) if the owner cannot take the
 * port or the message. */
static bool accept(Vm *vm, TcpPcb *c) {
    NetState *ns = vm->net;
    const TcpListen *l = &ns->lis[c->listener];
    Actor *o = l->port ? port_owner(vm, l->port) : NULL;
    if (!o) return false;
    ns->accepting = (uint8_t)(c - ns->tcp + 1);
    term pid = port_open_with(vm, o, &net_tcp_driver, NULL, 0);
    ns->accepting = 0;
    if (pid == T_NIL) return false;
    Msg *m = net_msg_new(vm, 3);
    if (m) {
        m->data[0] = hdr_make(3, K_STRUCT);
        m->data[1] = TYPE_NET_ACCEPTED;
        m->data[2] = pid;
        if (msg_try_enqueue(vm, o, m)) return true;
        msg_free(vm, m);
    }
    port_close(vm, pid);             /* t_close leaves it half-open */
    return false;
}

/* ---- input ---- */

/* RST in answer to a segment that has no connection (RFC 793). */
static void rst_reply(NetState *ns, uint32_t src, uint32_t sport, uint32_t dport, const Seg *s) {
    ns->st.tcp_noconn++;
    if (s->fl & TH_RST) return;
    if (s->fl & TH_ACK) {
        out(ns, src, dport, sport, s->ack, 0, TH_RST, 0, NULL, 0);
    } else {
        uint32_t len = s->n + ((s->fl & TH_SYN) ? 1u : 0u) + ((s->fl & TH_FIN) ? 1u : 0u);
        out(ns, src, dport, sport, 0, s->seq + len, TH_RST | TH_ACK, 0, NULL, 0);
    }
}

static uint16_t peer_mss(const Seg *s) {
    uint32_t m = s->mss ? s->mss : DEFAULT_MSS;
    return (uint16_t)(m > TCP_MSS ? TCP_MSS : m);
}

/* A SYN for listener li: a half-open connection, or dropped (no slot, or
 * it would take the last free or TIME_WAIT one, kept for Dial). */
static void passive(NetState *ns, uint32_t li, uint32_t src, uint32_t sport, uint32_t dport, const Seg *s) {
    uint32_t avail = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (ns->tcp[i].state == TCP_CLOSED || ns->tcp[i].state == TCP_TIME_WAIT) avail++;
    }
    TcpPcb *c = avail >= 2u ? pcb_alloc(ns, false) : NULL;
    if (!c) {
        ns->st.tcp_syn_drop++;
        return;
    }
    c->state = TCP_SYN_RCVD;
    c->rip = src;
    c->lport = (uint16_t)dport;
    c->rport = (uint16_t)sport;
    c->listener = (uint8_t)li;
    c->rcv_nxt = s->seq + 1u;
    c->snd_una = isn(ns);
    c->snd_nxt = c->snd_una + 1u;
    c->mss = peer_mss(s);
    c->wnd = s->wnd;
    c->flags = TF_PAUSED | TF_INBOUND;
    xmit(ns->vm, c);
    timer_set(ns, c, rto(c));
}

static void syn_sent(Vm *vm, TcpPcb *c, const Seg *s, uint32_t sport) {
    NetState *ns = vm->net;
    if ((s->fl & TH_ACK) && s->ack != c->snd_nxt) {
        if (!(s->fl & TH_RST)) out(ns, c->rip, c->lport, sport, s->ack, 0, TH_RST, 0, NULL, 0);
        ns->st.tcp_drop++;
        return;
    }
    if (s->fl & TH_RST) {
        if (s->fl & TH_ACK) abort_conn(vm, c, GOGO_E_REFUSED, false);
        else ns->st.tcp_drop++;
        return;
    }
    if ((s->fl & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
        ns->st.tcp_drop++;         /* simultaneous open is not supported */
        return;
    }
    c->rcv_nxt = s->seq + 1u;
    c->snd_una = s->ack;
    c->mss = peer_mss(s);
    c->wnd = s->wnd;
    c->state = TCP_ESTABLISHED;
    c->retries = 0;
    if (!(c->flags & TF_UPPER)) c->flags &= (uint8_t)~TF_UNTIL;   /* else the layer's handshake's too */
    idle(ns, c);
    send_ack(ns, c);
    event(vm, c, TYPE_NET_CONNECTED, -1);
    output(vm, c);
}

/* The ACK field (synchronized states). False: drop the segment. */
static bool ack_in(Vm *vm, TcpPcb *c, const Seg *s) {
    NetState *ns = vm->net;
    uint32_t acked = s->ack - c->snd_una;
    if ((int32_t)acked < 0) return true;        /* old: ignore the ACK field */
    if (acked > c->snd_nxt - c->snd_una) {      /* acknowledges what was never sent */
        send_ack(ns, c);
        ns->st.tcp_drop++;
        return false;
    }
    c->wnd = s->wnd;
    if (!acked) return true;
    c->snd_una = s->ack;
    c->retries = 0;
    uint32_t d = acked < c->seg ? acked : c->seg;
    c->seg = (uint16_t)(c->seg - d);
    if (c->flags & TF_UPPER) {
        if (d) ns->upper->acked(vm, c, d);
    } else {
        uint32_t len, k;
        c->wr_off += d;
        if (head(vm, c, &len, &k) && !c->seg && c->wr_off >= len) tcp_write_done(vm, c);
    }
    if (c->snd_una == c->snd_nxt) idle(ns, c);
    else timer_set(ns, c, rto(c));
    return true;
}

static void time_wait(Vm *vm, TcpPcb *c) {
    const NetState *ns = vm->net;
    c->state = TCP_TIME_WAIT;
    detach(vm, c);                   /* the port has no connection any more */
    c->flags = (uint8_t)(c->flags & TF_FIN);
    timer_set(ns, c, TIME_WAIT_MS);
}

/* Our FIN was acknowledged. False if c was freed. */
static bool fin_acked(Vm *vm, TcpPcb *c) {
    const NetState *ns = vm->net;
    switch (c->state) {
    case TCP_FIN_WAIT_1:
        c->state = TCP_FIN_WAIT_2;
        if (c->flags & TF_ORPHAN) timer_set(ns, c, ORPHAN_FW2_MS);
        else idle(ns, c);
        return true;
    case TCP_CLOSING:
        time_wait(vm, c);
        return true;
    case TCP_LAST_ACK:
        pcb_free(vm, c);
        return false;
    default:
        return true;
    }
}

static void synchronized(Vm *vm, TcpPcb *c, const Seg *s) {
    NetState *ns = vm->net;
    uint32_t off = s->seq - c->rcv_nxt;
    if (c->state == TCP_SYN_RCVD && (s->fl & TH_SYN) && !(s->fl & TH_ACK) && s->seq + 1u == c->rcv_nxt) {
        xmit(vm, c);                     /* the peer resent its SYN */
        return;
    }
    if (off > TCP_MSS + 1u) {        /* before rcv_nxt, or beyond any window we offered (+ FIN) */
        if (!(s->fl & TH_RST)) send_ack(ns, c);
        ns->st.tcp_drop++;
        return;
    }
    if (s->fl & TH_RST) {
        if (off) {                   /* RFC 5961 §3: in window but not exact */
            send_ack(ns, c);
            ns->st.tcp_drop++;
            return;
        }
        if (c->state == TCP_SYN_RCVD || c->state == TCP_LAST_ACK || c->state == TCP_CLOSING ||
            c->state == TCP_TIME_WAIT) pcb_free(vm, c);
        else abort_conn(vm, c, GOGO_E_RESET, false);
        return;
    }
    if (s->fl & TH_SYN) {            /* RFC 5961: challenge ACK */
        send_ack(ns, c);
        ns->st.tcp_drop++;
        return;
    }
    if (!(s->fl & TH_ACK)) {
        ns->st.tcp_drop++;
        return;
    }
    if (c->state == TCP_SYN_RCVD) {
        if (s->ack != c->snd_nxt) {
            out(ns, c->rip, c->lport, c->rport, s->ack, 0, TH_RST, 0, NULL, 0);
            ns->st.tcp_drop++;
            return;
        }
        if (!accept(vm, c)) return;  /* retried on the SYN-ACK timer */
        c->snd_una = s->ack;
        c->wnd = s->wnd;
        c->state = TCP_ESTABLISHED;
        c->retries = 0;
        idle(ns, c);
    } else {
        bool fin_out = (c->flags & TF_FIN) && c->snd_una != c->snd_nxt;
        if (!ack_in(vm, c, s)) return;
        if (fin_out && c->snd_una == c->snd_nxt && !fin_acked(vm, c)) return;
    }
    if (c->state == TCP_TIME_WAIT) {
        if (s->fl & TH_FIN) {        /* the peer resent its FIN */
            send_ack(ns, c);
            timer_set(ns, c, TIME_WAIT_MS);
        }
        return;
    }
    bool ack = false;
    if (s->n || (s->fl & TH_FIN)) {
        bool open = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
        if (off || !open || (c->flags & TF_PAUSED)) {
            /* Out of order, after the peer's FIN, or paused: ACK what we
             * have (window 0 while paused). */
            send_ack(ns, c);
            ns->st.tcp_drop++;
            heard(ns, c);
            output(vm, c);
            return;
        }
        if (s->n) {
            if (!deliver(vm, c, s->p, s->n)) {
                ns->st.tcp_quota++;    /* not ACKed: the peer resends */
                c->stall = 1;
                output(vm, c);
                return;
            }
            ns->st.tcp_rx++;
            c->rcv_nxt += s->n;
            c->stall = 0;
            ack = true;
        }
        bool fin = (s->fl & TH_FIN) != 0;
        if (fin && !((c->flags & TF_UPPER) ? ns->upper->fin(vm, c) : try_event(vm, c, TYPE_CLOSED, GOGO_E_OK))) {
            c->stall = 1;                /* not ACKed: the peer resends */
        } else if (fin) {
            c->stall = 0;
            c->rcv_nxt++;
            ack = true;
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_2) {
                send_ack(ns, c);
                time_wait(vm, c);
                return;
            } else {                     /* FIN_WAIT_1, our FIN not yet acknowledged */
                c->state = TCP_CLOSING;
            }
        }
    }
    heard(ns, c);
    if (ack) send_ack(ns, c);
    output(vm, c);
}

void tcp_rx(NetState *ns, uint32_t src, uint32_t dst, const uint8_t *t, uint32_t len) {
    Vm *vm = ns->vm;
    uint32_t hl = len >= TCP_HLEN ? (uint32_t)(t[12] >> 4) * 4u : 0u;
    if (hl < TCP_HLEN || hl > len || tcp_sum(src, dst, t, len) != 0xFFFFu) {
        ns->st.tcp_bad++;
        return;
    }
    Seg s;
    uint32_t sport = rd16(t), dport = rd16(t + 2);
    s.seq = rd32(t + 4);
    s.ack = rd32(t + 8);
    s.fl = t[13];
    s.wnd = rd16(t + 14);
    s.p = t + hl;
    s.n = len - hl;
    s.mss = 0;
    for (uint32_t i = TCP_HLEN; i < hl; ) {     /* options: only MSS is used */
        uint8_t kind = t[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1u >= hl || t[i + 1] < 2u || i + t[i + 1] > hl) break;
        if (kind == 2 && t[i + 1] == 4u) s.mss = rd16(t + i + 2);
        i += t[i + 1];
    }
    TcpPcb *c = NULL;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *k = &ns->tcp[i];
        if (k->state != TCP_CLOSED && k->lport == dport && k->rport == sport && k->rip == src) {
            c = k;
            break;
        }
    }
    if (c && c->state == TCP_TIME_WAIT && (s.fl & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN &&
        (int32_t)(s.seq - c->rcv_nxt) > 0) {
        pcb_free(vm, c);             /* a new connection reuses the pair */
        c = NULL;
    }
    if (!c) {
        if ((s.fl & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN) {
            for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
                if (ns->lis[i].port && ns->lis[i].lport == dport) {
                    passive(ns, i, src, sport, dport, &s);
                    return;
                }
            }
        }
        rst_reply(ns, src, sport, dport, &s);
        return;
    }
    if (c->state == TCP_SYN_SENT) syn_sent(vm, c, &s, sport);
    else synchronized(vm, c, &s);
}

/* ---- timers ---- */

static void fire(Vm *vm, TcpPcb *c, uint32_t now) {
    NetState *ns = vm->net;
    c->flags &= (uint8_t)~TF_TIMER;
    /* The connect deadline (and an upper layer's handshake's), or an
     * orphan's life: up, progress or not. */
    if ((c->flags & TF_UNTIL) && due(c->until, now)) {
        abort_conn(vm, c, GOGO_E_TIMEOUT, c->state != TCP_SYN_SENT);
        return;
    }
    switch (c->state) {
    case TCP_TIME_WAIT:
        pcb_free(vm, c);
        return;
    case TCP_FIN_WAIT_2:             /* an orphan the peer never closed */
        if (c->flags & TF_ORPHAN) {
            abort_conn(vm, c, GOGO_E_TIMEOUT, true);
            return;
        }
        break;
    case TCP_SYN_SENT:
        if (c->snd_nxt == c->snd_una) {  /* the first SYN (deferred from open) */
            c->snd_nxt++;
            xmit(vm, c);
            timer_set(ns, c, rto(c));
            return;
        }
        break;
    default:
        break;
    }
    if (c->snd_una == c->snd_nxt) {             /* nothing in flight */
        if (!keepalive(c)) {
            timer_stop(c);                       /* (an orphan's stays at its end) */
            return;
        }
        if (c->stall && owner_room(vm, c)) {    /* drained since: the peer's next resend is taken */
            c->stall = 0;
            idle(ns, c);
            return;
        }
        if (c->retries >= GOGO_NET_KEEPALIVE_PROBES) {
            abort_conn(vm, c, GOGO_E_TIMEOUT, true);
            return;
        }
        c->retries++;
        seg_out(ns, c, c->snd_nxt - 1u, TH_ACK, NULL, 0);    /* a probe */
        timer_set(ns, c, GOGO_NET_KEEPALIVE_INTVL_MS);
        return;
    }
    if (c->retries >= MAX_RETRIES) {
        if (c->state == TCP_SYN_RCVD && !c->port) {
            send_rst(ns, c);
            pcb_free(vm, c);
        } else {
            abort_conn(vm, c, GOGO_E_TIMEOUT, c->state != TCP_SYN_SENT);
        }
        return;
    }
    c->retries++;
    ns->st.tcp_retx++;
    xmit(vm, c);
    timer_set(ns, c, rto(c));
}

bool tcp_due(const NetState *ns) {
    bool any = ns->tcp_held;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) any |= (ns->tcp[i].flags & TF_TIMER) != 0;
    if (!any) return false;
    uint32_t now = now32(ns);
    if (ns->tcp_held && due(ns->held_at, now)) return true;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        const TcpPcb *c = &ns->tcp[i];
        if ((c->flags & TF_TIMER) && due(c->deadline, now)) return true;
    }
    return false;
}

void tcp_poll(Vm *vm) {
    NetState *ns = vm->net;
    if (!tcp_due(ns)) return;
    uint32_t now = now32(ns);
    if (ns->tcp_held && due(ns->held_at, now)) {
        ns->tcp_held = 0;
        for (uint32_t i = 0; i < vm->port_n; i++) {
            Port *p = port_at(vm, i);
            bool ours = p->drv == &net_tcp_driver || (ns->upper && p->drv == ns->upper->drv);
            if (ours && held(p) && !flush(vm, p)) retry_later(ns);
        }
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &ns->tcp[i];
        if (c->state != TCP_CLOSED && (c->flags & TF_TIMER) && due(c->deadline, now)) fire(vm, c, now);
    }
}

bool tcp_next_deadline(const NetState *ns, uint64_t *deadline) {
    bool have = false;
    uint64_t now = 0;
    if (ns->tcp_held) {
        now = vm_now(ns->vm);
        int32_t left = (int32_t)(ns->held_at - (uint32_t)now);
        *deadline = left > 0 ? now + (uint32_t)left : now;
        have = true;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        const TcpPcb *c = &ns->tcp[i];
        if (!(c->flags & TF_TIMER)) continue;
        if (!have) now = vm_now(ns->vm);
        int32_t left = (int32_t)(c->deadline - (uint32_t)now);
        uint64_t d = left > 0 ? now + (uint32_t)left : now;
        if (!have || d < *deadline) *deadline = d;
        have = true;
    }
    return have;
}

bool tcp_any_orphan(const NetState *ns) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (ns->tcp[i].flags & TF_ORPHAN) return true;
    }
    return false;
}

void tcp_down(Vm *vm) {
    NetState *ns = vm->net;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &ns->tcp[i];
        if (c->state == TCP_CLOSED) continue;
        if (c->port && c->state != TCP_SYN_RCVD) abort_conn(vm, c, GOGO_E_NOROUTE, false);
        else pcb_free(vm, c);
    }
}
