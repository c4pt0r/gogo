/* TCP (net/tcp.c): a table of (state, segment) -> (reply, next state,
 * event), plus timers on a fake clock, charges and Handoff, flow control,
 * the link going down, and malformed segments. Frames go in through
 * net_rx_offer and the port layer's drain, out through the unix
 * platform's test TX hook. */
#include "test.h"
#include "test_host.h"
#include "net_int.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "evq.h"
#include "bytes.h"
#include "signals.h"
#include "net_unix.h"
#include <string.h>

/* unix platform: forget memory earlier fixtures leaked (the MCU lane caps
 * the arena; the state table sets up a VM per row). */

/* ---- fixture ---- */

#define MAX_TX 16
static struct {
    int n;
    uint32_t len[MAX_TX];
    uint8_t f[MAX_TX][NET_FRAME_MAX];
} tx;

static int tx_hook(const uint8_t *f, uint32_t len) {
    if (tx.n < MAX_TX) {
        memcpy(tx.f[tx.n], f, len);
        tx.len[tx.n] = len;
    }
    tx.n++;
    return 0;
}

static uint64_t now_ms;
static uint64_t fake_clock(void) { return now_ms; }

static const uint8_t MY_MAC[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t PEER_MAC[6] = { 0x02, 0, 0, 0, 0, 0x99 };
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define MY_IP IP4(10, 0, 0, 2)
#define PEER_IP IP4(10, 0, 0, 1)
#define PEER_ISN 1000u

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm;
static NetState nst;     /* the stack under test (net_install) */

static void setup(void) {
    uint32_t *w = img_w;
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 16), 0);
    memset(&tx, 0, sizeof tx);
    now_ms = 0;
    test_host_set_clock(fake_clock);
    platform_unix_net_set_tx(tx_hook);
    net_install(&vm, &nst);
}

static void teardown(void) {
    port_close_all(&vm);
    net_uninstall(&vm);
    platform_unix_net_set_tx(NULL);
    test_host_set_clock(NULL);
}

static Actor *new_actor(void) {
    Actor *a = actor_new(&vm, 256);
    CHECK(a != NULL);
    a->state = A_WAITING;
    return a;
}

static term heap_str(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term heap_int(Actor *a, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    uint32_t off = heap_alloc(&vm, a, 2, NULL, 0);
    a->mem[off] = hdr_make(2, K_INT);
    a->mem[off + 1] = (uint32_t)v;
    return term_from_ref(off);
}

static term heap_struct(Actor *a, uint32_t type, const term *f, uint32_t n) {
    uint32_t off = heap_alloc(&vm, a, 2u + n, NULL, 0);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term heap_bytes(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    a->mem[off] = hdr_make(words, K_BYTES);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term open_port(Actor *a, const char *name, uint32_t type, const term *f, uint32_t n) {
    a->hp = 0;
    return port_open(&vm, a, heap_str(a, name), heap_struct(a, type, f, n));
}

static void command(Actor *from, term p, uint32_t type, term field, bool has) {
    from->hp = 0;
    port_command(&vm, from, p, heap_struct(from, type, &field, has ? 1u : 0u));
}

static void write_str(Actor *from, term p, const char *s) {
    from->hp = 0;
    term b = heap_bytes(from, s);
    port_command(&vm, from, p, heap_struct(from, TYPE_OUT, &b, 1));
}

static void drain(void) {
    if (port_work_pending(&vm)) port_drain(&vm, now_ms);
}

/* Moves the fake clock and runs due timers. */
static void advance(uint64_t ms) {
    now_ms += ms;
    drain();
}

/* ---- frames ---- */

static void rx_frame(const uint8_t *f, uint32_t len) {
    net_rx_offer(&nst, f, len);
    drain();
}

/* A segment from the peer (PEER_IP:sport -> MY_IP:dport), optionally with
 * the MSS option; returns its length. */
static uint32_t tcp_frame(uint8_t *b, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t fl,
                          uint16_t wnd, const char *data, bool mss) {
    uint32_t n = data ? (uint32_t)strlen(data) : 0u, hl = mss ? 24u : 20u;
    memcpy(b, MY_MAC, 6);
    memcpy(b + 6, PEER_MAC, 6);
    wr16(b + 12, ETH_IPV4);
    uint8_t *ip = b + 14, *t = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    wr16(ip + 2, 20 + hl + n);
    ip[8] = 64;
    ip[9] = IP_TCP;
    wr32(ip + 12, PEER_IP);
    wr32(ip + 16, MY_IP);
    wr16(ip + 10, (uint16_t)~net_fold(net_sum(0, ip, 20)));
    memset(t, 0, hl);
    wr16(t, sport);
    wr16(t + 2, dport);
    wr32(t + 4, seq);
    wr32(t + 8, ack);
    t[12] = (uint8_t)(hl << 2);
    t[13] = fl;
    wr16(t + 14, wnd);
    if (mss) { t[20] = 2; t[21] = 4; wr16(t + 22, 1000); }
    if (n) memcpy(t + hl, data, n);
    uint8_t ph[12];
    wr32(ph, PEER_IP); wr32(ph + 4, MY_IP); wr16(ph + 8, IP_TCP); wr16(ph + 10, hl + n);
    wr16(t + 16, (uint16_t)~net_fold(net_sum(net_sum(0, ph, 12), t, hl + n)));
    return 14 + 20 + hl + n;
}

static void seg_in(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t fl, const char *data) {
    uint8_t b[1600];
    rx_frame(b, tcp_frame(b, sport, dport, seq, ack, fl, 8192, data, false));
}

typedef struct Out {
    uint16_t sport, dport, wnd;
    uint32_t seq, ack, n;
    uint8_t fl;
    const uint8_t *data;
} Out;

/* Sent frame i as a TCP segment to the peer (checksums verified). */
static Out out_at(int i) {
    Out o;
    memset(&o, 0, sizeof o);
    const uint8_t *f = tx.f[i], *ip = f + 14;
    CHECK(i < tx.n && i < MAX_TX);
    CHECK(memcmp(f, PEER_MAC, 6) == 0);
    CHECK_EQ(ip[9], IP_TCP);
    CHECK_EQ(net_fold(net_sum(0, ip, 20)), 0xFFFF);
    CHECK_EQ(rd32(ip + 16), PEER_IP);
    uint32_t len = rd16(ip + 2) - 20u;
    const uint8_t *t = ip + 20;
    uint8_t ph[12];
    wr32(ph, MY_IP); wr32(ph + 4, PEER_IP); wr16(ph + 8, IP_TCP); wr16(ph + 10, len);
    CHECK_EQ(net_fold(net_sum(net_sum(0, ph, 12), t, len)), 0xFFFF);
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4u;
    o.sport = rd16(t);
    o.dport = rd16(t + 2);
    o.seq = rd32(t + 4);
    o.ack = rd32(t + 8);
    o.fl = t[13];
    o.wnd = rd16(t + 14);
    o.n = len - hl;
    o.data = t + hl;
    return o;
}

/* The head event of a's mailbox: its type (0 if none) and int field. */
static uint32_t event_of(Actor *a, int32_t *field) {
    Msg *m = a->mbox_head;
    if (!m) return 0;
    const uint32_t *d = m->data;
    uint32_t type = d[1];
    uint32_t i = hdr_words(d[0]) > 3u && term_is_pid(d[2]) ? 3u : 2u;   /* past a From */
    if (field) *field = hdr_words(d[0]) > i && term_is_int(d[i]) ? term_to_int(d[i]) : -1;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    a->mbox_words = (gogo_words_t)(a->mbox_words - msg_charge_words(&vm, m));
    msg_free(&vm, m);
    return type;
}

/* ---- connections in a given state ---- */

typedef struct Conn {
    Actor *o;               /* owner (also the listener's) */
    term port;              /* "tcp" port (T_NIL in SYN_RCVD) */
    TcpPcb *c;
    uint16_t pport, vport;  /* peer's and VM's port */
    uint32_t peer;          /* the peer's next sequence number (our rcv_nxt) */
    uint32_t una, nxt;      /* ours */
} Conn;

static Actor *netif_owner;

static void configure(void) {
    netif_owner = new_actor();
    term n = open_port(netif_owner, "netif", TYPE_NET_IFCONFIG, NULL, 0);
    netif_owner->hp = 0;
    term f[4] = { heap_int(netif_owner, (int32_t)MY_IP), heap_int(netif_owner, (int32_t)IP4(255, 255, 255, 0)),
                  heap_int(netif_owner, (int32_t)PEER_IP), term_from_int(0) };
    port_command(&vm, netif_owner, n, heap_struct(netif_owner, TYPE_NET_SETADDR, f, 4));
    uint8_t b[64];
    memcpy(b, MY_MAC, 6); memcpy(b + 6, PEER_MAC, 6); wr16(b + 12, ETH_ARP);
    uint8_t *p = b + 14;
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, 2);
    memcpy(p + 8, PEER_MAC, 6); wr32(p + 14, PEER_IP); memcpy(p + 18, MY_MAC, 6); wr32(p + 24, MY_IP);
    rx_frame(b, 42);
    tx.n = 0;
}

static TcpPcb *find_pcb(uint16_t vport) {
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) {
        if (nst.tcp[i].state != TCP_CLOSED && nst.tcp[i].lport == vport) return &nst.tcp[i];
    }
    return NULL;
}

static term dial(Actor *o, int32_t timeout) {
    term f[3] = { heap_int(o, (int32_t)PEER_IP), term_from_int(80), term_from_int(timeout) };
    o->hp = 0;
    f[0] = heap_int(o, (int32_t)PEER_IP);
    term p = port_open(&vm, o, heap_str(o, "tcp"), heap_struct(o, TYPE_NET_DIAL, f, 3));
    CHECK_EQ(tx.n, 0);                     /* the SYN waits for the first poll */
    drain();
    return p;
}

/* Drives a fresh connection into `state`; frames and events so far are
 * forgotten. */
static Conn mk(uint8_t state) {
    Conn k;
    memset(&k, 0, sizeof k);
    configure();
    k.o = new_actor();
    if (state == TCP_SYN_RCVD) {
        term f = term_from_int(8080);
        CHECK(test_port_is(open_port(k.o, "tcp_listen", TYPE_NET_LISTEN, &f, 1)));
        k.pport = 5555;
        k.vport = 8080;
        seg_in(k.pport, k.vport, PEER_ISN, 0, 0x02, NULL);
        k.peer = PEER_ISN + 1u;
        CHECK_EQ(tx.n, 1);
        Out s = out_at(0);
        CHECK_EQ(s.fl, 0x12);
        k.una = s.seq;
        k.nxt = s.seq + 1u;
        k.port = T_NIL;
        k.c = find_pcb(8080);
        tx.n = 0;
        return k;
    }
    k.port = dial(k.o, 0);
    CHECK(test_port_is(k.port));
    CHECK_EQ(tx.n, 1);
    Out s = out_at(0);
    k.pport = 80;
    k.vport = s.sport;
    k.una = s.seq;
    k.nxt = s.seq + 1u;
    k.c = find_pcb(k.vport);
    tx.n = 0;
    if (state == TCP_SYN_SENT) return k;
    seg_in(k.pport, k.vport, PEER_ISN, k.nxt, 0x12, NULL);
    k.una = k.nxt;
    k.peer = PEER_ISN + 1u;
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_CONNECTED);
    if (state == TCP_CLOSE_WAIT || state == TCP_LAST_ACK) {
        seg_in(k.pport, k.vport, k.peer, k.nxt, 0x11, NULL);
        k.peer++;
        CHECK_EQ(event_of(k.o, NULL), TYPE_CLOSED);
    }
    if (state == TCP_FIN_WAIT_1 || state == TCP_FIN_WAIT_2 || state == TCP_LAST_ACK ||
        state == TCP_CLOSING || state == TCP_TIME_WAIT) {
        command(k.o, k.port, TYPE_NET_SHUTDOWN, 0, false);
        k.nxt++;
    }
    if (state == TCP_FIN_WAIT_2 || state == TCP_TIME_WAIT) {
        seg_in(k.pport, k.vport, k.peer, k.nxt, 0x10, NULL);
        k.una = k.nxt;
    }
    if (state == TCP_CLOSING || state == TCP_TIME_WAIT) {
        seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
        k.peer++;
        CHECK_EQ(event_of(k.o, NULL), TYPE_CLOSED);
    }
    CHECK(k.c && k.c->state == state);
    tx.n = 0;
    return k;
}

/* ---- the state table ---- */

enum In {
    IN_SYNACK, IN_SYNACK_BAD, IN_RSTACK, IN_RST, IN_ACK, IN_ACK_BAD, IN_ACK_NONE, IN_DATA,
    IN_FIN, IN_FINACK, IN_SYN, IN_SYN_DUP, IN_OLD, IN_FIN_OLD, IN_RST_OFF, IN_ACK_FAR
};

typedef struct Row {
    uint8_t state;
    enum In in;
    uint8_t reply;          /* flags of the one segment sent, 0: none */
    uint8_t next;           /* state afterwards (TCP_CLOSED: slot freed) */
    uint32_t event;         /* to the owner, 0: none */
    int32_t reason;         /* Closed's */
} Row;

#define F_A 0x10u
#define F_R 0x04u
#define F_SA 0x12u
#define F_RA 0x14u

static const Row rows[] = {
    { TCP_SYN_SENT, IN_SYNACK, F_A, TCP_ESTABLISHED, TYPE_NET_CONNECTED, -1 },
    { TCP_SYN_SENT, IN_SYNACK_BAD, F_R, TCP_SYN_SENT, 0, -1 },
    { TCP_SYN_SENT, IN_RSTACK, 0, TCP_CLOSED, TYPE_CLOSED, GOGO_E_REFUSED },
    { TCP_SYN_SENT, IN_RST, 0, TCP_SYN_SENT, 0, -1 },
    { TCP_SYN_SENT, IN_ACK_BAD, F_R, TCP_SYN_SENT, 0, -1 },
    { TCP_SYN_SENT, IN_SYN, 0, TCP_SYN_SENT, 0, -1 },          /* no simultaneous open */
    { TCP_SYN_RCVD, IN_ACK, 0, TCP_ESTABLISHED, TYPE_NET_ACCEPTED, -1 },
    { TCP_SYN_RCVD, IN_ACK_BAD, F_R, TCP_SYN_RCVD, 0, -1 },
    { TCP_SYN_RCVD, IN_RST, 0, TCP_CLOSED, 0, -1 },
    { TCP_SYN_RCVD, IN_SYN_DUP, F_SA, TCP_SYN_RCVD, 0, -1 },
    { TCP_ESTABLISHED, IN_DATA, F_A, TCP_ESTABLISHED, TYPE_CHUNK, -1 },
    { TCP_ESTABLISHED, IN_FIN, F_A, TCP_CLOSE_WAIT, TYPE_CLOSED, GOGO_E_OK },
    { TCP_ESTABLISHED, IN_RST, 0, TCP_CLOSED, TYPE_CLOSED, GOGO_E_RESET },
    { TCP_ESTABLISHED, IN_RST_OFF, F_A, TCP_ESTABLISHED, 0, -1 },  /* RFC 5961 challenge ACK */
    { TCP_ESTABLISHED, IN_ACK_FAR, 0, TCP_ESTABLISHED, 0, -1 },    /* past a refused 1460 B + FIN */
    { TCP_ESTABLISHED, IN_SYN, F_A, TCP_ESTABLISHED, 0, -1 },  /* challenge ACK */
    { TCP_ESTABLISHED, IN_OLD, F_A, TCP_ESTABLISHED, 0, -1 },
    { TCP_ESTABLISHED, IN_ACK_BAD, F_A, TCP_ESTABLISHED, 0, -1 },
    { TCP_ESTABLISHED, IN_ACK, 0, TCP_ESTABLISHED, 0, -1 },
    { TCP_FIN_WAIT_1, IN_ACK, 0, TCP_FIN_WAIT_2, 0, -1 },
    { TCP_FIN_WAIT_1, IN_ACK_NONE, 0, TCP_FIN_WAIT_1, 0, -1 },
    { TCP_FIN_WAIT_1, IN_FIN, F_A, TCP_CLOSING, TYPE_CLOSED, GOGO_E_OK },
    { TCP_FIN_WAIT_1, IN_FINACK, F_A, TCP_TIME_WAIT, TYPE_CLOSED, GOGO_E_OK },
    { TCP_FIN_WAIT_1, IN_DATA, F_A, TCP_FIN_WAIT_1, TYPE_CHUNK, -1 },
    { TCP_FIN_WAIT_1, IN_RST, 0, TCP_CLOSED, TYPE_CLOSED, GOGO_E_RESET },
    { TCP_FIN_WAIT_2, IN_FIN, F_A, TCP_TIME_WAIT, TYPE_CLOSED, GOGO_E_OK },
    { TCP_FIN_WAIT_2, IN_DATA, F_A, TCP_FIN_WAIT_2, TYPE_CHUNK, -1 },
    { TCP_FIN_WAIT_2, IN_RST, 0, TCP_CLOSED, TYPE_CLOSED, GOGO_E_RESET },
    { TCP_CLOSE_WAIT, IN_DATA, F_A, TCP_CLOSE_WAIT, 0, -1 },   /* after the FIN: dropped */
    { TCP_CLOSE_WAIT, IN_RST, 0, TCP_CLOSED, TYPE_CLOSED, GOGO_E_RESET },
    { TCP_CLOSE_WAIT, IN_ACK, 0, TCP_CLOSE_WAIT, 0, -1 },
    { TCP_LAST_ACK, IN_ACK, 0, TCP_CLOSED, 0, -1 },
    { TCP_LAST_ACK, IN_ACK_NONE, 0, TCP_LAST_ACK, 0, -1 },
    { TCP_LAST_ACK, IN_RST, 0, TCP_CLOSED, 0, -1 },
    { TCP_CLOSING, IN_ACK, 0, TCP_TIME_WAIT, 0, -1 },
    { TCP_CLOSING, IN_RST, 0, TCP_CLOSED, 0, -1 },
    { TCP_TIME_WAIT, IN_FIN_OLD, F_A, TCP_TIME_WAIT, 0, -1 },
    { TCP_TIME_WAIT, IN_RST, 0, TCP_CLOSED, 0, -1 },
};

static void apply(const Conn *k, enum In in) {
    uint16_t s = k->pport, d = k->vport;
    switch (in) {
    case IN_SYNACK: seg_in(s, d, PEER_ISN, k->nxt, 0x12, NULL); break;
    case IN_SYNACK_BAD: seg_in(s, d, PEER_ISN, k->nxt + 5u, 0x12, NULL); break;
    case IN_RSTACK: seg_in(s, d, PEER_ISN, k->nxt, 0x14, NULL); break;
    case IN_RST: seg_in(s, d, k->peer, 0, 0x04, NULL); break;
    case IN_ACK: seg_in(s, d, k->peer, k->nxt, 0x10, NULL); break;
    case IN_ACK_BAD: seg_in(s, d, k->peer, k->nxt + 1000u, 0x10, NULL); break;
    case IN_ACK_NONE: seg_in(s, d, k->peer, k->una, 0x10, NULL); break;
    case IN_DATA: seg_in(s, d, k->peer, k->una, 0x18, "abc"); break;
    case IN_FIN: seg_in(s, d, k->peer, k->una, 0x11, NULL); break;
    case IN_FINACK: seg_in(s, d, k->peer, k->nxt, 0x11, NULL); break;
    case IN_SYN: seg_in(s, d, k->peer ? k->peer : PEER_ISN, 0, 0x02, NULL); break;
    case IN_SYN_DUP: seg_in(s, d, k->peer - 1u, 0, 0x02, NULL); break;
    case IN_OLD: seg_in(s, d, k->peer - 10u, k->una, 0x18, "0123456789"); break;
    case IN_FIN_OLD: seg_in(s, d, k->peer - 1u, k->nxt, 0x11, NULL); break;
    case IN_RST_OFF: seg_in(s, d, k->peer + 5u, 0, 0x04, NULL); break;
    case IN_ACK_FAR: seg_in(s, d, k->peer + TCP_MSS + 1u, k->nxt, 0x10, NULL); break;
    }
}

TEST(tcp_state_table) {
    for (uint32_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const Row *r = &rows[i];
        setup();
        Conn k = mk(r->state);
        CHECK(k.c != NULL);
        uint32_t slot = (uint32_t)(k.c - nst.tcp);
        apply(&k, r->in);
        if (r->reply) {
            CHECK_EQ(tx.n, 1);
            if (tx.n == 1) {
                Out s = out_at(0);
                if (s.fl != r->reply) printf("  row %u: reply %#x, want %#x\n", (unsigned)i, s.fl, r->reply);
                CHECK_EQ(s.fl, r->reply);
                CHECK_EQ(s.sport, k.vport);
                CHECK_EQ(s.dport, k.pport);
            }
        } else {
            if (tx.n) printf("  row %u: %d segments, want none\n", (unsigned)i, tx.n);
            CHECK_EQ(tx.n, 0);
        }
        uint8_t st = nst.tcp[slot].state;
        if (st != r->next) printf("  row %u: state %u, want %u\n", (unsigned)i, st, r->next);
        CHECK_EQ(st, r->next);
        int32_t f = -1;
        CHECK_EQ(event_of(k.o, &f), r->event);
        if (r->event == TYPE_CLOSED) CHECK_EQ(f, r->reason);
        CHECK_EQ(event_of(k.o, NULL), 0);
        teardown();
    }
}

/* Every reply carries the right numbers (the table checks flags). */
TEST(tcp_reply_numbers) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    apply(&k, IN_DATA);
    Out s = out_at(0);
    CHECK_EQ(s.seq, k.nxt);
    CHECK_EQ(s.ack, k.peer + 3u);
    CHECK_EQ(s.wnd, TCP_MSS);
    tx.n = 0;
    apply(&k, IN_OLD);                     /* old: ACKs what it has, delivers nothing */
    s = out_at(0);
    CHECK_EQ(s.ack, k.peer + 3u);
    CHECK_EQ(event_of(k.o, NULL), TYPE_CHUNK);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
    /* RST to a segment without a connection: seq from its ACK, or
     * RST|ACK acknowledging it. */
    setup();
    configure();
    seg_in(4000, 81, 77, 555, 0x10, NULL);
    s = out_at(0);
    CHECK_EQ(s.fl, F_R);
    CHECK_EQ(s.seq, 555u);
    seg_in(4000, 81, 77, 0, 0x02, NULL);
    s = out_at(1);
    CHECK_EQ(s.fl, F_RA);
    CHECK_EQ(s.ack, 78u);
    seg_in(4000, 81, 77, 0, 0x04, NULL);   /* never answer a RST */
    CHECK_EQ(tx.n, 2);
    CHECK_EQ(nst.st.tcp_noconn, 3u);
    teardown();
}

/* RTO 1 s doubling to 8 s, six retransmissions, then RST and Timeout. */
TEST(tcp_retransmit_timer) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    write_str(k.o, k.port, "hello");
    CHECK_EQ(tx.n, 1);
    const uint32_t at[] = { 1000, 3000, 7000, 15000, 23000, 31000 };
    uint64_t t0 = now_ms;
    for (uint32_t i = 0; i < 6; i++) {
        now_ms = t0 + at[i] - 1u;
        drain();
        CHECK_EQ(tx.n, (int)(1u + i));
        advance(1);
        CHECK_EQ(tx.n, (int)(2u + i));
        Out s = out_at((int)(1u + i));
        CHECK_EQ(s.seq, k.nxt);
        CHECK_EQ(s.n, 5u);
        CHECK(memcmp(s.data, "hello", 5) == 0);
    }
    now_ms = t0 + 39000u - 1u;
    drain();
    CHECK_EQ(tx.n, 7);
    advance(1);
    CHECK_EQ(tx.n, 8);
    CHECK_EQ(out_at(7).fl, F_RA);
    int32_t f;
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);   /* the Out in flight, not sent */
    CHECK_EQ(f, -1);
    CHECK_EQ(event_of(k.o, &f), TYPE_CLOSED);
    CHECK_EQ(f, GOGO_E_TIMEOUT);
    CHECK_EQ(nst.st.tcp_retx, 6u);
    CHECK(k.c->state == TCP_CLOSED);
    write_str(k.o, k.port, "late");         /* the port has no connection: refused */
    CHECK_EQ(tx.n, 8);
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    teardown();
}

/* Connect timeout (TimeoutMs) and SYN retransmission; TIME_WAIT 2 s; an
 * orphan in FIN_WAIT_2 gives up after 4 s. */
TEST(tcp_other_timers) {
    setup();
    configure();
    Actor *o = new_actor();
    term p = dial(o, 2500);
    CHECK(test_port_is(p));
    advance(1000);
    CHECK_EQ(tx.n, 2);                     /* SYN, retransmitted SYN */
    CHECK_EQ(out_at(1).fl, 0x02);
    advance(1499);
    CHECK_EQ(event_of(o, NULL), 0);
    advance(1);
    int32_t f;
    CHECK_EQ(event_of(o, &f), TYPE_CLOSED);
    CHECK_EQ(f, GOGO_E_TIMEOUT);
    CHECK_EQ(tx.n, 2);                     /* no RST for a SYN never answered */
    teardown();

    setup();
    Conn k = mk(TCP_TIME_WAIT);
    CHECK(k.c->port == NULL);              /* the port has no connection any more */
    advance(1999);
    CHECK(k.c->state == TCP_TIME_WAIT);
    advance(1);
    CHECK(k.c->state == TCP_CLOSED);
    teardown();

    setup();
    k = mk(TCP_FIN_WAIT_2);
    port_close(&vm, k.port);               /* after Shutdown: orphan */
    CHECK_EQ(tx.n, 0);
    CHECK(k.c->state == TCP_FIN_WAIT_2 && k.c->port == NULL);
    CHECK(tcp_any_orphan(&nst));
    advance(3999);
    CHECK_EQ(tx.n, 0);
    advance(1);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, F_RA);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK(!tcp_any_orphan(&nst));
    teardown();
}

/* The control block is charged to the owner, Handoff moves the charge and
 * the pinned Write data, and the new owner gets the events. */
TEST(tcp_charge_and_handoff) {
    setup();
    CHECK(sizeof(TcpPcb) <= 64u);
    printf("  tcp pcb %u bytes, listen entry %u bytes\n", (unsigned)sizeof(TcpPcb), (unsigned)sizeof(TcpListen));
    configure();
    Actor *o = new_actor(), *h = new_actor();
    uint32_t c0 = actor_charged_words(o), h0 = actor_charged_words(h);
    term p = dial(o, 0);
    CHECK_EQ(actor_charged_words(o), c0 + TCP_PCB_WORDS);
    Out s = out_at(0);
    seg_in(80, s.sport, PEER_ISN, s.seq + 1u, 0x12, NULL);
    CHECK_EQ(event_of(o, NULL), TYPE_NET_CONNECTED);
    write_str(o, p, "data");
    uint32_t c1 = actor_charged_words(o);
    CHECK(c1 > c0 + TCP_PCB_WORDS);        /* the copy is held for o */
    CHECK(port_handoff(&vm, o, p, h->id));
    CHECK_EQ(actor_charged_words(o), c0);
    CHECK_EQ(actor_charged_words(h), h0 + (c1 - c0));
    seg_in(80, s.sport, PEER_ISN + 1u, s.seq + 5u, 0x10, NULL);   /* ACK of "data" */
    CHECK_EQ(event_of(h, NULL), TYPE_DONE);
    CHECK_EQ(event_of(o, NULL), 0);
    CHECK_EQ(actor_charged_words(h), h0 + TCP_PCB_WORDS);
    port_close(&vm, p);
    CHECK_EQ(actor_charged_words(h), h0);
    teardown();
}

/* Two Outs may be in flight; a third gets Done{-1} and the connection
 * carries on. An Out before Connected waits; an empty one gets Done{-1}.
 * Each Done says From: the port. */
TEST(tcp_write_flow_control) {
    setup();
    Conn k = mk(TCP_SYN_SENT);
    int32_t f;
    write_str(k.o, k.port, "");
    CHECK(k.o->mbox_head && k.o->mbox_head->data[2] == k.port);   /* From */
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    write_str(k.o, k.port, "one");
    CHECK_EQ(tx.n, 0);
    seg_in(80, k.vport, PEER_ISN, k.nxt, 0x12, NULL);
    CHECK_EQ(tx.n, 2);                     /* ACK, then "one" */
    Out s = out_at(1);
    CHECK_EQ(s.n, 3u);
    write_str(k.o, k.port, "two");
    CHECK_EQ(tx.n, 2);                     /* one segment in flight */
    write_str(k.o, k.port, "three");
    CHECK_EQ(tx.n, 2);                     /* refused: nothing sent, no RST */
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_CONNECTED);
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    CHECK(k.c->state == TCP_ESTABLISHED);
    seg_in(80, k.vport, PEER_ISN + 1u, s.seq + 3u, 0x10, NULL);   /* ACK of "one" */
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, 1);
    CHECK_EQ(tx.n, 3);                     /* "two" */
    CHECK_EQ(out_at(2).n, 3u);
    teardown();
}

/* A peer advertising a zero window gets 1-byte probes; a bigger window,
 * MSS-sized segments. */
TEST(tcp_zero_window) {
    setup();
    configure();
    Actor *o = new_actor();
    term p = dial(o, 0);
    Out s = out_at(0);
    uint8_t b[1600];
    rx_frame(b, tcp_frame(b, 80, s.sport, PEER_ISN, s.seq + 1u, 0x12, 0, NULL, true));
    write_str(o, p, "abcdef");
    CHECK_EQ(tx.n, 3);
    Out d = out_at(2);
    CHECK_EQ(d.n, 1u);
    CHECK_EQ(d.data[0], 'a');
    rx_frame(b, tcp_frame(b, 80, s.sport, PEER_ISN + 1u, d.seq + 1u, 0x10, 8192, NULL, false));
    CHECK_EQ(tx.n, 4);
    d = out_at(3);
    CHECK_EQ(d.n, 5u);
    CHECK(memcmp(d.data, "bcdef", 5) == 0);
    teardown();
}

/* Accepted connections are paused until Resume (or Handoff). */
TEST(tcp_resume) {
    setup();
    Conn k = mk(TCP_SYN_RCVD);
    apply(&k, IN_ACK);
    CHECK_EQ(tx.n, 0);
    Msg *m = k.o->mbox_head;
    CHECK(m && m->data[1] == TYPE_NET_ACCEPTED);
    term conn = m->data[2];
    CHECK(test_port_is(conn));
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_ACCEPTED);
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "early");
    CHECK_EQ(tx.n, 1);
    Out s = out_at(0);
    CHECK_EQ(s.wnd, 0);
    CHECK_EQ(s.ack, k.peer);               /* not accepted */
    CHECK_EQ(event_of(k.o, NULL), 0);
    command(k.o, conn, TYPE_NET_RESUME, 0, false);
    CHECK_EQ(tx.n, 2);
    CHECK_EQ(out_at(1).wnd, TCP_MSS);
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "early");
    CHECK_EQ(out_at(2).ack, k.peer + 5u);
    CHECK_EQ(event_of(k.o, NULL), TYPE_CHUNK);
    teardown();
}

/* The interface going down ends every connection with NoRoute. */
TEST(tcp_link_down) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    net_link_event(&nst, false);
    drain();
    int32_t f;
    CHECK_EQ(event_of(k.o, &f), TYPE_CLOSED);
    CHECK_EQ(f, GOGO_E_NOROUTE);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK_EQ(tx.n, 0);
    /* While it is down a Dial fails at once (no route). */
    term fl[3] = { term_from_int(0), term_from_int(80), term_from_int(0) };
    k.o->hp = 0;
    fl[0] = heap_int(k.o, (int32_t)PEER_IP);
    CHECK(!test_port_is(port_open(&vm, k.o, heap_str(k.o, "tcp"), heap_struct(k.o, TYPE_NET_DIAL, fl, 3))));
    teardown();
}

/* Short, bad checksum, bad data offset, to a broadcast address: dropped
 * and counted, nothing sent. */
TEST(tcp_malformed) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    uint8_t b[1600];
    uint32_t len = tcp_frame(b, 80, k.vport, k.peer, k.nxt, 0x18, 8192, "abc", false);
    b[len - 1] ^= 0xFFu;                   /* checksum */
    rx_frame(b, len);
    len = tcp_frame(b, 80, k.vport, k.peer, k.nxt, 0x18, 8192, "abc", false);
    b[14 + 20 + 12] = 0x40;                /* data offset 16 < 20 */
    rx_frame(b, len);
    len = tcp_frame(b, 80, k.vport, k.peer, k.nxt, 0x18, 8192, "abc", false);
    b[14 + 20 + 12] = 0xF0;                /* data offset 60 > segment */
    rx_frame(b, len);
    len = tcp_frame(b, 80, k.vport, k.peer, k.nxt, 0x18, 8192, NULL, false);
    wr16(b + 14 + 2, 20 + 10);             /* IP says 10 bytes of TCP */
    b[14 + 10] = 0; b[14 + 11] = 0;
    wr16(b + 14 + 10, (uint16_t)~net_fold(net_sum(0, b + 14, 20)));
    rx_frame(b, len);
    CHECK_EQ(nst.st.tcp_bad, 4u);
    CHECK_EQ(tx.n, 0);
    CHECK_EQ(event_of(k.o, NULL), 0);
    /* Every truncation of a valid segment: dropped without a crash. */
    len = tcp_frame(b, 80, k.vport, k.peer, k.nxt, 0x18, 8192, "abcdef", true);
    for (uint32_t cut = 14; cut < len; cut++) rx_frame(b, cut);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* Writes "req" from a handle o keeps holding, so the acknowledgement frees
 * no charge (the reader stays as full as it was). */
static void write_held(Actor *o, term p) {
    uint8_t *w;
    term h = bytes_new(&vm, o, 3, &w);
    CHECK(h != T_NIL);
    memcpy(w, "req", 3);
    o->hp = 0;
    port_command(&vm, o, p, heap_struct(o, TYPE_OUT, &h, 1));
}

/* A slow reader whose mailbox the Data filled to its quota is never
 * killed by the control events that follow (Written, Closed{Reset}): Data
 * leaves headroom for them, and an event that still does not fit waits
 * and is retried until the reader has room. */
TEST(tcp_slow_reader_never_killed) {
    static char big[201];
    memset(big, 'y', 200);
    for (uint32_t slack = 100; slack < 400; slack += 7) {
        setup();
        Conn k = mk(TCP_ESTABLISHED);
        write_held(k.o, k.port);
        k.o->max_words = (gogo_words_t)(k.o->mem_words + actor_charged_words(k.o) + slack);
        uint32_t delivered = 0;
        for (int i = 0; i < 100; i++) {
            uint32_t before = nst.st.tcp_quota;
            seg_in(k.pport, k.vport, k.peer, k.una, 0x18, big);
            if (nst.st.tcp_quota != before) break;
            k.peer += 200;
            delivered++;
        }
        CHECK(delivered < 100);
        seg_in(k.pport, k.vport, k.peer, k.nxt + 3u, 0x10, NULL);   /* the peer ACKs "req" */
        CHECK(!(k.o->flags & AF_EXIT_PENDING));
        seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);            /* and resets */
        CHECK(!(k.o->flags & AF_EXIT_PENDING));
        /* The reader reads everything, then whatever was held arrives. */
        uint32_t n = 0, written = 0, closed = 0;
        int32_t f = -1;
        for (uint32_t t; (t = event_of(k.o, &f)) != 0; n++) {
            if (t == TYPE_DONE) written++;
            if (t == TYPE_CLOSED) { closed++; CHECK_EQ(f, GOGO_E_RESET); }
        }
        advance(1000);                       /* retried events, if any were held */
        for (uint32_t t; (t = event_of(k.o, &f)) != 0; n++) {
            if (t == TYPE_DONE) written++;
            if (t == TYPE_CLOSED) { closed++; CHECK_EQ(f, GOGO_E_RESET); }
        }
        CHECK_EQ(written, 1);
        CHECK_EQ(closed, 1);
        CHECK_EQ(n, delivered + 2u);
        teardown();
    }
}

/* Events that do not fit are held in the port and retried. */
TEST(tcp_event_retry) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    write_held(k.o, k.port);
    k.o->max_words = (gogo_words_t)(k.o->mem_words + actor_charged_words(k.o));   /* no room at all */
    seg_in(k.pport, k.vport, k.peer, k.nxt + 3u, 0x10, NULL);
    seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);
    CHECK(!(k.o->flags & AF_EXIT_PENDING));
    CHECK_EQ(event_of(k.o, NULL), 0);
    CHECK(port_any_live(&vm));
    k.o->max_words = 4096;
    advance(100);
    int32_t f = -1;
    CHECK_EQ(event_of(k.o, NULL), TYPE_DONE);
    CHECK_EQ(event_of(k.o, &f), TYPE_CLOSED);
    CHECK_EQ(f, GOGO_E_RESET);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* An empty Out is refused at once (Done{-1}); the Out before it goes on. */
TEST(tcp_empty_out_refused) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    int32_t f;
    write_str(k.o, k.port, "abc");
    write_str(k.o, k.port, "");
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    CHECK_EQ(event_of(k.o, NULL), 0);
    seg_in(k.pport, k.vport, k.peer, k.nxt + 3u, 0x10, NULL);
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, 1);
    CHECK_EQ(event_of(k.o, NULL), 0);
    command(k.o, k.port, TYPE_NET_SHUTDOWN, 0, false);
    CHECK_EQ(out_at(tx.n - 1).fl, 0x11);   /* FIN once "abc" is done */
    teardown();
}

/* A Dial the port layer refuses (the owner cannot take the charge)
 * sends nothing and takes no slot. */
TEST(tcp_refused_dial_sends_nothing) {
    setup();
    configure();
    Actor *o = new_actor();
    o->max_words = (gogo_words_t)(o->mem_words + actor_charged_words(o) + 1u);
    term f[3] = { term_from_int(0), term_from_int(80), term_from_int(0) };
    o->hp = 0;
    f[0] = heap_int(o, (int32_t)PEER_IP);
    term p = port_open(&vm, o, heap_str(o, "tcp"), heap_struct(o, TYPE_NET_DIAL, f, 3));
    CHECK(!test_port_is(p));
    advance(10);
    CHECK_EQ(tx.n, 0);
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP; i++) CHECK_EQ(nst.tcp[i].state, TCP_CLOSED);
    teardown();
}

/* SetAddr to another address ends the connections (NoRoute). */
TEST(tcp_address_change) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    term n = T_NIL;
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        if ((*port_at(&vm, i)).drv && (*port_at(&vm, i)).owner == netif_owner->id) n = (*port_at(&vm, i)).id;
    }
    netif_owner->hp = 0;
    term f[4] = { heap_int(netif_owner, (int32_t)IP4(10, 0, 0, 3)), heap_int(netif_owner, (int32_t)IP4(255, 255, 255, 0)),
                  heap_int(netif_owner, (int32_t)PEER_IP), term_from_int(0) };
    port_command(&vm, netif_owner, n, heap_struct(netif_owner, TYPE_NET_SETADDR, f, 4));
    int32_t r;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_NOROUTE);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK_EQ(actor_charged_words(k.o), 0);
    teardown();
}

/* An orphan lives at most GOGO_NET_ORPHAN_MS from its close, however it
 * progresses: a peer with a 1-byte window that acknowledges each byte
 * just before the retransmission timer (which would otherwise keep it, and
 * its uncharged data, forever) gets RST at the deadline, and the data's
 * handles are released. */
TEST(tcp_orphan_lifetime) {
    setup();
    uint32_t handles0 = bytes_table_used(&vm);
    Conn k = mk(TCP_ESTABLISHED);
    uint8_t b[1600];
    rx_frame(b, tcp_frame(b, k.pport, k.vport, k.peer, k.una, 0x10, 1, NULL, false));   /* window 1 */
    static char data[201];
    memset(data, 'z', 200);
    write_str(k.o, k.port, data);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).n, 1u);
    CHECK(bytes_table_used(&vm) > handles0);
    command(k.o, k.port, TYPE_NET_SHUTDOWN, 0, false);
    port_close(&vm, k.port);                 /* after Shutdown: orphan */
    CHECK(k.c->port == NULL && tcp_any_orphan(&nst));
    uint32_t una = out_at(0).seq;
    uint64_t closed_at = now_ms;
    uint32_t acked = 0;
    while (now_ms + 900u < closed_at + GOGO_NET_ORPHAN_MS) {
        advance(900);                        /* before the 1 s retransmission */
        tx.n = 0;
        una++;
        acked++;
        rx_frame(b, tcp_frame(b, k.pport, k.vport, k.peer, una, 0x10, 1, NULL, false));
        CHECK_EQ(tx.n, 1);                   /* the next byte */
        CHECK_EQ(out_at(0).n, 1u);
        CHECK(k.c->state == TCP_ESTABLISHED);
    }
    CHECK(acked > 10u && acked < 200u);      /* progressing, far from done */
    tx.n = 0;
    advance(closed_at + GOGO_NET_ORPHAN_MS - 1u - now_ms);
    CHECK(k.c->state == TCP_ESTABLISHED);
    CHECK_EQ(tx.n, 0);
    advance(1);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, F_RA);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK(!tcp_any_orphan(&nst));
    CHECK_EQ(bytes_table_used(&vm), handles0);
    teardown();
}

/* Inbound connections never take the last free slot: with 4 slots, 3
 * browsers' connections are accepted, a 4th SYN is dropped (no reply; it
 * retries), and the device's own Dial still gets a slot. */
TEST(tcp_inbound_keeps_a_slot_for_dial) {
    setup();
    configure();
    Actor *o = new_actor();
    term f = term_from_int(8080);
    CHECK(test_port_is(open_port(o, "tcp_listen", TYPE_NET_LISTEN, &f, 1)));
    uint32_t drops = nst.st.tcp_syn_drop;
    for (uint16_t i = 0; i < GOGO_NET_MAX_TCP; i++) seg_in((uint16_t)(5000 + i), 8080, PEER_ISN, 0, 0x02, NULL);
    CHECK_EQ(tx.n, (int)GOGO_NET_MAX_TCP - 1);           /* SYN-ACKs; none for the last */
    CHECK_EQ(nst.st.tcp_syn_drop, drops + 1u);
    for (int i = 0; i < (int)GOGO_NET_MAX_TCP - 1; i++) {
        Out s = out_at(i);
        CHECK_EQ(s.fl, F_SA);
        seg_in(s.dport, 8080, PEER_ISN + 1u, s.seq + 1u, 0x10, NULL);
        CHECK_EQ(event_of(o, NULL), TYPE_NET_ACCEPTED);
    }
    for (uint32_t i = 0; i < GOGO_NET_MAX_TCP - 1u; i++) {
        CHECK(nst.tcp[i].state == TCP_ESTABLISHED);
    }
    tx.n = 0;
    seg_in(6000, 8080, PEER_ISN, 0, 0x02, NULL);          /* still refused */
    CHECK_EQ(tx.n, 0);
    Actor *d = new_actor();
    term p = dial(d, 0);
    CHECK(test_port_is(p));
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, 0x02);                         /* our SYN */
    teardown();
}

/* An owner that cannot take its Dones: they are owed in the port's
 * StreamOut, an owed Done keeps its Out's place among the two in flight
 * (a further Out is refused, owed too), and they come out in order once
 * it has room. Nothing is lost and the owner is never killed. */
TEST(tcp_owed_dones) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    write_str(k.o, k.port, "a");
    write_str(k.o, k.port, "b");
    CHECK_EQ(tx.n, 1);
    k.o->max_words = (gogo_words_t)(k.o->mem_words + actor_charged_words(k.o));   /* no room at all */
    seg_in(k.pport, k.vport, k.peer, k.nxt + 1u, 0x10, NULL);   /* ACK of "a": its Done owed */
    CHECK_EQ(tx.n, 2);                       /* "b" goes out */
    CHECK(out_at(1).n == 1u && out_at(1).data[0] == 'b');
    write_str(k.o, k.port, "c");             /* "b" in flight + an owed Done: refused */
    CHECK(!(k.o->flags & AF_EXIT_PENDING));
    CHECK_EQ(event_of(k.o, NULL), 0);
    CHECK(k.c->state == TCP_ESTABLISHED);
    k.o->max_words = 4096;
    advance(100);
    int32_t f;
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, 1);
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    CHECK_EQ(event_of(k.o, NULL), 0);
    seg_in(k.pport, k.vport, k.peer, k.nxt + 2u, 0x10, NULL);   /* ACK of "b" */
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, 2);
    teardown();
}

/* A Done never overtakes a held Connected: with Connected held for a full
 * mailbox, an Out refused once the owner has room again is owed behind it,
 * and both come out in order. */
TEST(tcp_done_after_held_connected) {
    setup();
    Conn k = mk(TCP_SYN_SENT);
    uint16_t room = k.o->max_words;
    k.o->max_words = (gogo_words_t)(k.o->mem_words + actor_charged_words(k.o));   /* no room at all */
    seg_in(80, k.vport, PEER_ISN, k.nxt, 0x12, NULL);   /* SYN-ACK: Connected held */
    CHECK_EQ(event_of(k.o, NULL), 0);
    k.o->max_words = room;
    write_str(k.o, k.port, "");             /* refused: its Done{-1} follows Connected */
    int32_t f;
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_CONNECTED);
    CHECK_EQ(event_of(k.o, &f), TYPE_DONE);
    CHECK_EQ(f, -1);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* ---- an upper layer over TCP (TcpUpper; runtime/net/tls/tls.c is the real one) ---- */

static struct {
    int connected, ended, fins, drains;
    bool refuse_rx, refuse_fin, pend;
    int32_t fin_post;       /* >= 0: fin posts Closed{fin_post} itself */
    bool closed;            /* the layer posted its terminal Closed */
    uint8_t rx[256];
    uint32_t rx_n;
    const uint8_t *tx;
    uint32_t tx_len, acked;
} up;

static bool up_rx(Vm *v, TcpPcb *c, const uint8_t *p, uint32_t n) {
    (void)v; (void)c;
    if (up.refuse_rx || up.rx_n + n > sizeof up.rx) return false;
    memcpy(up.rx + up.rx_n, p, n);
    up.rx_n += n;
    return true;
}
static bool up_fin(Vm *v, TcpPcb *c) {
    up.fins++;
    if (up.fin_post >= 0) {
        tcp_post(v, c, TYPE_CLOSED, up.fin_post);
        if (up.fin_post != GOGO_E_OK) up.closed = true;
    }
    return !up.refuse_fin;
}
static bool up_closed(const NetState *ns, const TcpPcb *c) { (void)ns; (void)c; return up.closed; }
static void up_connected(Vm *v, TcpPcb *c) { (void)v; (void)c; up.connected++; }
static void up_acked(Vm *v, TcpPcb *c, uint32_t n) {
    (void)v; (void)c;
    up.acked += n;
    up.tx += n;
    up.tx_len -= n;
}
static const uint8_t *up_tx(const NetState *ns, const TcpPcb *c, uint32_t *len) {
    (void)ns; (void)c;
    *len = up.tx_len;
    return up.tx;
}
static void up_ended(NetState *ns, TcpPcb *c) { (void)ns; (void)c; up.ended++; }
static bool up_pending(const NetState *ns) { (void)ns; return up.pend; }
static void up_drain(Vm *v) { (void)v; up.pend = false; up.drains++; }

static int up_open(Vm *v, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
static void up_close(Vm *v, Port *p) {
    TcpPcb *c = tcp_pcb_of(v->net, p);
    if (c && (c->flags & TF_UPPER)) {
        c->flags &= (uint8_t)~TF_UPPER;
        up.ended++;
    }
    tcp_close_port(v, p);
}
static const PortDriver up_driver = {
    .name = "up", .cfg_type = TYPE_NET_DIAL, .event_type = TYPE_CHUNK, .open = up_open, .close = up_close,
    .can_event = tcp_port_can_event
};
static const TcpUpper upper = {
    &up_driver, up_rx, up_fin, up_connected, up_acked, up_tx, up_ended, up_pending, up_drain, NULL, up_closed
};
static int up_open(Vm *v, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    TcpPcb *c = tcp_dial(v->net, p, cfg, n);
    if (!c) return -1;
    c->flags |= TF_UPPER;
    v->net->upper = &upper;
    return 0;
}
static const PortDriver *const up_list[] = { &net_netif_driver, &net_tcp_driver, &up_driver, NULL };

/* Opens "up" to PEER_IP:80 and completes the TCP handshake. */
static Conn up_dial(int32_t timeout) {
    Conn k;
    memset(&k, 0, sizeof k);
    memset(&up, 0, sizeof up);
    up.fin_post = -1;
    port_set_drivers(&vm, up_list);
    configure();
    k.o = new_actor();
    term f[3] = { heap_int(k.o, (int32_t)PEER_IP), term_from_int(80), term_from_int(timeout) };
    k.o->hp = 0;
    f[0] = heap_int(k.o, (int32_t)PEER_IP);
    k.port = port_open(&vm, k.o, heap_str(k.o, "up"), heap_struct(k.o, TYPE_NET_DIAL, f, 3));
    CHECK(test_port_is(k.port));
    drain();
    CHECK_EQ(tx.n, 1);
    Out s = out_at(0);
    k.pport = 80;
    k.vport = s.sport;
    k.nxt = s.seq + 1u;
    k.c = find_pcb(k.vport);
    seg_in(k.pport, k.vport, PEER_ISN, k.nxt, 0x12, NULL);
    k.una = k.nxt;
    k.peer = PEER_ISN + 1u;
    tx.n = 0;
    return k;
}

/* The stream goes to the upper layer instead of the owner: Connected,
 * received bytes and the peer's FIN are its to take or refuse (not
 * ACKed), what it offers is sent and its acknowledgment reported; the
 * source hooks run in the stack's drain; the end of the connection frees
 * it and the owner gets Closed. */
TEST(tcp_upper_layer) {
    setup();
    Conn k = up_dial(0);
    CHECK_EQ(up.connected, 1);
    CHECK_EQ(event_of(k.o, NULL), 0);          /* Connected is the layer's to send */
    CHECK(k.c->state == TCP_ESTABLISHED);
    tcp_post(&vm, k.c, TYPE_NET_CONNECTED, -1);
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_CONNECTED);
    /* Sending: the layer's bytes, MSS-sized, acknowledgments reported. */
    static const char msg[] = "hello, upper";
    up.tx = (const uint8_t *)msg;
    up.tx_len = 12;
    tcp_output(&vm, k.c);
    CHECK_EQ(tx.n, 1);
    Out s = out_at(0);
    CHECK_EQ(s.n, 12u);
    CHECK(memcmp(s.data, msg, 12) == 0);
    seg_in(k.pport, k.vport, k.peer, k.una + 5u, 0x10, NULL);   /* a partial ACK */
    CHECK_EQ(up.acked, 5u);
    seg_in(k.pport, k.vport, k.peer, k.una + 12u, 0x10, NULL);
    CHECK_EQ(up.acked, 12u);
    k.una += 12u;
    k.nxt = k.una;
    CHECK_EQ(event_of(k.o, NULL), 0);          /* no Written: the layer reports its own */
    /* Receiving: taken and ACKed, or refused and not. */
    tx.n = 0;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x18, "abc");
    CHECK_EQ(up.rx_n, 3u);
    CHECK_EQ(out_at(0).ack, k.peer + 3u);
    k.peer += 3u;
    CHECK_EQ(event_of(k.o, NULL), 0);          /* no Data to the owner */
    up.refuse_rx = true;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x18, "def");
    CHECK_EQ(up.rx_n, 3u);
    CHECK_EQ(out_at(tx.n - 1).ack, k.peer);
    up.refuse_rx = false;
    /* The peer's FIN: the layer may refuse it (not ACKed) or take it. */
    up.refuse_fin = true;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
    CHECK_EQ(up.fins, 1);
    CHECK_EQ(out_at(tx.n - 1).ack, k.peer);
    CHECK(k.c->state == TCP_ESTABLISHED);
    up.refuse_fin = false;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
    CHECK_EQ(out_at(tx.n - 1).ack, k.peer + 1u);
    CHECK(k.c->state == TCP_CLOSE_WAIT);
    CHECK_EQ(event_of(k.o, NULL), 0);          /* Closed{Normal} is the layer's too */
    k.peer++;
    /* The source hooks. */
    up.pend = true;
    CHECK(port_work_pending(&vm));
    drain();
    CHECK_EQ(up.drains, 1);
    /* A reset ends it: the layer is freed, the owner gets Closed{Reset}. */
    seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);
    CHECK_EQ(up.ended, 1);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_RESET);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK_EQ(actor_charged_words(k.o), 0);
    teardown();
}

/* With an upper layer, TimeoutMs bounds the connect and the layer's own
 * handshake: it ends with RST and Closed{Timeout} unless the layer clears
 * the deadline (tcp_until_clear) first. */
TEST(tcp_upper_deadline) {
    setup();
    Conn k = up_dial(5000);
    advance(4999);
    CHECK(k.c->state == TCP_ESTABLISHED);
    tx.n = 0;
    advance(1);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, F_RA);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_TIMEOUT);
    CHECK_EQ(up.ended, 1);
    teardown();
    setup();
    k = up_dial(5000);
    tcp_until_clear(&nst, k.c);
    advance(60000);
    CHECK(k.c->state == TCP_ESTABLISHED);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* A port with no connection can still be given a Closed{reason}
 * (net.NoTime: the tls port's Dial before the clock is set). */
static int nc_open(Vm *v, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index; (void)cfg; (void)n;
    tcp_hold_closed(v->net, p, GOGO_E_NOTIME);
    return 0;
}
static const PortDriver nc_driver = {
    .name = "nc", .cfg_type = TYPE_NET_DIAL, .event_type = TYPE_CHUNK, .open = nc_open,
    .close = tcp_close_port, .can_event = tcp_port_can_event
};
static const TcpUpper nc_upper = { &nc_driver, NULL, NULL, NULL, NULL, NULL, NULL, up_pending, up_drain, NULL, NULL };
static const PortDriver *const nc_list[] = { &nc_driver, NULL };

TEST(tcp_hold_closed_without_connection) {
    setup();
    port_set_drivers(&vm, nc_list);
    nst.upper = &nc_upper;
    Actor *o = new_actor();
    term f[3] = { term_from_int(1), term_from_int(80), term_from_int(0) };
    term p = open_port(o, "nc", TYPE_NET_DIAL, f, 3);
    CHECK(test_port_is(p));
    CHECK(port_any_live(&vm));
    drain();
    int32_t r = -1;
    CHECK_EQ(event_of(o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_NOTIME);
    CHECK(!port_any_live(&vm));
    port_close(&vm, p);
    teardown();
}

/* Exactly one terminal Closed per connection: a layer that posted its own
 * (a failure found while taking the FIN) is not followed by TCP's when the
 * connection then ends (here a RST). */
TEST(tcp_upper_one_terminal_closed) {
    setup();
    Conn k = up_dial(0);
    up.fin_post = GOGO_E_TLS;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
    k.peer++;
    seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);   /* then the peer resets */
    CHECK(k.c->state == TCP_CLOSED);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_TLS);
    CHECK_EQ(event_of(k.o, NULL), 0);          /* no second Closed */
    teardown();
}

/* After the layer's Closed{Normal} the connection's end still gets its
 * terminal Closed, also while the Normal is held (the owner full): the
 * terminal reason replaces the held Normal, it is never dropped. */
TEST(tcp_upper_terminal_after_normal) {
    setup();
    Conn k = up_dial(0);
    up.fin_post = GOGO_E_OK;
    seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
    k.peer++;
    seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_OK);
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_RESET);
    teardown();
    setup();
    k = up_dial(0);
    up.fin_post = GOGO_E_OK;
    k.o->max_words = (gogo_words_t)(k.o->mem_words + actor_charged_words(k.o));   /* no room: Normal held */
    seg_in(k.pport, k.vport, k.peer, k.una, 0x11, NULL);
    k.peer++;
    CHECK_EQ(event_of(k.o, NULL), 0);
    seg_in(k.pport, k.vport, k.peer, 0, 0x04, NULL);
    k.o->max_words = 4096;
    advance(100);
    r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_RESET);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* Keepalive ends an idle connection with an upper layer as retransmission
 * would: probes (an ACK one below snd_nxt, no data), then RST; the layer
 * is freed and the owner gets one Closed{Timeout} (none from TCP if the
 * layer posted its own). An answered probe restarts the idle time. */
TEST(tcp_upper_keepalive) {
    for (int own = 0; own < 2; own++) {
        setup();
        Conn k = up_dial(0);
        tcp_until_clear(&nst, k.c);         /* the layer's handshake is done */
        advance(GOGO_NET_KEEPALIVE_IDLE_MS - 1u);
        CHECK_EQ(tx.n, 0);
        advance(1);
        CHECK_EQ(tx.n, 1);
        Out s = out_at(0);
        CHECK_EQ(s.fl, 0x10);
        CHECK_EQ(s.seq, k.nxt - 1u);
        CHECK_EQ(s.ack, k.peer);
        CHECK_EQ(s.n, 0u);
        seg_in(k.pport, k.vport, k.peer, k.nxt, 0x10, NULL);   /* answered */
        tx.n = 0;
        advance(GOGO_NET_KEEPALIVE_IDLE_MS - 1u);
        CHECK_EQ(tx.n, 0);
        up.closed = own != 0;
        for (uint32_t i = 0; i < GOGO_NET_KEEPALIVE_PROBES; i++) {
            advance(i ? GOGO_NET_KEEPALIVE_INTVL_MS : 1u);
            CHECK_EQ(tx.n, (int)i + 1);
            CHECK_EQ(out_at((int)i).fl, 0x10);
        }
        CHECK(k.c->state == TCP_ESTABLISHED);
        CHECK_EQ(up.ended, 0);
        advance(GOGO_NET_KEEPALIVE_INTVL_MS);
        CHECK_EQ(tx.n, (int)GOGO_NET_KEEPALIVE_PROBES + 1);
        CHECK_EQ(out_at((int)GOGO_NET_KEEPALIVE_PROBES).fl, F_RA);
        CHECK(k.c->state == TCP_CLOSED);
        CHECK_EQ(up.ended, 1);
        int32_t r = -1;
        if (own) {
            CHECK_EQ(event_of(k.o, NULL), 0);
        } else {
            CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
            CHECK_EQ(r, GOGO_E_TIMEOUT);
        }
        CHECK_EQ(actor_charged_words(k.o), 0);
        teardown();
    }
}

/* Keepalive covers every state with an owner and nothing in flight:
 * ESTABLISHED, CLOSE_WAIT and FIN_WAIT_2 (the owner shut down and keeps
 * the port for the peer's data). A silent peer is probed, then the
 * connection is reset and the owner gets Closed{Timeout}. */
TEST(tcp_keepalive_states) {
    static const uint8_t states[] = { TCP_ESTABLISHED, TCP_CLOSE_WAIT, TCP_FIN_WAIT_2 };
    for (uint32_t i = 0; i < sizeof states; i++) {
        setup();
        Conn k = mk(states[i]);
        advance(GOGO_NET_KEEPALIVE_IDLE_MS);   /* the fake clock fires one timer per step */
        for (uint32_t n = 1; n < GOGO_NET_KEEPALIVE_PROBES; n++) advance(GOGO_NET_KEEPALIVE_INTVL_MS);
        CHECK_EQ(tx.n, (int)GOGO_NET_KEEPALIVE_PROBES);
        Out s = out_at(0);
        CHECK_EQ(s.fl, 0x10);
        CHECK_EQ(s.seq, k.nxt - 1u);
        CHECK(k.c->state == states[i]);
        advance(GOGO_NET_KEEPALIVE_INTVL_MS - 1u);
        CHECK(k.c->state == states[i]);
        advance(1);
        CHECK_EQ(out_at((int)GOGO_NET_KEEPALIVE_PROBES).fl, F_RA);
        CHECK(k.c->state == TCP_CLOSED);
        int32_t r = -1;
        CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
        CHECK_EQ(r, GOGO_E_TIMEOUT);
        teardown();
    }
}

/* ---- keepalive with a refused receive (a full mailbox) ---- */

/* Runs the fake clock to `until` in 100 ms steps (one timer per step).
 * The peer answers whatever the stack sends with an ACK (a probe's
 * answer) and resends `resend` every 5 s (if any). False at a RST. */
static bool ka_run(Conn *k, uint64_t until, const char *resend) {
    while (now_ms < until) {
        advance(100);
        bool sent = tx.n > 0;
        for (int i = 0; i < tx.n && i < MAX_TX; i++) {
            if (out_at(i).fl & 0x04) {
                tx.n = 0;
                return false;
            }
        }
        tx.n = 0;
        if (sent) seg_in(k->pport, k->vport, k->peer, k->nxt, 0x10, NULL);
        if (resend && now_ms % 5000u == 0) seg_in(k->pport, k->vport, k->peer, k->nxt, 0x18, resend);
        tx.n = 0;
    }
    return true;
}

/* No room left in a's limit (deliveries are refused). */
static gogo_words_t ka_fill(Actor *a) {
    gogo_words_t room = a->max_words;
    a->max_words = (gogo_words_t)(a->mem_words + actor_charged_words(a));
    return room;
}

/* A refusal starts its own idle period: a connection quiet for almost
 * the whole idle time whose next segment is refused (the owner's mailbox
 * full) survives 150 s of refusals with a live peer, and the resend is
 * taken once the owner has room. */
TEST(tcp_keepalive_stall_own_period) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    advance(GOGO_NET_KEEPALIVE_IDLE_MS - 100u);
    CHECK_EQ(tx.n, 0);
    gogo_words_t room = ka_fill(k.o);
    uint64_t t0 = now_ms;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "req");
    CHECK_EQ(tx.n, 0);                      /* refused: not ACKed */
    CHECK(ka_run(&k, t0 + 149900u, "req"));
    CHECK(k.c->state == TCP_ESTABLISHED);
    k.o->max_words = room;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "req");
    CHECK(tx.n >= 1);
    CHECK_EQ(out_at(0).ack, k.peer + 3u);
    CHECK_EQ(event_of(k.o, NULL), TYPE_CHUNK);
    teardown();
}

/* Room that comes and goes between the keepalive's checks still counts:
 * an owner full only at the instants the timer fires, with room in
 * between, keeps a live connection whose peer resends only after 200 s
 * (a backed-off retransmission), and the resend is taken. */
TEST(tcp_keepalive_stall_room_between) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    gogo_words_t room = ka_fill(k.o);
    uint64_t t0 = now_ms;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "req");
    CHECK_EQ(tx.n, 0);                      /* refused */
    while (now_ms < t0 + 200000u) {
        bool fire = (now_ms + 100u - t0) % GOGO_NET_KEEPALIVE_INTVL_MS == 0;
        if (fire) (void)ka_fill(k.o);
        else k.o->max_words = room;
        CHECK(ka_run(&k, now_ms + 100u, NULL));
    }
    CHECK(k.c->state == TCP_ESTABLISHED);
    k.o->max_words = room;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "req");
    CHECK(tx.n >= 1);
    CHECK_EQ(out_at(0).ack, k.peer + 3u);
    CHECK_EQ(event_of(k.o, NULL), TYPE_CHUNK);
    teardown();
}

/* A refusal that room does not cure (here an upper layer's full receive
 * buffer while the owner has room) still bounds the connection: one fresh
 * idle period when the owner is seen with room, none after, so a live
 * peer resending and answering probes is reset 150 s after that, with one
 * Closed{Timeout} and the layer ended. */
TEST(tcp_keepalive_stall_upper_full) {
    setup();
    Conn k = up_dial(0);
    tcp_until_clear(&nst, k.c);
    up.refuse_rx = true;
    uint64_t t0 = now_ms;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "rec");
    CHECK_EQ(tx.n, 0);                      /* refused: not ACKed */
    CHECK(!ka_run(&k, t0 + 400000u, "rec"));
    CHECK(now_ms > t0 + GOGO_NET_KEEPALIVE_IDLE_MS + GOGO_NET_KEEPALIVE_PROBES * GOGO_NET_KEEPALIVE_INTVL_MS);
    CHECK(now_ms <= t0 + GOGO_NET_KEEPALIVE_IDLE_MS + GOGO_NET_KEEPALIVE_PROBES * GOGO_NET_KEEPALIVE_INTVL_MS + 200u);
    CHECK(k.c->state == TCP_CLOSED);
    CHECK_EQ(up.ended, 1);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_TIMEOUT);
    teardown();
}

/* Orphans are never probed and keep their own limits: one in FIN_WAIT_2
 * is reset 4 s after its owner closed, one in LAST_ACK whose FIN is never
 * acknowledged 20 s after (GOGO_NET_ORPHAN_MS), with only FIN
 * retransmissions in between. */
TEST(tcp_keepalive_orphans) {
    setup();
    Conn k = mk(TCP_FIN_WAIT_2);
    port_close(&vm, k.port);
    CHECK(k.c->state == TCP_FIN_WAIT_2 && !k.c->port);   /* an orphan */
    advance(3999);
    CHECK_EQ(tx.n, 0);
    advance(1);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, F_RA);
    CHECK(k.c->state == TCP_CLOSED);
    teardown();
    setup();
    k = mk(TCP_LAST_ACK);
    port_close(&vm, k.port);
    CHECK(k.c->state == TCP_LAST_ACK && !k.c->port);
    uint64_t t0 = now_ms;
    while (now_ms < t0 + GOGO_NET_ORPHAN_MS - 100u) {
        advance(100);
        for (int i = 0; i < tx.n && i < MAX_TX; i++) CHECK(out_at(i).fl & 0x01);   /* FINs only */
        tx.n = 0;
    }
    CHECK(k.c->state == TCP_LAST_ACK);
    advance(100);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(out_at(0).fl, F_RA);
    CHECK(k.c->state == TCP_CLOSED);
    teardown();
}

/* ---- keepalive: live connections and edge cases ---- */

/* Checks sent frame i is a keepalive probe of k: an ACK, seq one below
 * snd_nxt, acknowledging all the peer sent, no data. */
static void ka_probe(const Conn *k, int i) {
    Out s = out_at(i);
    CHECK_EQ(s.fl, 0x10);
    CHECK_EQ(s.seq, k->nxt - 1u);
    CHECK_EQ(s.ack, k->peer);
    CHECK_EQ(s.n, 0u);
}

/* FIN_WAIT_2 with the peer still sending (data every 100 s for 600 s): its
 * data restarts the idle time, so no probe goes out and it stays open. */
TEST(tcp_keepalive_fin_wait_2_sending) {
    setup();
    Conn k = mk(TCP_FIN_WAIT_2);
    for (int i = 0; i < 6; i++) {
        advance(100000);
        CHECK_EQ(tx.n, 0);
        seg_in(k.pport, k.vport, k.peer, k.nxt, 0x18, "more");
        k.peer += 4u;
        CHECK_EQ(tx.n, 1);
        CHECK_EQ(out_at(0).ack, k.peer);
        CHECK_EQ(event_of(k.o, NULL), TYPE_CHUNK);
        tx.n = 0;
    }
    CHECK(k.c->state == TCP_FIN_WAIT_2);
    teardown();
}

/* CLOSE_WAIT with a live peer: probes acknowledge the peer's FIN, and a
 * peer answering them keeps the connection open for 600 s. */
TEST(tcp_keepalive_close_wait_live) {
    setup();
    Conn k = mk(TCP_CLOSE_WAIT);
    advance(GOGO_NET_KEEPALIVE_IDLE_MS);
    CHECK_EQ(tx.n, 1);
    ka_probe(&k, 0);
    tx.n = 0;
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x10, NULL);
    CHECK(ka_run(&k, now_ms + 600000u, NULL));
    CHECK(k.c->state == TCP_CLOSE_WAIT);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}

/* The answer to the last probe arriving in the same drain as the reset's
 * deadline is taken first: the connection lives and is idle again. */
TEST(tcp_keepalive_answer_at_deadline) {
    setup();
    Conn k = mk(TCP_ESTABLISHED);
    advance(GOGO_NET_KEEPALIVE_IDLE_MS);
    for (uint32_t n = 1; n < GOGO_NET_KEEPALIVE_PROBES; n++) advance(GOGO_NET_KEEPALIVE_INTVL_MS);
    CHECK_EQ(tx.n, (int)GOGO_NET_KEEPALIVE_PROBES);
    tx.n = 0;
    uint8_t b[128];
    net_rx_offer(&nst, b, tcp_frame(b, k.pport, k.vport, k.peer, k.nxt, 0x10, 8192, NULL, false));
    advance(GOGO_NET_KEEPALIVE_INTVL_MS);   /* the frame and the deadline, one drain */
    CHECK_EQ(tx.n, 0);
    CHECK(k.c->state == TCP_ESTABLISHED);
    advance(GOGO_NET_KEEPALIVE_IDLE_MS - 1u);
    CHECK_EQ(tx.n, 0);
    advance(1);
    CHECK_EQ(tx.n, 1);
    ka_probe(&k, 0);
    teardown();
}

/* The 32-bit clock wrapping inside the idle period, or inside the
 * probes, changes nothing: probes at 120, 130 and 140 s, the reset at
 * 150 s. */
TEST(tcp_keepalive_clock_wrap) {
    static const uint64_t before[] = { 60000u, 125000u };   /* ms before the wrap */
    for (int w = 0; w < 2; w++) {
        setup();
        now_ms = 0x100000000ull - before[w];
        Conn k = mk(TCP_ESTABLISHED);
        uint64_t t0 = now_ms;
        advance(GOGO_NET_KEEPALIVE_IDLE_MS - 1u);
        CHECK_EQ(tx.n, 0);
        for (uint32_t n = 0; n < GOGO_NET_KEEPALIVE_PROBES; n++) {
            if (n) {
                advance(GOGO_NET_KEEPALIVE_INTVL_MS - 1u);
                CHECK_EQ(tx.n, (int)n);
            }
            advance(1);
            CHECK_EQ(tx.n, (int)n + 1);
            ka_probe(&k, (int)n);
        }
        CHECK(k.c->state == TCP_ESTABLISHED);
        advance(GOGO_NET_KEEPALIVE_INTVL_MS - 1u);
        CHECK(k.c->state == TCP_ESTABLISHED);
        advance(1);
        CHECK_EQ(now_ms - t0, (uint64_t)GOGO_NET_KEEPALIVE_IDLE_MS + GOGO_NET_KEEPALIVE_PROBES * GOGO_NET_KEEPALIVE_INTVL_MS);
        CHECK(k.c->state == TCP_CLOSED);
        CHECK_EQ(out_at((int)GOGO_NET_KEEPALIVE_PROBES).fl, F_RA);
        int32_t r = -1;
        CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
        CHECK_EQ(r, GOGO_E_TIMEOUT);
        teardown();
    }
}

/* An accepted connection never resumed or handed off is probed too (its
 * probes offer window 0); a silent peer gets it reset, and the listener's
 * owner, who holds the port, gets Closed{Timeout} after the Accepted. */
TEST(tcp_keepalive_accepted_paused) {
    setup();
    Conn k = mk(TCP_SYN_RCVD);
    seg_in(k.pport, k.vport, k.peer, k.nxt, 0x10, NULL);
    CHECK(k.c->state == TCP_ESTABLISHED);
    CHECK_EQ(event_of(k.o, NULL), TYPE_NET_ACCEPTED);
    tx.n = 0;
    advance(GOGO_NET_KEEPALIVE_IDLE_MS);
    CHECK_EQ(tx.n, 1);
    ka_probe(&k, 0);
    CHECK_EQ(out_at(0).wnd, 0);
    for (uint32_t n = 0; n < GOGO_NET_KEEPALIVE_PROBES; n++) advance(GOGO_NET_KEEPALIVE_INTVL_MS);
    CHECK_EQ(out_at((int)GOGO_NET_KEEPALIVE_PROBES).fl, F_RA);
    CHECK(k.c->state == TCP_CLOSED);
    int32_t r = -1;
    CHECK_EQ(event_of(k.o, &r), TYPE_CLOSED);
    CHECK_EQ(r, GOGO_E_TIMEOUT);
    CHECK_EQ(event_of(k.o, NULL), 0);
    teardown();
}
