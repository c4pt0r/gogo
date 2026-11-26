/* Two Vms on two threads run TLS handshakes at the same time against one
 * real TLS server (tests/net/tls_threads_test.go starts it and runs this
 * program), each with its own wall clock: Vm 0's is now (the certificate is
 * valid), Vm 1's three days on (the certificate has expired). mbedTLS's
 * time hooks are process-global; each Vm must still check the certificate
 * against its own clock: Vm 0 gets Connected, Vm 1 Closed{BadCert}.
 *
 *   test_tls_threads <port> <ca.pem> <host>
 *
 * Each thread owns its Vm, its NetState and a minimal TCP peer for it (one
 * segment in flight each way) that relays the stream to 127.0.0.1:<port>.
 * Prints "vm<i> connected" or "vm<i> closed <reason>" per Vm; exits 0 when
 * both are as expected. */
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "net_int.h"
#include "tls.h"
#include "actor.h"
#include "heap.h"
#include "image.h"
#include "module.h"
#include "opcodes.h"
#include "port.h"
#include "host.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PEER_IP IP4(10, 0, 0, 1)
#define VM_IP IP4(10, 0, 0, 2)
#define PEER_PORT 443u
#define QMAX 32
#define OUT_CAP 65536u
#define RESEND_MS 20u
#define LIMIT_MS 30000u
static const uint8_t PEER_MAC[6] = { 2, 0, 0, 0, 0, 0x99 };

static int srv_port;
static const char *host_name;
static unsigned char *ca_pem;
static size_t ca_len;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- the board's TlsHw (thread-safe: no counters) ---- */

static void *hw_alloc(size_t n) { return calloc(1, n); }
static void hw_free(void *p) { free(p); }
static int hw_verify(struct mbedtls_ssl_config *conf, void **state) {
    mbedtls_x509_crt *ca = calloc(1, sizeof *ca);
    if (!ca) return -1;
    mbedtls_x509_crt_init(ca);
    if (mbedtls_x509_crt_parse(ca, ca_pem, ca_len) < 0) {
        mbedtls_x509_crt_free(ca);
        free(ca);
        return -1;
    }
    mbedtls_ssl_conf_ca_chain(conf, ca, NULL);
    *state = ca;
    return 0;
}
static void hw_verify_free(void *state) {
    mbedtls_x509_crt_free(state);
    free(state);
}
static uint64_t hw_clock_us(void) { return mono_ms() * 1000u; }
static int hw_random(uint8_t *out, size_t n) {
    while (n) {
        size_t k = n < 256u ? n : 256u;
        if (getentropy(out, k) != 0) return -1;
        out += k;
        n -= k;
    }
    return 0;
}
static const TlsHw hw = { hw_alloc, hw_free, hw_verify, hw_verify_free, hw_clock_us, hw_random };

/* ---- the host (one per process; every hook is thread-safe) ---- */

static void h_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    fwrite(s, 1, n, stream == 2 ? stderr : stdout);
}
static void *h_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void h_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
static uint64_t h_now(void *ctx) { (void)ctx; return mono_ms(); }
static void h_wake(void *ctx) { (void)ctx; }
static const gogo_host host = { .write = h_write, .alloc = h_alloc, .free = h_free, .now_ms = h_now, .wake = h_wake };

/* ---- one Vm and its peer ---- */

typedef struct Peer {
    int idx;
    int64_t wall_ms;
    Vm vm;
    NetState ns;
    VmBoard board;
    NetIf nif;
    uint32_t img_w[32];
    Image img;
    Actor *owner;
    /* frames for the Vm */
    uint8_t q[QMAX][1600];
    uint32_t qlen[QMAX];
    int qh, qn;
    uint8_t vm_mac[6];
    /* the TCP peer */
    int sock;
    bool syn_seen, syn_acked, reset;
    uint16_t vm_port, wnd;
    uint32_t rcv_nxt, snd_una, snd_nxt;
    uint64_t sent_at;
    uint8_t out[OUT_CAP];        /* from the server; out[0] is at snd_una */
    uint32_t out_len;
    int result;                  /* 0 pending, 1 connected, 2 closed */
    int32_t reason;
} Peer;

static Peer peers[2];

static uint8_t *q_slot(Peer *pe) {
    if (pe->qn == QMAX) return NULL;
    return pe->q[(pe->qh + pe->qn) % QMAX];
}
static void q_push(Peer *pe, uint32_t len) { pe->qlen[(pe->qh + pe->qn++) % QMAX] = len; }

static void eth_hdr(Peer *pe, uint8_t *f, uint16_t type) {
    memcpy(f, pe->vm_mac, 6);
    memcpy(f + 6, PEER_MAC, 6);
    wr16(f + 12, type);
}

static void send_tcp(Peer *pe, uint32_t seq, uint8_t fl, const uint8_t *d, uint32_t n) {
    uint8_t *f = q_slot(pe);
    if (!f) return;                  /* the Vm resends */
    bool syn = (fl & 0x02u) != 0;
    uint32_t hl = syn ? 24u : 20u;
    eth_hdr(pe, f, ETH_IPV4);
    uint8_t *ip = f + 14, *t = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    wr16(ip + 2, 20 + hl + n);
    wr16(ip + 6, 0x4000u);
    ip[8] = 64;
    ip[9] = IP_TCP;
    wr32(ip + 12, PEER_IP);
    wr32(ip + 16, VM_IP);
    wr16(ip + 10, (uint16_t)~net_fold(net_sum(0, ip, 20)));
    memset(t, 0, hl);
    wr16(t, PEER_PORT);
    wr16(t + 2, pe->vm_port);
    wr32(t + 4, seq);
    wr32(t + 8, pe->rcv_nxt);
    t[12] = (uint8_t)(hl << 2);
    t[13] = (uint8_t)(fl | 0x10u);   /* always ACK */
    wr16(t + 14, 65535u);
    if (syn) { t[20] = 2; t[21] = 4; wr16(t + 22, TCP_MSS); }
    if (n) memcpy(t + hl, d, n);
    uint8_t ph[12];
    wr32(ph, PEER_IP); wr32(ph + 4, VM_IP); wr16(ph + 8, IP_TCP); wr16(ph + 10, hl + n);
    wr16(t + 16, (uint16_t)~net_fold(net_sum(net_sum(0, ph, 12), t, hl + n)));
    q_push(pe, 14u + 20u + hl + n);
}

static void write_all(int fd, const uint8_t *p, uint32_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (w <= 0) return;
        p += w;
        n -= (uint32_t)w;
    }
}

static int dial_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)srv_port);
    a.sin_addr.s_addr = htonl(0x7F000001u);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

/* A TCP segment from the Vm. */
static void peer_tcp(Peer *pe, const uint8_t *t, uint32_t len) {
    uint32_t hl = (uint32_t)(t[12] >> 4) * 4u;
    if (len < 20u || hl > len || rd16(t + 2) != PEER_PORT) return;
    uint8_t fl = t[13];
    uint32_t seq = rd32(t + 4), ack = rd32(t + 8), n = len - hl;
    if (fl & 0x04u) {                /* RST */
        pe->reset = true;
        return;
    }
    if (fl & 0x02u) {                /* SYN (again: the SYN-ACK is resent) */
        if (!pe->syn_seen) {
            pe->syn_seen = true;
            pe->vm_port = rd16(t);
            pe->rcv_nxt = seq + 1u;
            pe->snd_una = 7000u;
            pe->snd_nxt = 7001u;
            pe->sock = dial_server();
        }
        send_tcp(pe, pe->snd_una, 0x02u, NULL, 0);
        pe->sent_at = mono_ms();
        return;
    }
    if (fl & 0x10u) {
        uint32_t a = ack - pe->snd_una;
        if (a && a <= pe->snd_nxt - pe->snd_una) {
            pe->snd_una = ack;
            if (!pe->syn_acked) {
                pe->syn_acked = true;
                a--;
            }
            memmove(pe->out, pe->out + a, pe->out_len - a);
            pe->out_len -= a;
        }
        pe->wnd = rd16(t + 14);
    }
    bool ack_it = false;
    if (n) {
        if (seq == pe->rcv_nxt && pe->sock >= 0) {
            write_all(pe->sock, t + hl, n);
            pe->rcv_nxt += n;
        }
        ack_it = true;
    }
    if ((fl & 0x01u) && seq + n == pe->rcv_nxt) {
        pe->rcv_nxt++;
        if (pe->sock >= 0) shutdown(pe->sock, SHUT_WR);
        ack_it = true;
    }
    if (ack_it) send_tcp(pe, pe->snd_nxt, 0, NULL, 0);
}

/* NetIf.send: a frame from the Vm (inside its drain, on its thread). */
static int nif_send(void *ctx, const uint8_t *f, uint32_t len) {
    Peer *pe = ctx;
    if (len < 14u) return 0;
    memcpy(pe->vm_mac, f + 6, 6);
    uint16_t type = rd16(f + 12);
    const uint8_t *p = f + 14;
    if (type == ETH_ARP && len >= 42u && rd16(p + 6) == 1u && rd32(p + 24) == PEER_IP) {
        uint8_t *r = q_slot(pe);
        if (!r) return 0;
        eth_hdr(pe, r, ETH_ARP);
        uint8_t *a = r + 14;
        wr16(a, 1); wr16(a + 2, ETH_IPV4); a[4] = 6; a[5] = 4; wr16(a + 6, 2);
        memcpy(a + 8, PEER_MAC, 6); wr32(a + 14, PEER_IP);
        memcpy(a + 18, p + 8, 6); memcpy(a + 24, p + 14, 4);
        q_push(pe, 42);
    } else if (type == ETH_IPV4 && len >= 34u && p[9] == IP_TCP) {
        uint32_t ihl = (uint32_t)(p[0] & 0xFu) * 4u, tl = rd16(p + 2);
        if (tl <= len - 14u && ihl < tl) peer_tcp(pe, p + ihl, tl - ihl);
    }
    return 0;
}
static bool nif_mac(void *ctx, uint8_t mac[6]) {
    const Peer *pe = ctx;
    static const uint8_t base[6] = { 2, 0, 0, 0, 0, 0x10 };
    memcpy(mac, base, 6);
    mac[5] = (uint8_t)(mac[5] + pe->idx);
    return true;
}
static bool nif_link_up(void *ctx) { (void)ctx; return true; }
static void nif_report(void *ctx, bool up, uint32_t ip, uint32_t gw, uint32_t dns) {
    (void)ctx; (void)up; (void)ip; (void)gw; (void)dns;
}

/* Server bytes in, the next segment out (or the one in flight again). */
static void peer_pump(Peer *pe) {
    if (pe->sock >= 0 && pe->out_len < OUT_CAP) {
        ssize_t r = recv(pe->sock, pe->out + pe->out_len, OUT_CAP - pe->out_len, 0);
        if (r > 0) pe->out_len += (uint32_t)r;
    }
    uint64_t now = mono_ms();
    if (!pe->syn_seen) return;
    if (pe->snd_nxt != pe->snd_una) {
        if (now - pe->sent_at < RESEND_MS) return;
        uint32_t n = pe->snd_nxt - pe->snd_una - (pe->syn_acked ? 0u : 1u);
        if (!pe->syn_acked) send_tcp(pe, pe->snd_una, 0x02u, NULL, 0);
        else send_tcp(pe, pe->snd_una, 0x08u, pe->out, n);
        pe->sent_at = now;
        return;
    }
    if (!pe->out_len || !pe->wnd) return;
    uint32_t n = pe->out_len < TCP_MSS ? pe->out_len : TCP_MSS;
    if (n > pe->wnd) n = pe->wnd;
    send_tcp(pe, pe->snd_una, 0x08u, pe->out, n);
    pe->snd_nxt += n;
    pe->sent_at = now;
}

static void drain_vm(Peer *pe) {
    for (int k = 0; k < 64 && port_work_pending(&pe->vm); k++) port_drain(&pe->vm, vm_now(&pe->vm));
}

/* Frames to the Vm, one at a time through its single RX slot. */
static void deliver(Peer *pe) {
    while (pe->qn) {
        net_rx_offer(&pe->ns, pe->q[pe->qh], pe->qlen[pe->qh]);
        pe->qh = (pe->qh + 1) % QMAX;
        pe->qn--;
        drain_vm(pe);
    }
}

static void check_mbox(Peer *pe) {
    for (const Msg *m = pe->owner->mbox_head; m; m = m->next) {
        if (m->data[1] == TYPE_NET_CONNECTED) pe->result = 1;
        if (m->data[1] == TYPE_CLOSED) {
            pe->result = 2;
            pe->reason = term_to_int(m->data[3]);   /* after From */
        }
    }
}

static term str_at(Vm *v, Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(v, a, words, NULL, 0);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}
static term struct_at(Vm *v, Actor *a, uint32_t type, const term *f, uint32_t n) {
    uint32_t off = heap_alloc(v, a, 2u + n, NULL, 0);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static const PortDriver *const drivers[] = { &net_netif_driver, &net_tcp_driver, &net_tls_driver, NULL };

/* Both threads dial together (no pthread_barrier on macOS). */
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_n;
static void gate(void) {
    pthread_mutex_lock(&gate_mu);
    if (++gate_n == 2) pthread_cond_broadcast(&gate_cv);
    while (gate_n < 2) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}

static void *run(void *arg) {
    Peer *pe = arg;
    pe->sock = -1;
    uint32_t *w = pe->img_w;
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    pe->nif = (NetIf){ nif_mac, nif_send, nif_link_up, nif_report, pe };
    pe->board = (VmBoard){ drivers, NULL, &pe->nif, &hw, NULL };
    if (image_load(&pe->img, w, img_seal(w)) != IMG_OK ||
        vm_init(&pe->vm, &host, NULL, &pe->board, 16) != 0 ||
        module_install_static(&pe->vm, &pe->img) != GOGO_E_OK) {
        gate();
        return NULL;
    }
    Vm *vm = &pe->vm;
    net_install(vm, &pe->ns);
    Actor *a = vm_set_unix_time_ms(vm, pe->wall_ms) == 1 ? actor_new(vm, 256) : NULL;
    if (!a) {
        gate();
        return NULL;
    }
    a->state = A_WAITING;
    a->max_words = 60000u;
    pe->owner = a;
    a->hp = 0;
    term n = port_open(vm, a, str_at(vm, a, "netif"), struct_at(vm, a, TYPE_NET_IFCONFIG, NULL, 0));
    a->hp = 0;
    term f[4] = { term_from_int((int32_t)VM_IP), term_from_int((int32_t)IP4(255, 255, 255, 0)),
                  term_from_int((int32_t)PEER_IP), term_from_int(0) };
    port_command(vm, a, n, struct_at(vm, a, TYPE_NET_SETADDR, f, 4));
    deliver(pe);                     /* the gateway's ARP reply */
    gate();                          /* both handshakes at once */
    a->hp = 0;
    term d[4] = { term_from_int((int32_t)PEER_IP), term_from_int((int32_t)PEER_PORT),
                  term_from_int((int32_t)LIMIT_MS), str_at(vm, a, host_name) };
    if (!port_is(port_open(vm, a, str_at(vm, a, "tls"), struct_at(vm, a, TYPE_NET_DIALTLS, d, 4)))) return NULL;
    uint64_t end = mono_ms() + LIMIT_MS + 5000u;
    while (!pe->result && mono_ms() < end) {
        drain_vm(pe);
        peer_pump(pe);
        deliver(pe);
        check_mbox(pe);
        if (!pe->qn && !port_work_pending(vm)) {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    port_close_all(vm);
    deliver(pe);
    if (pe->sock >= 0) close(pe->sock);
    net_uninstall(vm);
    return NULL;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    unsigned char *b = malloc(65537u);
    size_t n = b ? fread(b, 1, 65536u, fp) : 0;
    fclose(fp);
    if (!b) return NULL;
    b[n] = 0;
    *len = n + 1u;                   /* PEM wants the NUL counted */
    return b;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: test_tls_threads <port> <ca.pem> <host>\n");
        return 2;
    }
    srv_port = atoi(argv[1]);
    host_name = argv[3];
    ca_pem = read_file(argv[2], &ca_len);
    if (!ca_pem || !tls_init(&hw)) {   /* bound once, before the threads */
        fprintf(stderr, "test_tls_threads: setup failed\n");
        return 2;
    }
    int64_t now = (int64_t)time(NULL) * 1000;
    peers[0].wall_ms = now;                              /* valid */
    peers[1].wall_ms = now + 3 * 24 * 3600 * 1000LL;     /* expired */
    pthread_t th[2];
    for (int i = 0; i < 2; i++) {
        peers[i].idx = i;
        pthread_create(&th[i], NULL, run, &peers[i]);
    }
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 2; i++) {
        if (peers[i].result == 1) printf("vm%d connected\n", i);
        else if (peers[i].result == 2) printf("vm%d closed %d\n", i, (int)peers[i].reason);
        else printf("vm%d none\n", i);
    }
    bool ok = peers[0].result == 1 && peers[1].result == 2 && peers[1].reason == CR_BADCERT;
    return ok ? 0 : 1;
}
