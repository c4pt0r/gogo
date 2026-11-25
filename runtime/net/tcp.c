/* TCP and the "tcp" / "tcp_listen" ports (spec M5 §5.2, §5.3, §9.3, §9.5,
 * §9.6): one segment in flight each way, in-order receive only, data
 * acknowledged only once delivered into the owner's mailbox.
 *
 * Control blocks live in net_s.tcp (GOGO_NET_MAX_TCP of them), charged to
 * the port's owner. A half-open passive connection (SYN_RCVD) takes a slot
 * with no port; completing the handshake opens a paused "tcp" port for the
 * listener's owner and sends it Accepted{Conn}. A port whose connection
 * has ended keeps no slot: its commands are ignored until it is closed.
 * Inbound connections (TF_INBOUND) never take the last free slot: a SYN
 * that would is dropped (the peer retries), so the device's own Dial
 * always finds one (a free slot, else a TIME_WAIT or half-open one).
 *
 * Timers are the stack's own: one deadline per connection (retransmission,
 * connect timeout, TIME_WAIT, an orphan's FIN_WAIT_2 limit and absolute
 * lifetime), reported to the scheduler through the port source's
 * next_deadline and fired from its drain (tcp_poll). Not the VM timer heap.
 *
 * Write data is pinned on the owner's behalf (a handle) or copied into a
 * port-held handle (heap or const Bytes) and sent from there, the first
 * time and on every retransmission. Closing a port after Shutdown (also by
 * its owner's exit) leaves an orphan that finishes the close: it holds its
 * unacknowledged data by message references (bytes_msg_ref), charged to
 * no one, until acknowledged, for at most GOGO_NET_ORPHAN_MS from the
 * close: then it is reset (RST) whatever its progress, so a peer that
 * acknowledges a byte at a time cannot keep it alive (spec §9.5). Closing
 * it otherwise sends RST.
 *
 * Nothing TCP sends its owner can kill it (spec §9.1): every event goes
 * through msg_try_enqueue. Data is refused (and not acknowledged) unless
 * it leaves EVENT_RESERVE words of headroom (an owner waiting in Receive
 * is first collected without growth slack, heap_reclaim_waiting, since it
 * cannot collect itself until a message comes) for the control events that
 * may follow it; a control event that still does not fit is held in the
 * port (Port.state[1]: Connected, a count of Written, a Closed reason),
 * delivered in order before anything else and retried every
 * HELD_RETRY_MS until the owner has room. */
#include "net_int.h"
#include "heap.h"
#include "bytes.h"
#include "opcodes.h"
#include "platform.h"
#include "signals.h"
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
/* Port.state[1]: held events. */
#define HELD_CONN 1u
#define HELD_WR_ONE 2u
#define HELD_WR_MASK 6u
#define HELD_CL_SHIFT 4u
#define EVENT_RESERVE TCP_EVENT_RESERVE

/* A received segment. */
typedef struct Seg {
    uint32_t seq, ack;
    const uint8_t *p;       /* payload */
    uint32_t n;             /* payload bytes */
    uint16_t wnd, mss;      /* mss: the option's value, 0 if none */
    uint8_t fl;
} Seg;

static uint32_t now32(void) { return (uint32_t)platform_now_ms(); }
static bool due(uint32_t t, uint32_t now) { return (int32_t)(now - t) >= 0; }

void tcp_reset(void) {
    memset(net_s.tcp, 0, sizeof net_s.tcp);
    memset(net_s.lis, 0, sizeof net_s.lis);
    net_s.tcp_eph = EPH_FIRST;
    net_s.accepting = 0;
}

/* ---- timers ---- */

static uint32_t rto(const TcpPcb *c) {
    return RTO_MS << (c->retries < RTO_MAX_SHIFT ? c->retries : RTO_MAX_SHIFT);
}

/* TF_UNTIL (a connect deadline, or an orphan's end) caps every timer. */
static void timer_set(TcpPcb *c, uint32_t ms) {
    c->deadline = now32() + ms;
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
static void out(uint32_t rip, uint32_t lport, uint32_t rport, uint32_t seq, uint32_t ack, uint8_t fl,
                uint32_t wnd, const uint8_t *p, uint32_t len) {
    uint8_t *t = net_s.tx + ETH_HLEN + IP_HLEN;
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
    wr16(t + 16, (uint16_t)~tcp_sum(net_s.ip, rip, t, hl + len));
    ip_send(net_s.ip, rip, IP_TCP, hl + len, NULL);
}

static void seg_out(const TcpPcb *c, uint32_t seq, uint8_t fl, const uint8_t *p, uint32_t len) {
    out(c->rip, c->lport, c->rport, seq, (fl & TH_ACK) ? c->rcv_nxt : 0u, fl,
        (c->flags & TF_PAUSED) ? 0u : TCP_MSS, p, len);
}

static void send_ack(const TcpPcb *c) { seg_out(c, c->snd_nxt, TH_ACK, NULL, 0); }
static void send_rst(const TcpPcb *c) { seg_out(c, c->snd_nxt, TH_RST | TH_ACK, NULL, 0); }

/* The bytes to send: wr[0] from wr_off, or the upper layer's. */
static const uint8_t *tx_view(const TcpPcb *c, uint32_t *len) {
    if (c->flags & TF_UPPER) return net_s.upper->tx(c, len);
    uint32_t n = 0;
    const uint8_t *d = c->wr[0] ? bytes_data(c->wr[0], &n) : NULL;
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
static void xmit(const TcpPcb *c) {
    switch (c->state) {
    case TCP_SYN_SENT:
        seg_out(c, c->snd_una, TH_SYN, NULL, 0);
        return;
    case TCP_SYN_RCVD:
        seg_out(c, c->snd_una, TH_SYN | TH_ACK, NULL, 0);
        return;
    default: {
        uint8_t fl = TH_ACK;
        const uint8_t *d = NULL;
        uint32_t len = 0;
        if (c->seg) {
            d = tx_view(c, &len);
            if (!d || c->seg > len) return;   /* cannot happen */
            fl |= TH_PSH;
        }
        if (fin_in_flight(c)) fl |= TH_FIN;
        seg_out(c, c->snd_una, fl, d, c->seg);
        return;
    }
    }
}

/* Sends the next segment (data and/or FIN) if none is in flight. */
static void output(TcpPcb *c) {
    if (c->snd_nxt != c->snd_una || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)) return;
    uint32_t left;
    (void)tx_view(c, &left);
    uint32_t n = left;
    if (n > c->mss) n = c->mss;
    if (n > c->wnd) n = c->wnd ? c->wnd : 1u;    /* zero window: a 1-byte probe */
    bool fin = (c->flags & TF_SHUT) && n == left && ((c->flags & TF_UPPER) || !c->wr[1]);
    if (!n && !fin) return;
    c->seg = (uint16_t)n;
    if (fin) {
        c->flags |= TF_FIN;
        c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
    }
    c->snd_nxt = c->snd_una + n + (fin ? 1u : 0u);
    c->retries = 0;
    xmit(c);
    timer_set(c, rto(c));
}

/* ---- events to the owner ---- */

static Actor *owner(Vm *vm, const TcpPcb *c) { return c->port ? port_owner(vm, c->port) : NULL; }

/* A struct event of `type` with no field, or with int field `reason` >= 0. */
static Msg *event_new(uint32_t type, int32_t reason) {
    uint32_t words = reason >= 0 ? 3u : 2u;
    Msg *m = net_msg_new(words);
    if (!m) return NULL;
    m->data[0] = hdr_make(words, K_STRUCT);
    m->data[1] = type;
    if (reason >= 0) m->data[2] = term_from_int(reason);
    return m;
}

void tcp_output(TcpPcb *c) { output(c); }

/* Queues an event on p's owner if it fits (never fatal). */
static bool post(Vm *vm, const Port *p, uint32_t type, int32_t reason) {
    Actor *o = port_owner(vm, p);
    Msg *m = o ? event_new(type, reason) : NULL;
    if (!m) return false;
    if (msg_try_enqueue(vm, o, m)) return true;
    msg_free(m);
    return false;
}

/* Delivers p's held events in order; true when none is left. */
static bool flush(Vm *vm, Port *p) {
    uint32_t *e = &p->state[1];
    if (!*e) return true;
    if (*e & HELD_CONN) {
        if (!post(vm, p, TYPE_NET_CONNECTED, -1)) return false;
        *e &= ~HELD_CONN;
    }
    while (*e & HELD_WR_MASK) {
        if (!post(vm, p, TYPE_NET_WRITTEN, -1)) return false;
        *e -= HELD_WR_ONE;
    }
    if (*e >> HELD_CL_SHIFT) {
        if (!post(vm, p, TYPE_NET_CLOSED, (int32_t)(*e >> HELD_CL_SHIFT) - 1)) return false;
        *e = 0;
    }
    return true;
}

bool tcp_flush(Vm *vm, Port *p) { return flush(vm, p); }

static void retry_later(void) {
    net_s.tcp_held = 1;
    net_s.held_at = now32() + HELD_RETRY_MS;
}

/* Control events (Connected, Written, Closed other than the peer's FIN):
 * held in the port, after any already held, and delivered when they fit. */
void tcp_post(Vm *vm, const TcpPcb *c, uint32_t type, int32_t reason) {
    Port *p = c->port;
    if (!p) return;
    uint32_t *e = &p->state[1];
    if (type == TYPE_NET_CONNECTED) *e |= HELD_CONN;
    else if (type == TYPE_NET_WRITTEN) { if ((*e & HELD_WR_MASK) != HELD_WR_MASK) *e += HELD_WR_ONE; }  /* full: t_command prevents it */
    else if (!(*e >> HELD_CL_SHIFT) ||    /* the first, or a terminal one after a held Normal */
             ((*e >> HELD_CL_SHIFT) == CR_NORMAL + 1u && reason != CR_NORMAL)) {
        *e = (*e & ((1u << HELD_CL_SHIFT) - 1u)) | (uint32_t)(reason + 1) << HELD_CL_SHIFT;
    }
    if (!flush(vm, p)) retry_later();
}

/* An upper layer's connection: Connected is the layer's to send, and its
 * own terminal Closed stands for the connection's end. */
static void event(Vm *vm, TcpPcb *c, uint32_t type, int32_t reason) {
    if (c->flags & TF_UPPER) {
        if (type == TYPE_NET_CONNECTED) {
            net_s.upper->connected(vm, c);
            return;
        }
        if (type == TYPE_NET_CLOSED && net_s.upper->closed && net_s.upper->closed(c)) return;
    }
    tcp_post(vm, c, type, reason);
}

void tcp_hold_closed(Port *p, int32_t reason) {
    p->state[0] = GOGO_NET_MAX_TCP;              /* no connection */
    p->state[1] = (uint32_t)(reason + 1) << HELD_CL_SHIFT;
    net_s.tcp_held = 1;
    net_s.held_at = now32();
}

/* The peer's FIN's Closed{Normal}: acknowledged only once queued, so not
 * held; true if queued, or if there is no port (orphan). */
static bool try_event(Vm *vm, const TcpPcb *c, uint32_t type, int32_t reason) {
    if (!c->port) return true;
    return flush(vm, c->port) && post(vm, c->port, type, reason);
}

/* Data{Data}: the payload as a heap K_BYTES in the block (spec §9.2). An
 * orphan discards it. */
static bool deliver(Vm *vm, TcpPcb *c, const uint8_t *p, uint32_t n) {
    if (!c->port) return true;
    if (c->flags & TF_UPPER) return net_s.upper->rx(vm, c, p, n);
    Actor *o = owner(vm, c);
    uint32_t bw = 2u + (n + 3u) / 4u;
    Msg *m = o && flush(vm, c->port) ? net_msg_new(3u + bw) : NULL;
    if (!m) return false;
    uint32_t *d = m->data;
    d[0] = hdr_make(3, K_STRUCT);
    d[1] = TYPE_NET_DATA;
    d[2] = term_from_ref(3);
    d[3] = hdr_make(bw, K_BYTES);
    d[4] = n;
    d[3u + bw - 1u] = 0;             /* padding */
    memcpy(&d[5], p, n);
    if (net_deliver(vm, o, m, EVENT_RESERVE)) return true;
    msg_free(m);
    return false;
}

/* ---- control blocks ---- */

TcpPcb *tcp_pcb_of(const Port *p) {
    uint32_t i = p->state[0];
    return i < GOGO_NET_MAX_TCP && net_s.tcp[i].state != TCP_CLOSED && net_s.tcp[i].port == p ?
        &net_s.tcp[i] : NULL;
}

/* Releases wr[k]: the port's pin, or an orphan's message reference. */
static void drop_data(Vm *vm, TcpPcb *c, uint32_t k) {
    term h = c->wr[k];
    c->wr[k] = 0;
    if (!term_is_handle(h)) return;  /* none, or an empty Write */
    if (c->flags & TF_ORPHAN) bytes_msg_unref(h);
    else if (c->port && vm) port_bytes_release(vm, c->port, h);
}

/* Releases c's data and detaches its port, which stays open with no
 * connection (its held events still go out) and no longer charged. */
static void detach(Vm *vm, TcpPcb *c) {
    if (c->flags & TF_UPPER) {
        c->flags &= (uint8_t)~TF_UPPER;
        net_s.upper->ended(c);
    }
    drop_data(vm, c, 0);
    drop_data(vm, c, 1);
    if (c->port && vm) {
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
    if (rst) send_rst(c);
    if (reason >= 0) event(vm, c, TYPE_NET_CLOSED, reason);
    pcb_free(vm, c);
}

void tcp_abort(Vm *vm, TcpPcb *c, int32_t reason, bool rst) { abort_conn(vm, c, reason, rst); }

void tcp_until_clear(TcpPcb *c) {
    c->flags &= (uint8_t)~TF_UNTIL;
    if (c->snd_una == c->snd_nxt) c->flags &= (uint8_t)~TF_TIMER;
    else timer_set(c, rto(c));
}

/* A free slot, else the oldest TIME_WAIT's, else (for an active open) a
 * half-open connection's (reset); NULL if none. */
static TcpPcb *pcb_alloc(bool dial) {
    TcpPcb *tw = NULL, *half = NULL;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &net_s.tcp[i];
        if (c->state == TCP_CLOSED) return c;
        if (c->state == TCP_TIME_WAIT && (!tw || (int32_t)(c->deadline - tw->deadline) < 0)) tw = c;
        if (c->state == TCP_SYN_RCVD && (c->flags & TF_INBOUND) && !c->port && !half) half = c;
    }
    TcpPcb *v = tw ? tw : dial ? half : NULL;
    if (!v) return NULL;
    pcb_free(NULL, v);               /* neither holds data or a port; no RST (the
                                      * peer's next segment gets one) */
    return v;
}

/* Clock-driven (RFC 793) plus a counter stirred by our MAC and the
 * traffic seen so far: not a secret, only harder to guess blindly. */
static uint32_t isn(void) {
    net_s.isn = net_s.isn * 1103515245u + 12345u + net_s.st.rx_frames + rd32(net_s.mac + 2);
    return now32() * 250u + net_s.isn;
}

static bool lport_used(uint32_t lport) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (net_s.tcp[i].state != TCP_CLOSED && net_s.tcp[i].lport == lport) return true;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        if (net_s.lis[i].port && net_s.lis[i].lport == lport) return true;
    }
    return false;
}

/* ---- the "tcp" port ---- */

/* The active open of Dial{IP, Port, TimeoutMs}. */
TcpPcb *tcp_dial(Port *p, const int32_t *cfg, uint32_t n) {
    if (!net_s.attached || n != 3u || !ip_configured() || cfg[0] == 0 || (uint32_t)cfg[0] == net_s.ip ||
        (uint32_t)cfg[0] == IP_BCAST || ((uint32_t)cfg[0] >> 28) == 0xEu ||
        cfg[1] <= 0 || cfg[1] > 65535 || cfg[2] < 0) return NULL;
    uint32_t rip = (uint32_t)cfg[0];
    if (!net_s.link_up || (!ip_on_link(rip) && !net_s.gw)) return NULL;   /* no route */
    TcpPcb *c = pcb_alloc(true);
    if (!c) return NULL;
    uint32_t lport;
    do {
        lport = net_s.tcp_eph;
        net_s.tcp_eph = (uint16_t)(net_s.tcp_eph == 65535u ? EPH_FIRST : net_s.tcp_eph + 1u);
    } while (lport_used(lport));
    c->state = TCP_SYN_SENT;
    c->rip = rip;
    c->lport = (uint16_t)lport;
    c->rport = (uint16_t)cfg[1];
    c->snd_una = isn();
    c->mss = DEFAULT_MSS;
    if (cfg[2]) {
        c->until = now32() + (uint32_t)cfg[2];
        c->flags |= TF_UNTIL;
    }
    c->snd_nxt = c->snd_una;     /* the SYN goes out at the next poll, */
    timer_set(c, 0);             /* once the port layer has accepted the open */
    c->port = p;
    p->state[0] = (uint32_t)(c - net_s.tcp);
    p->charge_words = TCP_PCB_WORDS;
    return c;
}

/* Dial{IP, Port, TimeoutMs}; or, with no fields, the connection being
 * accepted (net_s.accepting, set by accept() below). */
static int t_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    if (n != 0u) return tcp_dial(p, cfg, n) ? 0 : -1;
    if (!net_s.accepting) return -1;
    TcpPcb *c = &net_s.tcp[net_s.accepting - 1u];
    c->port = p;
    p->state[0] = (uint32_t)(c - net_s.tcp);
    p->charge_words = TCP_PCB_WORDS;
    return 0;
}

/* No vm here: the port layer releases the pins after this. */
void tcp_close_port(Port *p) {
    TcpPcb *c = tcp_pcb_of(p);
    if (!c) return;
    switch (c->state) {
    case TCP_SYN_RCVD:               /* accept failed: stays half-open */
        c->port = NULL;
        return;
    case TCP_ESTABLISHED: case TCP_CLOSE_WAIT: case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2: case TCP_CLOSING: case TCP_LAST_ACK:
        if (c->flags & TF_SHUT) {
            /* Orphan (spec §9.5): it finishes the FIN exchange alone. */
            if (c->wr[0]) bytes_msg_ref(c->wr[0]);
            if (c->wr[1]) bytes_msg_ref(c->wr[1]);
            c->flags = (uint8_t)((c->flags | TF_ORPHAN | TF_UNTIL) & ~TF_PAUSED);
            c->port = NULL;
            c->until = now32() + GOGO_NET_ORPHAN_MS;
            if (c->state == TCP_FIN_WAIT_2) timer_set(c, ORPHAN_FW2_MS);
            else if (!(c->flags & TF_TIMER) || (int32_t)(c->deadline - c->until) > 0) timer_stop(c);
            return;
        }
        send_rst(c);
        break;
    default:
        break;
    }
    memset(c, 0, sizeof *c);         /* the port layer releases wr[] */
}

bool tcp_port_can_event(const Port *p) { return tcp_pcb_of(p) != NULL || p->state[1] != 0; }

static void unpause(TcpPcb *c) {
    if (!(c->flags & TF_PAUSED)) return;
    c->flags &= (uint8_t)~TF_PAUSED;
    send_ack(c);                     /* window update */
}

static void t_handoff(Vm *vm, Port *p) {
    (void)vm;
    TcpPcb *c = tcp_pcb_of(p);
    if (c) unpause(c);
}

static void written(Vm *vm, const TcpPcb *c) { tcp_post(vm, c, TYPE_NET_WRITTEN, -1); }

void tcp_write_done(Vm *vm, TcpPcb *c) {
    do {                             /* this Write, and empty ones queued after it */
        drop_data(vm, c, 0);
        c->wr[0] = c->wr[1];
        c->wr[1] = 0;
        c->wr_off = 0;
        written(vm, c);
    } while (c->wr[0] == T_EMPTYBYTES);
}

/* Write{Data} (o, a struct of type TYPE_NET_WRITE): pinned into wr[]. */
bool tcp_write_cmd(Vm *vm, const Actor *from, Port *p, TcpPcb *c, const uint32_t *o) {
    const uint8_t *d;
    uint32_t len;
    if (hdr_words(o[0]) != 3u || !bytes_view(vm, from, o[2], &d, &len) || (c->flags & TF_SHUT) ||
        (c->state != TCP_SYN_SENT && c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)) {
        net_s.st.tx_bad++;
        return false;
    }
    /* Held Written events plus Writes in progress: at most 3 (what
     * Port.state[1] counts). More means the owner keeps writing
     * without reading its Written (more than 2 outstanding). */
    uint32_t owed = (p->state[1] & HELD_WR_MASK) / HELD_WR_ONE + (c->wr[0] ? 1u : 0u) + (c->wr[1] ? 1u : 0u);
    if (owed >= 3u) {
        abort_conn(vm, c, CR_OVERFLOW, c->state != TCP_SYN_SENT);
        return false;
    }
    if (!len && !c->wr[0]) {
        written(vm, c);
        return true;
    }
    uint32_t k = !c->wr[0] ? 0u : !c->wr[1] ? 1u : 2u;
    term h = T_NIL;
    if (k < 2u && !len) {
        h = T_EMPTYBYTES;        /* Written in turn, after the one before */
    } else if (k < 2u) {
        if (term_is_handle(o[2])) {
            if (port_bytes_retain(vm, p, o[2])) h = o[2];
        } else {
            uint8_t *w;
            h = port_bytes_new(vm, p, len, &w);
            if (h != T_NIL) memcpy(w, d, len);
        }
    }
    if (h == T_NIL) {            /* a third Write, or no room to hold it */
        abort_conn(vm, c, CR_OVERFLOW, c->state != TCP_SYN_SENT);
        return false;
    }
    c->wr[k] = h;
    return true;
}

void tcp_shutdown(TcpPcb *c) {
    c->flags |= TF_SHUT;
    output(c);
}

/* Write{Data}, Shutdown{}, Resume{}. */
static void t_command(Vm *vm, const Actor *from, Port *p, term msg) {
    TcpPcb *c = tcp_pcb_of(p);
    const uint32_t *o = obj_of(vm, from, msg);
    if (!c) return;                  /* the connection has ended */
    if (!o || hdr_kind(o[0]) != K_STRUCT) {
        net_s.st.tx_bad++;
        return;
    }
    switch (o[1]) {
    case TYPE_NET_WRITE:
        if (tcp_write_cmd(vm, from, p, c, o)) output(c);
        return;
    case TYPE_NET_SHUTDOWN:
        if (hdr_words(o[0]) != 2u) break;
        tcp_shutdown(c);
        return;
    case TYPE_NET_RESUME:
        if (hdr_words(o[0]) != 2u) break;
        unpause(c);
        return;
    default:
        break;
    }
    net_s.st.tx_bad++;
}

const PortDriver net_tcp_driver = {
    "tcp", TYPE_NET_DIAL, TYPE_NET_DATA, t_open, NULL, tcp_close_port, tcp_port_can_event, t_handoff, t_command, NULL
};

/* ---- the "tcp_listen" port ---- */

static int l_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    if (!net_s.attached || n != 1u || cfg[0] <= 0 || cfg[0] > 65535) return -1;
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        if (net_s.lis[i].port && net_s.lis[i].lport == (uint32_t)cfg[0]) return -1;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_LISTEN; i++) {
        TcpListen *l = &net_s.lis[i];
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
static void l_close(Port *p) {
    uint32_t i = p->state[0];
    if (i >= GOGO_NET_MAX_LISTEN || net_s.lis[i].port != p) return;
    for (uint32_t k = 0; k < GOGO_NET_MAX_TCP; k++) {
        TcpPcb *c = &net_s.tcp[k];
        if (c->state == TCP_SYN_RCVD && c->listener == i) {
            send_rst(c);
            memset(c, 0, sizeof *c);
        }
    }
    memset(&net_s.lis[i], 0, sizeof net_s.lis[i]);
}

static bool l_can_event(const Port *p) { (void)p; return true; }

const PortDriver net_tcp_listen_driver = {
    "tcp_listen", TYPE_NET_LISTEN, TYPE_NET_ACCEPTED, l_open, NULL, l_close, l_can_event, NULL, NULL, NULL
};

/* The handshake completed: opens a paused "tcp" port for the listener's
 * owner and sends it Accepted{Conn}. False (the connection stays half-open
 * and retries on its SYN-ACK retransmission) if the owner cannot take the
 * port or the message. */
static bool accept(Vm *vm, TcpPcb *c) {
    const TcpListen *l = &net_s.lis[c->listener];
    Actor *o = l->port ? port_owner(vm, l->port) : NULL;
    if (!o) return false;
    net_s.accepting = (uint8_t)(c - net_s.tcp + 1);
    term pid = port_open_with(vm, o, &net_tcp_driver, NULL, 0);
    net_s.accepting = 0;
    if (pid == T_NIL) return false;
    Msg *m = net_msg_new(3);
    if (m) {
        m->data[0] = hdr_make(3, K_STRUCT);
        m->data[1] = TYPE_NET_ACCEPTED;
        m->data[2] = pid;
        if (msg_try_enqueue(vm, o, m)) return true;
        msg_free(m);
    }
    port_close(vm, pid);             /* t_close leaves it half-open */
    return false;
}

/* ---- input ---- */

/* RST in answer to a segment that has no connection (RFC 793). */
static void rst_reply(uint32_t src, uint32_t sport, uint32_t dport, const Seg *s) {
    net_s.st.tcp_noconn++;
    if (s->fl & TH_RST) return;
    if (s->fl & TH_ACK) {
        out(src, dport, sport, s->ack, 0, TH_RST, 0, NULL, 0);
    } else {
        uint32_t len = s->n + ((s->fl & TH_SYN) ? 1u : 0u) + ((s->fl & TH_FIN) ? 1u : 0u);
        out(src, dport, sport, 0, s->seq + len, TH_RST | TH_ACK, 0, NULL, 0);
    }
}

static uint16_t peer_mss(const Seg *s) {
    uint32_t m = s->mss ? s->mss : DEFAULT_MSS;
    return (uint16_t)(m > TCP_MSS ? TCP_MSS : m);
}

/* A SYN for listener li: a half-open connection, or dropped (no slot, or
 * it would take the last free or TIME_WAIT one, kept for Dial). */
static void passive(uint32_t li, uint32_t src, uint32_t sport, uint32_t dport, const Seg *s) {
    uint32_t avail = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (net_s.tcp[i].state == TCP_CLOSED || net_s.tcp[i].state == TCP_TIME_WAIT) avail++;
    }
    TcpPcb *c = avail >= 2u ? pcb_alloc(false) : NULL;
    if (!c) {
        net_s.st.tcp_syn_drop++;
        return;
    }
    c->state = TCP_SYN_RCVD;
    c->rip = src;
    c->lport = (uint16_t)dport;
    c->rport = (uint16_t)sport;
    c->listener = (uint8_t)li;
    c->rcv_nxt = s->seq + 1u;
    c->snd_una = isn();
    c->snd_nxt = c->snd_una + 1u;
    c->mss = peer_mss(s);
    c->wnd = s->wnd;
    c->flags = TF_PAUSED | TF_INBOUND;
    xmit(c);
    timer_set(c, rto(c));
}

static void syn_sent(Vm *vm, TcpPcb *c, const Seg *s, uint32_t sport) {
    if ((s->fl & TH_ACK) && s->ack != c->snd_nxt) {
        if (!(s->fl & TH_RST)) out(c->rip, c->lport, sport, s->ack, 0, TH_RST, 0, NULL, 0);
        net_s.st.tcp_drop++;
        return;
    }
    if (s->fl & TH_RST) {
        if (s->fl & TH_ACK) abort_conn(vm, c, CR_REFUSED, false);
        else net_s.st.tcp_drop++;
        return;
    }
    if ((s->fl & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
        net_s.st.tcp_drop++;         /* simultaneous open is not supported */
        return;
    }
    c->rcv_nxt = s->seq + 1u;
    c->snd_una = s->ack;
    c->mss = peer_mss(s);
    c->wnd = s->wnd;
    c->state = TCP_ESTABLISHED;
    c->retries = 0;
    if (!(c->flags & TF_UPPER)) c->flags &= (uint8_t)~TF_UNTIL;   /* else the layer's handshake's too */
    timer_stop(c);
    send_ack(c);
    event(vm, c, TYPE_NET_CONNECTED, -1);
    output(c);
}

/* The ACK field (synchronized states). False: drop the segment. */
static bool ack_in(Vm *vm, TcpPcb *c, const Seg *s) {
    uint32_t acked = s->ack - c->snd_una;
    if ((int32_t)acked < 0) return true;        /* old: ignore the ACK field */
    if (acked > c->snd_nxt - c->snd_una) {      /* acknowledges what was never sent */
        send_ack(c);
        net_s.st.tcp_drop++;
        return false;
    }
    c->wnd = s->wnd;
    if (!acked) return true;
    c->snd_una = s->ack;
    c->retries = 0;
    uint32_t d = acked < c->seg ? acked : c->seg;
    c->seg = (uint16_t)(c->seg - d);
    uint32_t len = 0;
    if (c->flags & TF_UPPER) {
        if (d) net_s.upper->acked(vm, c, d);
    } else {
        c->wr_off += d;
        if (c->wr[0] && !c->seg && bytes_data(c->wr[0], &len) && c->wr_off >= len) tcp_write_done(vm, c);
    }
    if (c->snd_una == c->snd_nxt) timer_stop(c);
    else timer_set(c, rto(c));
    return true;
}

static void time_wait(Vm *vm, TcpPcb *c) {
    c->state = TCP_TIME_WAIT;
    detach(vm, c);                   /* the port has no connection any more */
    c->flags = (uint8_t)(c->flags & TF_FIN);
    timer_set(c, TIME_WAIT_MS);
}

/* Our FIN was acknowledged. False if c was freed. */
static bool fin_acked(Vm *vm, TcpPcb *c) {
    switch (c->state) {
    case TCP_FIN_WAIT_1:
        c->state = TCP_FIN_WAIT_2;
        if (c->flags & TF_ORPHAN) timer_set(c, ORPHAN_FW2_MS);
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
    uint32_t off = s->seq - c->rcv_nxt;
    if (c->state == TCP_SYN_RCVD && (s->fl & TH_SYN) && !(s->fl & TH_ACK) && s->seq + 1u == c->rcv_nxt) {
        xmit(c);                     /* the peer resent its SYN */
        return;
    }
    if (off > TCP_MSS + 1u) {        /* before rcv_nxt, or beyond any window we offered (+ FIN) */
        if (!(s->fl & TH_RST)) send_ack(c);
        net_s.st.tcp_drop++;
        return;
    }
    if (s->fl & TH_RST) {
        if (off) {                   /* RFC 5961 §3: in window but not exact */
            send_ack(c);
            net_s.st.tcp_drop++;
            return;
        }
        if (c->state == TCP_SYN_RCVD || c->state == TCP_LAST_ACK || c->state == TCP_CLOSING ||
            c->state == TCP_TIME_WAIT) pcb_free(vm, c);
        else abort_conn(vm, c, CR_RESET, false);
        return;
    }
    if (s->fl & TH_SYN) {            /* RFC 5961: challenge ACK */
        send_ack(c);
        net_s.st.tcp_drop++;
        return;
    }
    if (!(s->fl & TH_ACK)) {
        net_s.st.tcp_drop++;
        return;
    }
    if (c->state == TCP_SYN_RCVD) {
        if (s->ack != c->snd_nxt) {
            out(c->rip, c->lport, c->rport, s->ack, 0, TH_RST, 0, NULL, 0);
            net_s.st.tcp_drop++;
            return;
        }
        if (!accept(vm, c)) return;  /* retried on the SYN-ACK timer */
        c->snd_una = s->ack;
        c->wnd = s->wnd;
        c->state = TCP_ESTABLISHED;
        c->retries = 0;
        timer_stop(c);
    } else {
        bool fin_out = (c->flags & TF_FIN) && c->snd_una != c->snd_nxt;
        if (!ack_in(vm, c, s)) return;
        if (fin_out && c->snd_una == c->snd_nxt && !fin_acked(vm, c)) return;
    }
    if (c->state == TCP_TIME_WAIT) {
        if (s->fl & TH_FIN) {        /* the peer resent its FIN */
            send_ack(c);
            timer_set(c, TIME_WAIT_MS);
        }
        return;
    }
    bool ack = false;
    if (s->n || (s->fl & TH_FIN)) {
        bool open = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;
        if (off || !open || (c->flags & TF_PAUSED)) {
            /* Out of order, after the peer's FIN, or paused: ACK what we
             * have (window 0 while paused). */
            send_ack(c);
            net_s.st.tcp_drop++;
            output(c);
            return;
        }
        if (s->n) {
            if (!deliver(vm, c, s->p, s->n)) {
                net_s.st.tcp_quota++;    /* not ACKed: the peer resends */
                output(c);
                return;
            }
            net_s.st.tcp_rx++;
            c->rcv_nxt += s->n;
            ack = true;
        }
        if ((s->fl & TH_FIN) && ((c->flags & TF_UPPER) ? net_s.upper->fin(vm, c) :
                                 try_event(vm, c, TYPE_NET_CLOSED, CR_NORMAL))) {
            c->rcv_nxt++;
            ack = true;
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_2) {
                send_ack(c);
                time_wait(vm, c);
                return;
            } else {                     /* FIN_WAIT_1, our FIN not yet acknowledged */
                c->state = TCP_CLOSING;
            }
        }
    }
    if (ack) send_ack(c);
    output(c);
}

void tcp_rx(Vm *vm, uint32_t src, uint32_t dst, const uint8_t *t, uint32_t len) {
    uint32_t hl = len >= TCP_HLEN ? (uint32_t)(t[12] >> 4) * 4u : 0u;
    if (hl < TCP_HLEN || hl > len || tcp_sum(src, dst, t, len) != 0xFFFFu) {
        net_s.st.tcp_bad++;
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
        TcpPcb *k = &net_s.tcp[i];
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
                if (net_s.lis[i].port && net_s.lis[i].lport == dport) {
                    passive(i, src, sport, dport, &s);
                    return;
                }
            }
        }
        rst_reply(src, sport, dport, &s);
        return;
    }
    if (c->state == TCP_SYN_SENT) syn_sent(vm, c, &s, sport);
    else synchronized(vm, c, &s);
}

/* ---- timers ---- */

static void fire(Vm *vm, TcpPcb *c, uint32_t now) {
    c->flags &= (uint8_t)~TF_TIMER;
    /* The connect deadline (and an upper layer's handshake's), or an
     * orphan's life: up, progress or not. */
    if ((c->flags & TF_UNTIL) && due(c->until, now)) {
        abort_conn(vm, c, CR_TIMEOUT, c->state != TCP_SYN_SENT);
        return;
    }
    switch (c->state) {
    case TCP_TIME_WAIT:
        pcb_free(vm, c);
        return;
    case TCP_FIN_WAIT_2:             /* an orphan the peer never closed */
        if (c->flags & TF_ORPHAN) abort_conn(vm, c, CR_TIMEOUT, true);
        return;
    case TCP_SYN_SENT:
        if (c->snd_nxt == c->snd_una) {  /* the first SYN (deferred from open) */
            c->snd_nxt++;
            xmit(c);
            timer_set(c, rto(c));
            return;
        }
        break;
    default:
        break;
    }
    if (c->snd_una == c->snd_nxt) {             /* nothing in flight */
        timer_stop(c);                           /* (an orphan's stays at its end) */
        return;
    }
    if (c->retries >= MAX_RETRIES) {
        if (c->state == TCP_SYN_RCVD && !c->port) {
            send_rst(c);
            pcb_free(vm, c);
        } else {
            abort_conn(vm, c, CR_TIMEOUT, c->state != TCP_SYN_SENT);
        }
        return;
    }
    c->retries++;
    net_s.st.tcp_retx++;
    xmit(c);
    timer_set(c, rto(c));
}

bool tcp_due(void) {
    bool any = net_s.tcp_held;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) any |= (net_s.tcp[i].flags & TF_TIMER) != 0;
    if (!any) return false;
    uint32_t now = now32();
    if (net_s.tcp_held && due(net_s.held_at, now)) return true;
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        const TcpPcb *c = &net_s.tcp[i];
        if ((c->flags & TF_TIMER) && due(c->deadline, now)) return true;
    }
    return false;
}

void tcp_poll(Vm *vm) {
    if (!tcp_due()) return;
    uint32_t now = now32();
    if (net_s.tcp_held && due(net_s.held_at, now)) {
        net_s.tcp_held = 0;
        for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
            Port *p = &vm->ports[i];
            bool ours = p->drv == &net_tcp_driver || (net_s.upper && p->drv == net_s.upper->drv);
            if (ours && p->state[1] && !flush(vm, p)) retry_later();
        }
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &net_s.tcp[i];
        if (c->state != TCP_CLOSED && (c->flags & TF_TIMER) && due(c->deadline, now)) fire(vm, c, now);
    }
}

bool tcp_next_deadline(uint64_t *deadline) {
    bool have = false;
    uint64_t now = 0;
    if (net_s.tcp_held) {
        now = platform_now_ms();
        int32_t left = (int32_t)(net_s.held_at - (uint32_t)now);
        *deadline = left > 0 ? now + (uint32_t)left : now;
        have = true;
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        const TcpPcb *c = &net_s.tcp[i];
        if (!(c->flags & TF_TIMER)) continue;
        if (!have) now = platform_now_ms();
        int32_t left = (int32_t)(c->deadline - (uint32_t)now);
        uint64_t d = left > 0 ? now + (uint32_t)left : now;
        if (!have || d < *deadline) *deadline = d;
        have = true;
    }
    return have;
}

bool tcp_any_orphan(void) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (net_s.tcp[i].flags & TF_ORPHAN) return true;
    }
    return false;
}

void tcp_down(Vm *vm) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        TcpPcb *c = &net_s.tcp[i];
        if (c->state == TCP_CLOSED) continue;
        if (c->port && c->state != TCP_SYN_RCVD) abort_conn(vm, c, CR_NOROUTE, false);
        else pcb_free(vm, c);
    }
}
