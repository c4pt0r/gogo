/* The net stack: ARP, IPv4, ICMP echo, UDP, the netif and udp ports,
 * Subscribe, and malformed frames of every layer:
 * dropped, counted, nothing sent or delivered. Frames go in through
 * net_rx_offer and the port layer's drain, out through the unix
 * platform's test TX hook. */
#include "test.h"
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
#include <stdlib.h>

/* ---- TX capture ---- */

#define MAX_TX 8
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

/* ---- addresses ---- */

static const uint8_t MY_MAC[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t PEER_MAC[6] = { 0x02, 0, 0, 0, 0, 0x99 };
static const uint8_t OTHER_MAC[6] = { 0x02, 0, 0, 0, 0, 0x55 };
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define MY_IP IP4(10, 0, 0, 2)
#define PEER_IP IP4(10, 0, 0, 1)        /* also the gateway */
#define MASK IP4(255, 255, 255, 0)

/* ---- VM fixture ---- */

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm;

static void mini_image(void) {
    uint32_t *w = img_w;
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
}

void platform_unix_arena_forget(void);

static void setup(void) {
    platform_unix_arena_forget();   /* setup may run many times in one test */
    va_reset(&vm);
    bytes_reset(&vm);
    mini_image();
    CHECK_EQ(vm_init(&vm, &img, 16), 0);
    memset(&tx, 0, sizeof tx);
    platform_unix_net_set_tx(tx_hook);
    net_attach(&vm);
}

static void teardown(void) {
    port_close_all(&vm);
    net_detach(&vm);
    platform_unix_net_set_tx(NULL);
}

static Actor *new_actor(void) {
    Actor *a = actor_new(&vm, 256);
    CHECK(a != NULL);
    a->state = A_WAITING;
    return a;
}

/* Terms on a's heap (no GC: the caller resets hp, the block has room). */
static term heap_str(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
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
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term heap_bytes(Actor *a, const uint8_t *p, uint32_t len) {
    if (!len) return T_EMPTYBYTES;
    uint32_t words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_BYTES);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, p, len);
    return term_from_ref(off);
}

static term open_port(Actor *a, const char *name, uint32_t type, const term *f, uint32_t n) {
    a->hp = 0;
    return port_open(&vm, a, heap_str(a, name), heap_struct(a, type, f, n));
}

static term open_netif(Actor *a) { return open_port(a, "netif", TYPE_NET_IFCONFIG, NULL, 0); }

static term open_udp(Actor *a, int32_t lport) {
    term f = term_from_int(lport);
    return open_port(a, "udp", TYPE_NET_UDP, &f, 1);
}

static void set_addr(Actor *from, term netif, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    from->hp = 0;
    term f[4] = { heap_int(from, (int32_t)ip), heap_int(from, (int32_t)mask),
                  heap_int(from, (int32_t)gw), heap_int(from, (int32_t)dns) };
    port_command(&vm, from, netif, heap_struct(from, TYPE_NET_SETADDR, f, 4));
}

static void send_to(Actor *from, term u, uint32_t ip, int32_t port, const uint8_t *data, uint32_t len) {
    from->hp = 0;
    term af[2] = { heap_int(from, (int32_t)ip), term_from_int(port) };
    term f[2] = { heap_struct(from, TYPE_NET_ADDR, af, 2), heap_bytes(from, data, len) };
    port_command(&vm, from, u, heap_struct(from, TYPE_NET_SENDTO, f, 2));
}

/* A configured stack (10.0.0.2/24, gateway 10.0.0.1) owned by `owner`,
 * with the gateway's MAC known; frames sent so far forgotten. */
static void rx(const uint8_t *f, uint32_t len);
static uint32_t arp_frame(uint8_t *b, uint16_t op, const uint8_t *sha, uint32_t spa,
                          const uint8_t *tha, uint32_t tpa, const uint8_t *dst);
static term configure(Actor *owner) {
    term n = open_netif(owner);
    CHECK(port_is(n));
    set_addr(owner, n, MY_IP, MASK, PEER_IP, IP4(10, 0, 0, 53));
    CHECK_EQ(tx.n, 1);                       /* the gateway pre-resolution */
    uint8_t b[64];
    uint32_t len = arp_frame(b, 2, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);
    rx(b, len);
    tx.n = 0;
    return n;
}

static void drain(void) {
    CHECK(port_work_pending(&vm));
    port_drain(&vm, 0);
    CHECK(!net_s.rx_ready);
}

static void rx(const uint8_t *f, uint32_t len) {
    net_rx_offer(f, len);
    drain();
}

static int mbox_len(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* ---- frame builders ---- */

static uint32_t eth_hdr(uint8_t *b, const uint8_t *dst, const uint8_t *src, uint16_t type) {
    memcpy(b, dst, 6);
    memcpy(b + 6, src, 6);
    wr16(b + 12, type);
    return 14;
}

static uint32_t arp_frame(uint8_t *b, uint16_t op, const uint8_t *sha, uint32_t spa,
                          const uint8_t *tha, uint32_t tpa, const uint8_t *dst) {
    eth_hdr(b, dst, sha, ETH_ARP);
    uint8_t *p = b + 14;
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, op);
    memcpy(p + 8, sha, 6); wr32(p + 14, spa);
    memcpy(p + 18, tha, 6); wr32(p + 24, tpa);
    return 14 + 28;
}

static uint16_t csum(const uint8_t *p, uint32_t n) { return (uint16_t)~net_fold(net_sum(0, p, n)); }

/* IPv4 header (20 bytes, or 24 with one option word) + payload at b+14. */
static uint32_t ip_pkt(uint8_t *b, uint32_t src, uint32_t dst, uint8_t proto,
                       const uint8_t *pl, uint32_t n, bool opt) {
    uint8_t *p = b + 14;
    uint32_t hl = opt ? 24u : 20u;
    memset(p, 0, hl);
    p[0] = (uint8_t)(0x40 | (hl / 4));
    wr16(p + 2, hl + n);
    p[8] = 64;
    p[9] = proto;
    wr32(p + 12, src);
    wr32(p + 16, dst);
    if (opt) p[20] = 1;                      /* NOP options */
    wr16(p + 10, csum(p, hl));
    memmove(p + hl, pl, n);
    return 14 + hl + n;
}

static uint32_t udp_seg(uint8_t *u, uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp,
                        const uint8_t *data, uint32_t n, bool cs) {
    wr16(u, sp); wr16(u + 2, dp); wr16(u + 4, 8 + n); wr16(u + 6, 0);
    memcpy(u + 8, data, n);
    if (cs) {
        uint8_t ph[12];
        wr32(ph, src); wr32(ph + 4, dst); wr16(ph + 8, 17); wr16(ph + 10, 8 + n);
        uint16_t c = (uint16_t)~net_fold(net_sum(net_sum(0, ph, 12), u, 8 + n));
        wr16(u + 6, c ? c : 0xFFFF);
    }
    return 8 + n;
}

static uint32_t udp_frame(uint8_t *b, const uint8_t *dmac, uint32_t src, uint32_t dst,
                          uint16_t sp, uint16_t dp, const uint8_t *data, uint32_t n) {
    uint8_t u[1600];
    uint32_t ul = udp_seg(u, src, dst, sp, dp, data, n, true);
    eth_hdr(b, dmac, PEER_MAC, ETH_IPV4);
    return ip_pkt(b, src, dst, 17, u, ul, false);
}

static uint32_t echo_frame(uint8_t *b, uint32_t src, uint32_t dst, uint16_t id, uint16_t seq,
                           const uint8_t *data, uint32_t n) {
    uint8_t m[1600];
    m[0] = 8; m[1] = 0; wr16(m + 2, 0); wr16(m + 4, id); wr16(m + 6, seq);
    memcpy(m + 8, data, n);
    wr16(m + 2, csum(m, 8 + n));
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    return ip_pkt(b, src, dst, 1, m, 8 + n, false);
}

/* Checks a sent IPv4 frame: header sane, checksum good; returns its payload. */
static const uint8_t *check_ip(int i, const uint8_t *dmac, uint32_t src, uint32_t dst, uint8_t proto, uint32_t *n) {
    const uint8_t *f = tx.f[i];
    CHECK(memcmp(f, dmac, 6) == 0);
    CHECK(memcmp(f + 6, MY_MAC, 6) == 0);
    CHECK_EQ(rd16(f + 12), ETH_IPV4);
    const uint8_t *p = f + 14;
    CHECK_EQ(p[0], 0x45);
    CHECK_EQ(rd16(p + 2), tx.len[i] - 14u);
    CHECK_EQ(net_fold(net_sum(0, p, 20)), 0xFFFF);
    CHECK_EQ(p[9], proto);
    CHECK_EQ(rd32(p + 12), src);
    CHECK_EQ(rd32(p + 16), dst);
    *n = rd16(p + 2) - 20u;
    return p + 20;
}

/* The Datagram at the head of a's mailbox: checks the layout, returns
 * the payload (NULL if empty) and its source. */
static const uint8_t *datagram(const Actor *a, uint32_t *ip, int32_t *port, uint32_t *len) {
    const Msg *m = a->mbox_head;
    CHECK(m != NULL);
    if (!m) return NULL;
    const uint32_t *d = m->data;
    CHECK_EQ(m->flags, 0);
    CHECK_EQ(d[0], hdr_make(4, K_STRUCT));
    CHECK_EQ(d[1], TYPE_NET_DATAGRAM);
    CHECK_EQ(d[2], term_from_ref(4));
    CHECK_EQ(d[4], hdr_make(4, K_STRUCT));
    CHECK_EQ(d[5], TYPE_NET_ADDR);
    if (term_is_int(d[6])) {
        *ip = (uint32_t)term_to_int(d[6]);
    } else {
        CHECK_EQ(d[term_ref_off(d[6])], hdr_make(2, K_INT));
        *ip = d[term_ref_off(d[6]) + 1];
    }
    *port = term_to_int(d[7]);
    if (d[3] == T_EMPTYBYTES) {
        *len = 0;
        return NULL;
    }
    const uint32_t *b = d + term_ref_off(d[3]);
    CHECK_EQ(hdr_kind(b[0]), K_BYTES);
    *len = b[1];
    CHECK_EQ(hdr_words(b[0]), 2u + (b[1] + 3u) / 4u);
    CHECK_EQ(term_ref_off(d[3]) + hdr_words(b[0]), m->words);
    return (const uint8_t *)(b + 2);
}

static void pop(Actor *a) {
    Msg *m = a->mbox_head;
    if (!m) return;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
    msg_free(&vm, m);
}

/* ---- tests ---- */

TEST(net_fixed_state_budget) {
    CHECK(net_fixed_bytes() <= NET_FIXED_LIMIT);
    CHECK(sizeof(UdpPcb) <= 32u);
    printf("  net fixed state %u bytes (UDP pcb %u)\n", (unsigned)net_fixed_bytes(), (unsigned)sizeof(UdpPcb));
}

/* The memory report: the stack's fixed state and the per-connection
 * control blocks (each charged to its owner, in whole 32-bit words) stay
 * within the budgets. */
TEST(net_mem_report) {
    printf("  mem: fixed %u/%u B, tcb %u/64 B (%u words), udp %u/32 B (%u words), listen %u B (%u words)\n",
           (unsigned)net_fixed_bytes(), (unsigned)NET_FIXED_LIMIT, (unsigned)sizeof(TcpPcb), (unsigned)TCP_PCB_WORDS,
           (unsigned)sizeof(UdpPcb), (unsigned)UDP_PCB_WORDS, (unsigned)sizeof(TcpListen), (unsigned)TCP_LISTEN_WORDS);
    CHECK(net_fixed_bytes() <= 4096u);
    CHECK(sizeof(TcpPcb) <= 64u);
    CHECK(sizeof(UdpPcb) <= 32u);
    CHECK(TCP_PCB_WORDS <= 16u && UDP_PCB_WORDS <= 8u);
    CHECK(GOGO_NET_MAX_TCP == 4u && GOGO_NET_MAX_LISTEN == 2u && GOGO_NET_MAX_UDP == 4u &&
          GOGO_NET_RX_BUFS == 1u && NET_FRAME_MAX == 1536u && GOGO_NET_ARP == 4u);
}

TEST(net_ports_refused_unattached) {
    va_reset(&vm);
    mini_image();
    CHECK_EQ(vm_init(&vm, &img, 16), 0);
    net_detach(&vm);
    Actor *a = new_actor();
    CHECK(open_netif(a) == T_NIL);
    CHECK(open_udp(a, 7) == T_NIL);
    CHECK(!port_subscribe(&vm, a));
}

TEST(net_netif_single_owner) {
    setup();
    Actor *a = new_actor(), *b = new_actor();
    term n = open_netif(a);
    CHECK(port_is(n));
    CHECK(open_netif(b) == T_NIL);           /* one owner at a time */
    set_addr(b, n, MY_IP, MASK, PEER_IP, 0); /* not the owner: ignored */
    CHECK(!ip_configured());
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK_EQ(net_s.ip, MY_IP);
    port_close(&vm, n);                      /* the address goes with it */
    CHECK(!ip_configured());
    CHECK(port_is(open_netif(b)));
    teardown();
}

TEST(net_arp_reply_and_preresolve) {
    setup();
    Actor *a = new_actor();
    uint8_t b[64];
    uint32_t len = arp_frame(b, 1, PEER_MAC, PEER_IP, (const uint8_t *)"\0\0\0\0\0\0", MY_IP, net_bcast_mac);
    rx(b, len);
    CHECK_EQ(tx.n, 0);                       /* unconfigured: no answer */
    term n = open_netif(a);
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK_EQ(tx.n, 1);                       /* ARP request for the gateway */
    const uint8_t *f = tx.f[0];
    CHECK(memcmp(f, net_bcast_mac, 6) == 0);
    CHECK_EQ(rd16(f + 12), ETH_ARP);
    CHECK_EQ(rd16(f + 20), 1);
    CHECK_EQ(rd32(f + 28), MY_IP);
    CHECK_EQ(rd32(f + 38), PEER_IP);
    tx.n = 0;
    rx(b, len);                              /* request for us: answered */
    CHECK_EQ(tx.n, 1);
    f = tx.f[0];
    CHECK_EQ(tx.len[0], 42u);
    CHECK(memcmp(f, PEER_MAC, 6) == 0);
    CHECK(memcmp(f + 6, MY_MAC, 6) == 0);
    CHECK_EQ(rd16(f + 20), 2);
    CHECK(memcmp(f + 22, MY_MAC, 6) == 0);
    CHECK_EQ(rd32(f + 28), MY_IP);
    CHECK(memcmp(f + 32, PEER_MAC, 6) == 0);
    CHECK_EQ(rd32(f + 38), PEER_IP);
    /* the requester was learned */
    CHECK(arp_resolve(PEER_IP) != NULL);
    tx.n = 0;
    len = arp_frame(b, 1, OTHER_MAC, IP4(10, 0, 0, 7), (const uint8_t *)"\0\0\0\0\0\0", IP4(10, 0, 0, 9), net_bcast_mac);
    uint32_t ign = net_s.st.arp_ignored;
    rx(b, len);                              /* for another host */
    CHECK_EQ(tx.n, 0);
    CHECK_EQ(net_s.st.arp_ignored, ign + 1);
    teardown();
}

TEST(net_arp_cache_lru) {
    setup();
    Actor *a = new_actor();
    configure(a);                            /* gateway learned first */
    uint8_t mac[6] = { 2, 0, 0, 0, 1, 0 };
    for (uint32_t i = 0; i < 2u * GOGO_NET_ARP; i++) {
        mac[5] = (uint8_t)i;
        arp_learn(IP4(10, 0, 0, 100 + i), mac, true);
    }
    /* Many newer local hosts: the gateway, the least recently used, is
     * still there (never evicted); the oldest of the others went. */
    CHECK_EQ(tx.n, 0);
    CHECK(arp_resolve(PEER_IP) != NULL);
    CHECK_EQ(tx.n, 0);
    CHECK(arp_resolve(IP4(10, 0, 0, 100 + 2u * GOGO_NET_ARP - 1u)) != NULL);
    CHECK(arp_resolve(IP4(10, 0, 0, 100)) == NULL);
    CHECK_EQ(tx.n, 1);                       /* a request went out instead */
    /* Inbound traffic from new local hosts does not push it out either. */
    uint8_t b[1600], d[4] = { 1, 2, 3, 4 };
    for (uint32_t i = 0; i < 2u * GOGO_NET_ARP; i++) {
        uint32_t len = udp_frame(b, MY_MAC, IP4(10, 0, 0, 200 + i), MY_IP, 9, 9, d, 4);
        b[11] = (uint8_t)(0x40 + i);         /* a source MAC per host */
        rx(b, len);
    }
    tx.n = 0;
    CHECK(arp_resolve(PEER_IP) != NULL);
    CHECK_EQ(tx.n, 0);
    teardown();
}

TEST(net_icmp_echo) {
    setup();
    Actor *a = new_actor();
    configure(a);
    uint8_t b[1600], data[100];
    for (int i = 0; i < 100; i++) data[i] = (uint8_t)(i * 7);
    uint32_t len = echo_frame(b, IP4(8, 8, 8, 8), MY_IP, 0x1234, 7, data, 100);   /* off-subnet */
    rx(b, len);
    CHECK_EQ(tx.n, 1);
    uint32_t n;
    const uint8_t *m = check_ip(0, PEER_MAC, MY_IP, IP4(8, 8, 8, 8), 1, &n);
    CHECK_EQ(n, 108u);
    CHECK_EQ(m[0], 0);
    CHECK_EQ(net_fold(net_sum(0, m, n)), 0xFFFF);
    CHECK_EQ(rd16(m + 4), 0x1234);
    CHECK_EQ(rd16(m + 6), 7);
    CHECK(memcmp(m + 8, data, 100) == 0);
    CHECK_EQ(net_s.st.icmp_echo, 1u);
    /* not an echo request: ignored; to a broadcast address: ignored */
    tx.n = 0;
    b[14 + 20] = 13;
    wr16(b + 14 + 22, 0);
    wr16(b + 14 + 22, csum(b + 34, 108));
    rx(b, len);
    len = echo_frame(b, PEER_IP, IP4(10, 0, 0, 255), 1, 1, data, 4);
    rx(b, len);
    CHECK_EQ(tx.n, 0);
    teardown();
}

TEST(net_udp_deliver) {
    setup();
    Actor *a = new_actor(), *o = new_actor();
    configure(a);
    term u = open_udp(o, 7);
    CHECK(port_is(u));
    CHECK(open_udp(a, 7) == T_NIL);          /* port taken */
    uint8_t b[1600], data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)i;
    /* from an address that needs a boxed int (100.64.0.1) */
    uint32_t len = udp_frame(b, MY_MAC, IP4(100, 64, 0, 1), MY_IP, 5555, 7, data, 300);
    rx(b, len);
    CHECK_EQ(mbox_len(o), 1);
    uint32_t ip, n;
    int32_t port;
    const uint8_t *p = datagram(o, &ip, &port, &n);
    CHECK_EQ(ip, IP4(100, 64, 0, 1));
    CHECK_EQ(port, 5555);
    CHECK_EQ(n, 300u);
    CHECK(p && memcmp(p, data, 300) == 0);
    /* receive it into the heap: a K_BYTES of 300 bytes */
    term t;
    CHECK(msg_receive(&vm, o, &t));
    const uint32_t *s = obj_of(&vm, o, t);
    CHECK(s && s[1] == TYPE_NET_DATAGRAM);
    const uint8_t *bp;
    uint32_t bl;
    CHECK(bytes_view(&vm, o, s[3], &bp, &bl));
    CHECK_EQ(bl, 300u);
    CHECK(memcmp(bp, data, 300) == 0);
    /* empty payload, no checksum, broadcast destination */
    len = udp_frame(b, net_bcast_mac, PEER_IP, IP4(255, 255, 255, 255), 9, 7, data, 0);
    wr16(b + 34 + 6, 0);
    rx(b, len);
    CHECK(datagram(o, &ip, &port, &n) == NULL);
    CHECK_EQ(n, 0u);
    pop(o);
    /* unbound port: counted */
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 8, data, 4);
    rx(b, len);
    CHECK_EQ(net_s.st.udp_noport, 1u);
    CHECK_EQ(mbox_len(o), 0);
    /* IP options are skipped */
    uint8_t useg[64];
    uint32_t ul = udp_seg(useg, PEER_IP, MY_IP, 9, 7, data, 5, true);
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    len = ip_pkt(b, PEER_IP, MY_IP, 17, useg, ul, true);
    rx(b, len);
    CHECK_EQ(mbox_len(o), 1);
    /* the owner is charged the control block; close frees the pcb */
    CHECK_EQ(actor_charged_words(o) >= UDP_PCB_WORDS, 1);
    port_close(&vm, u);
    CHECK(port_is(open_udp(a, 7)));
    teardown();
}

TEST(net_udp_send) {
    setup();
    Actor *a = new_actor();
    configure(a);
    term u = open_udp(a, 0);                 /* ephemeral */
    CHECK(port_is(u));
    uint8_t data[200];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)(255 - i);
    send_to(a, u, PEER_IP, 9000, data, 200);
    CHECK_EQ(tx.n, 1);
    uint32_t n;
    const uint8_t *p = check_ip(0, PEER_MAC, MY_IP, PEER_IP, 17, &n);
    CHECK_EQ(n, 208u);
    CHECK_EQ(rd16(p), 49152);
    CHECK_EQ(rd16(p + 2), 9000);
    CHECK_EQ(rd16(p + 4), 208);
    uint8_t ph[12];
    wr32(ph, MY_IP); wr32(ph + 4, PEER_IP); wr16(ph + 8, 17); wr16(ph + 10, 208);
    CHECK_EQ(net_fold(net_sum(net_sum(0, ph, 12), p, 208)), 0xFFFF);
    CHECK(memcmp(p + 8, data, 200) == 0);
    /* off-subnet: through the gateway */
    send_to(a, u, IP4(100, 64, 0, 1), 53, data, 1);
    CHECK_EQ(tx.n, 2);
    check_ip(1, PEER_MAC, MY_IP, IP4(100, 64, 0, 1), 17, &n);
    /* subnet broadcast */
    send_to(a, u, IP4(10, 0, 0, 255), 53, data, 0);
    CHECK_EQ(tx.n, 3);
    check_ip(2, net_bcast_mac, MY_IP, IP4(10, 0, 0, 255), 17, &n);
    CHECK_EQ(n, 8u);
    /* unresolved on-link host: dropped, ARP request sent */
    send_to(a, u, IP4(10, 0, 0, 5), 53, data, 10);
    CHECK_EQ(tx.n, 4);
    CHECK_EQ(rd16(tx.f[3] + 12), ETH_ARP);
    CHECK_EQ(rd32(tx.f[3] + 38), IP4(10, 0, 0, 5));
    CHECK_EQ(net_s.st.tx_arp_miss, 1u);
    uint8_t b[64];
    rx(b, arp_frame(b, 2, OTHER_MAC, IP4(10, 0, 0, 5), MY_MAC, MY_IP, MY_MAC));
    send_to(a, u, IP4(10, 0, 0, 5), 53, data, 10);
    CHECK_EQ(tx.n, 5);
    check_ip(4, OTHER_MAC, MY_IP, IP4(10, 0, 0, 5), 17, &n);
    /* malformed commands: bad port, too large, wrong struct; to ourselves */
    uint32_t bad = net_s.st.tx_bad;
    send_to(a, u, PEER_IP, 0, data, 1);
    send_to(a, u, PEER_IP, 70000, data, 1);
    static uint8_t big[1500];
    send_to(a, u, PEER_IP, 9, big, UDP_MAX_PAYLOAD + 1);
    a->hp = 0;
    term one = term_from_int(1);
    port_command(&vm, a, u, heap_struct(a, TYPE_NET_UDP, &one, 1));
    CHECK_EQ(net_s.st.tx_bad, bad + 4);
    send_to(a, u, 0, 9, data, 1);                      /* 0.0.0.0 */
    send_to(a, u, IP4(224, 0, 0, 251), 5353, data, 1); /* multicast */
    send_to(a, u, IP4(239, 1, 2, 3), 9, data, 1);
    CHECK_EQ(net_s.st.tx_bad, bad + 7);
    send_to(a, u, MY_IP, 9, data, 1);
    CHECK_EQ(tx.n, 5);
    send_to(a, u, PEER_IP, 9, big, UDP_MAX_PAYLOAD);   /* the largest fits */
    CHECK_EQ(tx.n, 6);
    CHECK_EQ(tx.len[5], 14u + 1500u);
    teardown();
}

TEST(net_udp_quota_never_kills) {
    setup();
    Actor *a = new_actor(), *o = new_actor();
    configure(a);
    term u = open_udp(o, 7);
    o->max_words = (uint16_t)(o->mem_words + actor_charged_words(o) + 400u);
    uint8_t b[1600], data[400];
    memset(data, 0x5A, sizeof data);
    uint32_t len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, data, 400);
    for (int i = 0; i < 10; i++) rx(b, len);
    int got = mbox_len(o);
    CHECK(got >= 1 && got < 10);
    CHECK_EQ(net_s.st.udp_quota, (uint32_t)(10 - got));
    uint32_t drops = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_UDP; i++) drops += net_s.udp[i].drops;
    CHECK_EQ(drops, (uint32_t)(10 - got));
    CHECK(!(o->flags & AF_EXIT_PENDING));    /* never killed */
    while (o->mbox_head) pop(o);
    rx(b, len);                              /* room again: delivered */
    CHECK_EQ(mbox_len(o), 1);
    (void)u;
    teardown();
}

static uint32_t msg_type(const Actor *a, int nth);

/* An owner that is also a subscriber, busy
 * while datagrams fill its mailbox, is not killed by the stack's next
 * Up: delivery leaves room for one Up, so it arrives at once. For every
 * quota the flood of large then empty datagrams stops short of the limit. */
TEST(net_udp_fill_leaves_up_room) {
    static uint8_t data[1400];
    memset(data, 0x41, sizeof data);
    for (uint32_t slack = 40; slack < 1200; slack += 37) {
        setup();
        Actor *a = new_actor(), *o = new_actor();
        term n = configure(a);
        term u = open_udp(o, 5000);
        CHECK(port_is(u));
        CHECK(port_subscribe(&vm, o));
        while (o->mbox_head) pop(o);         /* the initial Up */
        o->max_words = (uint16_t)(o->mem_words + actor_charged_words(o) + slack);
        uint8_t b[1600];
        uint32_t len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9999, 5000, data, sizeof data);
        for (int i = 0; i < 10; i++) rx(b, len);
        len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9999, 5000, data, 0);
        for (int i = 0; i < 40; i++) rx(b, len);
        CHECK(actor_max_words(o) - (o->mem_words + actor_charged_words(o)) >= msg_footprint_words(11));
        int got = mbox_len(o);
        set_addr(a, n, MY_IP, MASK, PEER_IP, IP4(8, 8, 8, 8));   /* a new DNS server: Up */
        CHECK(!(o->flags & AF_EXIT_PENDING));
        CHECK_EQ(mbox_len(o), got + 1);
        CHECK_EQ(msg_type(o, got), TYPE_NET_UP);
        teardown();
    }
}

/* NET-5: delivery to an owner blocked in Receive first collects the slack
 * of its grown block (as TCP does), instead of dropping a datagram that
 * fits once the slack is gone. */
TEST(net_udp_reclaims_waiting_slack) {
    setup();
    Actor *a = new_actor(), *o = new_actor();
    configure(a);
    term u = open_udp(o, 7);
    CHECK(port_is(u));
    o->hp = 0;                               /* nothing live: the 256-word block is slack */
    o->max_words = (uint16_t)(o->mem_words + actor_charged_words(o) + 20u);
    uint8_t b[1600], data[400];
    memset(data, 0x5A, sizeof data);
    uint32_t len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, data, sizeof data);
    rx(b, len);
    CHECK_EQ(mbox_len(o), 1);
    CHECK_EQ(net_s.st.udp_quota, 0);
    CHECK(o->mem_words < 256);
    teardown();
}

TEST(net_unconfigured_dhcp_only) {
    setup();
    Actor *a = new_actor();
    term n = open_netif(a);
    term c = open_udp(a, 68), s = open_udp(a, 7);
    CHECK(port_is(n) && port_is(c) && port_is(s));
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    send_to(a, c, IP_BCAST, 67, data, 8);    /* DHCP discover: allowed */
    CHECK_EQ(tx.n, 1);
    uint32_t len;
    check_ip(0, net_bcast_mac, 0, IP_BCAST, 17, &len);
    send_to(a, c, PEER_IP, 67, data, 8);     /* unicast: no route yet */
    send_to(a, c, IP_BCAST, 68, data, 8);    /* wrong server port */
    send_to(a, s, IP_BCAST, 67, data, 8);    /* wrong client port */
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(net_s.st.tx_noroute, 3u);
    uint8_t b[1600];
    /* an offer to the offered address (unicast to our MAC): delivered */
    uint8_t reply[240] = { 2 };
    memcpy(reply + 28, MY_MAC, 6);
    len = udp_frame(b, MY_MAC, PEER_IP, IP4(10, 0, 0, 2), 67, 68, reply, sizeof reply);
    rx(b, len);
    CHECK_EQ(mbox_len(a), 1);
    len = udp_frame(b, net_bcast_mac, PEER_IP, IP_BCAST, 67, 68, reply, sizeof reply);
    rx(b, len);
    CHECK_EQ(mbox_len(a), 2);
    /* anything else is filtered */
    len = udp_frame(b, MY_MAC, PEER_IP, IP4(10, 0, 0, 2), 9, 7, data, 8);
    rx(b, len);
    len = udp_frame(b, MY_MAC, PEER_IP, IP4(10, 0, 0, 2), 66, 68, data, 8);
    rx(b, len);
    len = echo_frame(b, PEER_IP, IP4(10, 0, 0, 2), 1, 1, data, 8);
    rx(b, len);
    CHECK_EQ(mbox_len(a), 2);
    CHECK_EQ(tx.n, 1);
    CHECK_EQ(net_s.st.ip_filtered, 3u);
    teardown();
}

/* BOOTP requests from port 68 to 67 carry the interface MAC in chaddr
 * (programs cannot know it); replies to port 68 for another chaddr, or
 * too short to have one, are not delivered. */
TEST(net_dhcp_chaddr) {
    setup();
    Actor *a = new_actor();
    term n = open_netif(a);
    term c = open_udp(a, 68);
    CHECK(port_is(n) && port_is(c));
    uint8_t req[244] = { 1, 1, 6 };
    memset(req + 28, 0xEE, 16);
    send_to(a, c, IP_BCAST, 67, req, sizeof req);
    req[0] = 2;                              /* not a request: untouched */
    send_to(a, c, IP_BCAST, 67, req, sizeof req);
    send_to(a, c, IP_BCAST, 67, req, 33);    /* too short: untouched */
    CHECK_EQ(tx.n, 3);
    uint32_t len;
    const uint8_t *u = check_ip(0, net_bcast_mac, 0, IP_BCAST, 17, &len);
    CHECK_EQ(len, 8u + sizeof req);
    CHECK(memcmp(u + 8 + 28, MY_MAC, 6) == 0);
    CHECK_EQ(u[8 + 34], 0xEE);               /* the rest of chaddr kept */
    uint8_t ph[12];                          /* checksum over the stamped bytes */
    wr32(ph, 0); wr32(ph + 4, IP_BCAST); wr16(ph + 8, 17); wr16(ph + 10, len);
    CHECK_EQ(net_fold(net_sum(net_sum(0, ph, 12), u, len)), 0xFFFF);
    u = check_ip(1, net_bcast_mac, 0, IP_BCAST, 17, &len);
    CHECK_EQ(u[8 + 28], 0xEE);
    u = check_ip(2, net_bcast_mac, 0, IP_BCAST, 17, &len);
    CHECK_EQ(u[8 + 28], 0xEE);
    uint8_t reply[240] = { 2 }, b[1600];
    memcpy(reply + 28, PEER_MAC, 6);         /* another client's */
    len = udp_frame(b, net_bcast_mac, PEER_IP, IP_BCAST, 67, 68, reply, sizeof reply);
    rx(b, len);
    memcpy(reply + 28, MY_MAC, 6);
    len = udp_frame(b, net_bcast_mac, PEER_IP, IP_BCAST, 67, 68, reply, 33);
    rx(b, len);
    CHECK_EQ(mbox_len(a), 0);
    CHECK_EQ(net_s.st.ip_filtered, 2u);
    len = udp_frame(b, net_bcast_mac, PEER_IP, IP_BCAST, 67, 68, reply, 34);
    rx(b, len);
    CHECK_EQ(mbox_len(a), 1);
    teardown();
}

/* Type of the n-th message queued on a (an Up / Down struct). */
static uint32_t msg_type(const Actor *a, int nth) {
    const Msg *m = a->mbox_head;
    while (m && nth--) m = m->next;
    CHECK(m != NULL);
    return m ? m->data[1] : 0xFFFFFFFFu;
}

static int32_t up_field(const Actor *a, int nth, int i) {
    const Msg *m = a->mbox_head;
    while (m && nth--) m = m->next;
    if (!m) return 0;
    term t = m->data[2 + i];
    return term_is_int(t) ? term_to_int(t) : (int32_t)m->data[term_ref_off(t) + 1];
}

TEST(net_subscribe_up_down) {
    setup();
    Actor *a = new_actor(), *s = new_actor(), *s2 = new_actor();
    CHECK(port_subscribe(&vm, s));
    CHECK(port_subscribe(&vm, s));           /* twice = once */
    term n = open_netif(a);
    set_addr(a, n, IP4(100, 64, 0, 2), MASK, IP4(100, 64, 0, 1), IP4(10, 0, 0, 53));
    CHECK_EQ(mbox_len(s), 1);
    CHECK_EQ(msg_type(s, 0), TYPE_NET_UP);
    CHECK_EQ((uint32_t)up_field(s, 0, 0), IP4(100, 64, 0, 2));
    CHECK_EQ((uint32_t)up_field(s, 0, 1), IP4(100, 64, 0, 1));
    CHECK_EQ((uint32_t)up_field(s, 0, 2), IP4(10, 0, 0, 53));
    set_addr(a, n, IP4(100, 64, 0, 2), MASK, IP4(100, 64, 0, 1), IP4(10, 0, 0, 53));
    CHECK_EQ(mbox_len(s), 1);                /* no change: no report */
    CHECK(port_subscribe(&vm, s2));          /* up already: Up at once */
    CHECK_EQ(mbox_len(s2), 1);
    CHECK_EQ(msg_type(s2, 0), TYPE_NET_UP);
    set_addr(a, n, IP4(100, 64, 0, 3), MASK, IP4(100, 64, 0, 1), 0);   /* changed */
    CHECK_EQ(mbox_len(s), 2);
    CHECK_EQ((uint32_t)up_field(s, 1, 0), IP4(100, 64, 0, 3));
    set_addr(a, n, 0, 0, 0, 0);              /* cleared: Down */
    CHECK_EQ(mbox_len(s), 3);
    CHECK_EQ(msg_type(s, 2), TYPE_NET_DOWN);
    CHECK_EQ(s->mbox_head->next->next->words, 2);
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK_EQ(mbox_len(s), 4);
    net_link_event(false);                   /* link lost: Down */
    drain();
    CHECK_EQ(mbox_len(s), 5);
    CHECK_EQ(msg_type(s, 4), TYPE_NET_DOWN);
    net_link_event(true);                    /* back: Up, gateway re-resolved */
    tx.n = 0;
    drain();
    CHECK_EQ(mbox_len(s), 6);
    CHECK_EQ(msg_type(s, 5), TYPE_NET_UP);
    CHECK_EQ(tx.n, 1);
    net_link_event(false);                   /* Down and Up before one drain: */
    net_link_event(true);                    /* the Down is not lost */
    drain();
    CHECK_EQ(mbox_len(s), 8);
    CHECK_EQ(msg_type(s, 6), TYPE_NET_DOWN);
    CHECK_EQ(msg_type(s, 7), TYPE_NET_UP);
    /* The producer's steps taken one at a time (state, then the counter):
     * a drain between them reports the Down once, not twice, and no Up. */
    net_s.link_state = 0;
    net_s.link_pending = 1;                  /* mid net_link_event(false) */
    drain();
    CHECK_EQ(mbox_len(s), 9);
    CHECK_EQ(msg_type(s, 8), TYPE_NET_DOWN);
    net_s.down_seq++;
    net_s.link_pending = 1;                  /* the event completes */
    drain();
    CHECK_EQ(mbox_len(s), 9);
    net_s.link_state = 1;
    net_s.link_pending = 1;                  /* net_link_event(true) */
    drain();
    CHECK_EQ(mbox_len(s), 10);
    CHECK_EQ(msg_type(s, 9), TYPE_NET_UP);
    /* A whole Down completed between the drain's first read of the state
     * (an old Up) and its read of the counter: the re-read gives the Down,
     * so exactly one Down and no Up (the stale 1 cannot be injected, the
     * end state it would race with is checked). */
    net_s.link_state = 0;
    net_s.down_seq++;
    net_s.link_pending = 1;
    drain();
    CHECK_EQ(mbox_len(s), 11);
    CHECK_EQ(msg_type(s, 10), TYPE_NET_DOWN);
    net_link_event(true);
    drain();
    CHECK_EQ(mbox_len(s), 12);
    CHECK_EQ(msg_type(s, 11), TYPE_NET_UP);
    port_close(&vm, n);                      /* lease gone with the port */
    drain();
    CHECK_EQ(mbox_len(s), 13);
    CHECK_EQ(msg_type(s, 12), TYPE_NET_DOWN);
    /* table full: dead subscribers make room */
    Actor *x[GOGO_NET_MAX_SUBS];
    uint32_t ok = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        x[i] = new_actor();
        ok += port_subscribe(&vm, x[i]);
    }
    CHECK_EQ(ok, GOGO_NET_MAX_SUBS - 2u);
    x[0]->flags |= AF_EXIT_PENDING;
    CHECK(port_subscribe(&vm, x[GOGO_NET_MAX_SUBS - 1]));
    teardown();
}

/* An Up or Down that cannot be allocated is owed to the subscriber and
 * retried from the source's poll (every 50 ms) until it goes out. */
static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
extern void platform_unix_set_clock(uint64_t (*fn)(void));

TEST(net_subscribe_retry_on_oom) {
    setup();
    fake_now = 1000;
    platform_unix_set_clock(fake_clock);
    Actor *a = new_actor(), *s = new_actor();
    CHECK(port_subscribe(&vm, s));
    term n = open_netif(a);
    va_set_limit(&vm, va_used_bytes(&vm));           /* no message can be allocated */
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK_EQ(mbox_len(s), 0);
    CHECK(net_s.sub_owed != 0);
    CHECK(!port_work_pending(&vm));          /* not before the retry time */
    fake_now += 50;
    drain();                                 /* still no memory: owed again */
    CHECK_EQ(mbox_len(s), 0);
    va_set_limit(&vm, 0);
    fake_now += 50;
    drain();
    CHECK_EQ(mbox_len(s), 1);
    CHECK_EQ(msg_type(s, 0), TYPE_NET_UP);
    CHECK_EQ(net_s.sub_owed, 0);
    va_set_limit(&vm, va_used_bytes(&vm));
    set_addr(a, n, 0, 0, 0, 0);              /* the Down, likewise */
    CHECK_EQ(mbox_len(s), 1);
    va_set_limit(&vm, 0);
    fake_now += 50;
    drain();
    CHECK_EQ(mbox_len(s), 2);
    CHECK_EQ(msg_type(s, 1), TYPE_NET_DOWN);
    platform_unix_set_clock(NULL);
    teardown();
}

/* NET-1: an Up to a subscriber whose mailbox is full never kills it:
 * it is owed and retried until the subscriber has room. This
 * covers subscribers that own TCP connections as well as UDP ports. */
TEST(net_up_to_full_mailbox_is_owed) {
    setup();
    fake_now = 1000;
    platform_unix_set_clock(fake_clock);
    Actor *a = new_actor(), *s = new_actor();
    CHECK(port_subscribe(&vm, s));
    term n = open_netif(a);
    /* s's whole block is live data (no slack a GC could collect), and its
     * limit is what it uses: no room. */
    CHECK(heap_push_frame(&vm, s, 1, -1));
    uint32_t live = (uint32_t)(s->sp - s->hp), o = heap_alloc(&vm, s, live, NULL, 0);
    CHECK(o != UINT32_MAX);
    memset(s->mem + o, 0, live * 4u);
    s->mem[o] = hdr_make(live, K_STRING);
    s->mem[o + 1] = (live - 2u) * 4u;
    s->mem[s->fp] = term_from_ref(o);
    s->max_words = (uint16_t)(s->mem_words + actor_charged_words(s));
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK(!(s->flags & AF_EXIT_PENDING));
    CHECK_EQ(mbox_len(s), 0);
    CHECK(net_s.sub_owed != 0);
    fake_now += 50;
    drain();                                 /* still full: owed again */
    CHECK(!(s->flags & AF_EXIT_PENDING));
    CHECK_EQ(mbox_len(s), 0);
    s->max_words = 0;                        /* the subscriber drained */
    fake_now += 50;
    drain();
    CHECK_EQ(mbox_len(s), 1);
    CHECK_EQ(msg_type(s, 0), TYPE_NET_UP);
    CHECK_EQ(net_s.sub_owed, 0);
    platform_unix_set_clock(NULL);
    teardown();
}

/* A subscriber blocked in Receive whose grown block is all
 * slack gets its Up at once: the slack is collected first, as for UDP and
 * TCP delivery, instead of the Up being owed forever. */
TEST(net_up_reclaims_waiting_subscriber_slack) {
    setup();
    Actor *a = new_actor(), *s = new_actor();   /* s: A_WAITING, 256 words, nothing live */
    CHECK(port_subscribe(&vm, s));
    term n = open_netif(a);
    s->max_words = (uint16_t)(s->mem_words + actor_charged_words(s));
    set_addr(a, n, MY_IP, MASK, PEER_IP, 0);
    CHECK(!(s->flags & AF_EXIT_PENDING));
    CHECK_EQ(mbox_len(s), 1);
    CHECK_EQ(msg_type(s, 0), TYPE_NET_UP);
    CHECK_EQ(net_s.sub_owed, 0);
    CHECK(s->mem_words < 256);
    teardown();
}

TEST(net_rx_slot) {
    setup();
    uint8_t b[NET_FRAME_MAX + 1];
    memset(b, 0, sizeof b);
    net_rx_offer(b, NET_FRAME_MAX + 1);
    CHECK_EQ(net_s.st.rx_oversize, 1u);
    CHECK(!net_s.rx_ready);
    net_rx_offer(b, 60);
    CHECK(net_s.rx_ready);
    net_rx_offer(b, 60);                     /* slot busy: dropped */
    CHECK_EQ(net_s.st.rx_busy, 1u);
    drain();
    CHECK_EQ(net_s.st.rx_frames, 1u);
    teardown();
}

/* One malformed frame per case: dropped, its counter bumped, nothing sent
 * or delivered. */
TEST(net_malformed_frames) {
    setup();
    Actor *a = new_actor(), *o = new_actor();
    configure(a);
    open_udp(o, 7);
    open_udp(o, 68);
    uint8_t b[1600], d[32];
    memset(d, 0xAB, sizeof d);
    uint32_t len;
    const NetStats *st = net_stats();
#define DROP(counter, frame_len) do {                                       \
        uint32_t before_ = st->counter;                                     \
        tx.n = 0;                                                           \
        rx(b, (frame_len));                                                 \
        CHECK_EQ(st->counter, before_ + 1u);                                \
        CHECK_EQ(tx.n, 0);                                                  \
        CHECK_EQ(mbox_len(o), 0);                                           \
        if (st->counter != before_ + 1u || tx.n || mbox_len(o))             \
            printf("  in case %s at line %d\n", #counter, __LINE__);         \
    } while (0)
    /* Ethernet */
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    DROP(eth_bad, 0);
    DROP(eth_bad, 13);
    eth_hdr(b, MY_MAC, PEER_MAC, 0x86DD);    /* IPv6 */
    DROP(eth_unknown, 60);
    eth_hdr(b, MY_MAC, PEER_MAC, 0x8100);    /* VLAN */
    DROP(eth_unknown, 60);
    len = udp_frame(b, OTHER_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    DROP(eth_filtered, len);                 /* another host's */
    b[0] = 0x01; b[1] = 0x00; b[2] = 0x5E;   /* multicast */
    DROP(eth_filtered, len);
    /* ARP */
    len = arp_frame(b, 1, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);
    DROP(arp_bad, len - 1);                  /* short */
    wr16(b + 14, 6);                         /* hardware type */
    DROP(arp_bad, len);
    len = arp_frame(b, 1, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);
    b[18] = 8;                               /* hlen */
    DROP(arp_bad, len);
    len = arp_frame(b, 3, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);   /* op */
    DROP(arp_bad, len);
    len = arp_frame(b, 1, net_bcast_mac, PEER_IP, MY_MAC, MY_IP, MY_MAC);   /* broadcast sender */
    memcpy(b + 6, PEER_MAC, 6);              /* (a valid Ethernet source) */
    DROP(arp_bad, len);
    len = arp_frame(b, 1, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);
    b[6] = 0x01;                             /* group-bit Ethernet source */
    DROP(eth_bad, len);
    memcpy(b + 6, net_bcast_mac, 6);
    DROP(eth_bad, len);
    /* IPv4 */
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    DROP(ip_bad, 14 + 19);                   /* short header */
    b[14] = 0x65;                            /* version 6 */
    DROP(ip_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    b[14] = 0x44;                            /* IHL 4 */
    DROP(ip_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    b[14] = 0x4F;                            /* IHL 15 > total length */
    DROP(ip_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    DROP(ip_bad, len - 1);                   /* total length > frame */
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    b[24] ^= 1;                              /* header checksum */
    DROP(ip_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    b[20] |= 0x20;                           /* MF */
    wr16(b + 24, 0); wr16(b + 24, csum(b + 14, 20));
    DROP(ip_frag, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    wr16(b + 20, 3);                         /* fragment offset */
    wr16(b + 24, 0); wr16(b + 24, csum(b + 14, 20));
    DROP(ip_frag, len);
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    len = ip_pkt(b, PEER_IP, MY_IP, 2, d, 8, false);   /* IGMP */
    DROP(ip_proto, len);
    len = udp_frame(b, MY_MAC, PEER_IP, IP4(10, 0, 0, 3), 9, 7, d, 4);   /* another IP */
    DROP(ip_filtered, len);
    len = udp_frame(b, MY_MAC, PEER_IP, IP4(224, 0, 0, 1), 9, 7, d, 4);  /* multicast IP */
    DROP(ip_filtered, len);
    len = udp_frame(b, MY_MAC, IP_BCAST, MY_IP, 9, 7, d, 4);             /* broadcast source */
    DROP(ip_filtered, len);
    len = udp_frame(b, MY_MAC, IP4(10, 0, 0, 255), MY_IP, 9, 7, d, 4);   /* subnet broadcast source */
    DROP(ip_filtered, len);
    len = udp_frame(b, MY_MAC, IP4(224, 0, 0, 9), MY_IP, 9, 7, d, 4);    /* multicast source */
    DROP(ip_filtered, len);
    len = udp_frame(b, MY_MAC, MY_IP, MY_IP, 9, 7, d, 4);                /* our own address */
    DROP(ip_filtered, len);
    {   /* total length 1501 (a frame that long, from a link with a bigger MTU) */
        static uint8_t jumbo[1600];
        memset(jumbo, 0, sizeof jumbo);
        len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, jumbo, 1501u - 28u);
    }
    DROP(ip_bad, len);
    /* ICMP */
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    len = ip_pkt(b, PEER_IP, MY_IP, 1, d, 7, false);   /* short */
    DROP(icmp_bad, len);
    len = echo_frame(b, PEER_IP, MY_IP, 1, 1, d, 8);
    b[len - 1] ^= 0xFF;                      /* checksum */
    DROP(icmp_bad, len);
    /* UDP */
    eth_hdr(b, MY_MAC, PEER_MAC, ETH_IPV4);
    len = ip_pkt(b, PEER_IP, MY_IP, 17, d, 7, false);  /* short */
    DROP(udp_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    wr16(b + 34 + 4, 13);                    /* length > IP payload */
    DROP(udp_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    wr16(b + 34 + 4, 7);                     /* length < header */
    DROP(udp_bad, len);
    len = udp_frame(b, MY_MAC, PEER_IP, MY_IP, 9, 7, d, 4);
    b[len - 1] ^= 1;                         /* checksum */
    DROP(udp_bad, len);
#undef DROP
    teardown();
}

/* Every truncation of valid frames, and random frames to our MAC: no
 * crash (run under ASan), nothing delivered from a cut frame. */
TEST(net_truncation_and_noise) {
    setup();
    Actor *a = new_actor(), *o = new_actor();
    configure(a);
    open_udp(o, 7);
    uint8_t b[1600], d[64];
    memset(d, 0x11, sizeof d);
    uint32_t lens[3];
    static uint8_t frames[3][1600];
    lens[0] = udp_frame(frames[0], MY_MAC, PEER_IP, MY_IP, 9, 7, d, 64);
    lens[1] = echo_frame(frames[1], PEER_IP, MY_IP, 1, 1, d, 64);
    lens[2] = arp_frame(frames[2], 1, PEER_MAC, PEER_IP, MY_MAC, MY_IP, MY_MAC);
    for (int k = 0; k < 3; k++) {
        for (uint32_t n = 0; n < lens[k]; n++) {
            memcpy(b, frames[k], n);
            tx.n = 0;
            rx(b, n);
            CHECK_EQ(tx.n, 0);
            CHECK_EQ(mbox_len(o), 0);
        }
    }
    srand(12345);
    for (int i = 0; i < 20000; i++) {
        uint32_t n = (uint32_t)(rand() % 200);
        memcpy(b, frames[i % 3], 60);
        for (uint32_t j = 14 + (uint32_t)(rand() % 30); j < n; j++) b[j] = (uint8_t)rand();
        if (i & 1) { wr16(b + 24, 0); wr16(b + 24, csum(b + 14, 20)); }   /* a valid IP header checksum */
        rx(b, n);
        while (o->mbox_head) pop(o);
    }
    /* a broadcast storm of ARP requests for other hosts: nothing sent */
    tx.n = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t len = arp_frame(b, 1, OTHER_MAC, IP4(10, 0, 1, i & 0xFF), (const uint8_t *)"\0\0\0\0\0\0",
                                 IP4(10, 0, 2, i & 0xFF), net_bcast_mac);
        rx(b, len);
    }
    CHECK_EQ(tx.n, 0);
    teardown();
}
