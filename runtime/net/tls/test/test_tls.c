/* The "tls" port's state is per net stack: two Vms in one process, each
 * with its own NetState, each hold their own connection (GOGO_TLS_MAX per
 * stack) and counters; a Vm whose board brings a different TlsHw is refused
 * (mbedTLS's hooks are process-global), logged once. Opens only: no
 * handshake runs (the SYN is never even sent: nothing is drained). */
#include "test.h"
#include "test_host.h"
#include "net_int.h"
#include "tls.h"
#include "actor.h"
#include "heap.h"
#include "alloc.h"
#include "bytes.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "net_unix.h"
#include "mbedtls/platform.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>   /* getentropy (glibc declares it here, not in unistd.h at -std=c99) */

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PEER_IP IP4(10, 0, 0, 1)

/* ---- a TlsHw without trust anchors (no handshake runs) ---- */

static int alloc_fails;      /* hw_alloc refuses everything (PSA's init fails) */
static void *hw_alloc(size_t n) { return alloc_fails ? NULL : calloc(1, n); }
static void hw_free(void *p) { free(p); }
static int hw_verify(struct mbedtls_ssl_config *conf, void **state) { (void)conf; *state = NULL; return 0; }
static void hw_verify_free(void *state) { (void)state; }
static int hw_random(uint8_t *out, size_t n) {
    while (n) {
        size_t k = n < 256u ? n : 256u;
        if (getentropy(out, k) != 0) return -1;
        out += k;
        n -= k;
    }
    return 0;
}
static const TlsHw hw_a = { hw_alloc, hw_free, hw_verify, hw_verify_free, NULL, hw_random };
static const TlsHw hw_b = { hw_alloc, hw_free, hw_verify, hw_verify_free, NULL, hw_random };   /* another table */

/* ---- fixture: up to three Vms, each with a stack ---- */

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm[3];
static NetState nst[3];
static VmBoard board[3];
static const PortDriver *const drivers[] = { &net_netif_driver, &net_tcp_driver, &net_tls_driver, NULL };

static int tx_hook(const uint8_t *f, uint32_t len) { (void)f; (void)len; return 0; }

static uint32_t errs;
static void count_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx; (void)s; (void)n;
    if (stream == 2) errs++;
}

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

static term str_at(Vm *v, Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(v, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term struct_at(Vm *v, Actor *a, uint32_t type, const term *f, uint32_t n) {
    uint32_t off = heap_alloc(v, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

/* Vm i on a board with TlsHw hw: its stack configured (10.0.0.2+i/24),
 * the wall clock set; returns the actor that dials. */
static Actor *vm_up(int i, const TlsHw *hw) {
    va_reset(&vm[i]);
    bytes_reset(&vm[i]);
    CHECK_EQ(test_vm_init(&vm[i], &img, 16), 0);
    board[i] = test_board;
    board[i].tls = hw;
    vm[i].board = &board[i];
    port_set_drivers(&vm[i], drivers);
    net_install(&vm[i], &nst[i]);
    CHECK(vm[i].net == &nst[i]);
    CHECK_EQ(vm_set_unix_time_ms(&vm[i], GOGO_BUILD_UNIX_MS + 1000), 1);
    Actor *a = actor_new(&vm[i], 256);
    CHECK(a != NULL);
    a->state = A_WAITING;
    a->max_words = 60000u;               /* room for a connection's charge */
    a->hp = 0;
    term n = port_open(&vm[i], a, str_at(&vm[i], a, "netif"), struct_at(&vm[i], a, TYPE_NET_IFCONFIG, NULL, 0));
    CHECK(test_port_is(n));
    a->hp = 0;
    term f[4] = { term_from_int((int32_t)IP4(10, 0, 0, 2 + i)), term_from_int((int32_t)IP4(255, 255, 255, 0)),
                  term_from_int((int32_t)PEER_IP), term_from_int(0) };
    port_command(&vm[i], a, n, struct_at(&vm[i], a, TYPE_NET_SETADDR, f, 4));
    CHECK_EQ(nst[i].ip, IP4(10, 0, 0, 2 + i));
    return a;
}

static term dial_tls(int i, Actor *a) {
    a->hp = 0;
    term f[4] = { term_from_int((int32_t)PEER_IP), term_from_int(443), term_from_int(0),
                  str_at(&vm[i], a, "example.com") };
    return port_open(&vm[i], a, str_at(&vm[i], a, "tls"), struct_at(&vm[i], a, TYPE_NET_DIALTLS, f, 4));
}

TEST(tls_state_per_stack) {
    test_host_arena_forget();
    mini_image();
    platform_unix_net_set_tx(tx_hook);
    void (*old_write)(void *, int, const char *, size_t) = test_host.write;
    test_host.write = count_write;
    errs = 0;

    /* The process's first tls_init, with PSA failing (its allocations
     * refused): false, but the TlsHw stays bound, so mbedTLS's hooks (now
     * installed) still work for anyone who allocates through them, another
     * TlsHw is refused, and a retry with the same one succeeds. This must
     * stay the first tls_init of the process: the binding and PSA's state
     * are process-wide and never reset, so a test that runs tls_init
     * earlier would make this one fail. */
    alloc_fails = 1;
    CHECK(!tls_init(&hw_a));
    CHECK(mbedtls_calloc(1, 16) == NULL);
    alloc_fails = 0;
    void *p = mbedtls_calloc(4, 4);
    CHECK(p != NULL);
    mbedtls_free(p);
    CHECK(!tls_init(&hw_b));
    CHECK(tls_init(&hw_a));
    CHECK(tls_init(&hw_a));

    Actor *a0 = vm_up(0, &hw_a), *a1 = vm_up(1, &hw_a);

    /* Each stack holds its own connection: the other Vm's is no obstacle. */
    term t0 = dial_tls(0, a0);
    CHECK(test_port_is(t0));
    CHECK(nst[0].tls.conn != NULL && nst[1].tls.conn == NULL);
    CHECK(dial_tls(0, a0) == T_NIL);          /* GOGO_TLS_MAX (1) on this stack */
    term t1 = dial_tls(1, a1);
    CHECK(test_port_is(t1));
    CHECK(nst[1].tls.conn != NULL && nst[1].tls.conn != nst[0].tls.conn);
    CHECK_EQ(tls_stats(&nst[0])->opens, 1u);
    CHECK_EQ(tls_stats(&nst[1])->opens, 1u);
    CHECK_EQ(tls_stats(&nst[0])->live, 1u);
    CHECK_EQ(tls_stats(&nst[1])->live, 1u);
    CHECK(nst[0].upper != NULL && nst[0].upper == nst[1].upper);

    /* Closing one frees only its own. */
    port_close(&vm[0], t0);
    CHECK(nst[0].tls.conn == NULL && nst[1].tls.conn != NULL);
    CHECK_EQ(tls_stats(&nst[0])->live, 0u);
    CHECK_EQ(tls_stats(&nst[1])->live, 1u);
    CHECK(test_port_is(dial_tls(0, a0)));          /* and that stack may open again */
    CHECK_EQ(tls_stats(&nst[0])->opens, 2u);
    CHECK_EQ(errs, 0u);

    /* A board with another TlsHw: refused, logged once per process. */
    Actor *a2 = vm_up(2, &hw_b);
    CHECK(dial_tls(2, a2) == T_NIL);
    CHECK(dial_tls(2, a2) == T_NIL);
    CHECK(nst[2].tls.conn == NULL);
    CHECK_EQ(tls_stats(&nst[2])->opens, 0u);
    CHECK_EQ(errs, 1u);
    CHECK(!tls_init(&hw_b));
    CHECK(tls_init(&hw_a));

    for (int i = 0; i < 3; i++) {
        port_close_all(&vm[i]);
        net_uninstall(&vm[i]);
    }
    CHECK(nst[0].tls.conn == NULL && nst[1].tls.conn == NULL);
    test_host.write = old_write;
    platform_unix_net_set_tx(NULL);
}
