/* The "tls" port over mbedTLS (tls.h has the contract and the design). */
#include "tls.h"
#include "net_int.h"
#include "heap.h"
#include "bytes.h"
#include "msg.h"
#include "opcodes.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

#if (GOGO_TLS_MAX) != 1u
#error "GOGO_TLS_MAX: only 1 is implemented"
#endif

#define HOST_MAX 253u
#define RX_CAP TCP_MSS           /* ciphertext buffer: one segment (our window) */
#define DATA_MAX TCP_MSS         /* plaintext per Data event */
#define RETRY_MS 50u
/* The record buffers mbedTLS allocates: content plus header, IV, MAC and
 * padding (MBEDTLS_SSL_*_BUFFER_LEN, internal to mbedTLS; an upper bound). */
#define RECORD_SLACK 512u

/* TlsConn.flags */
enum {
    TS_KICK = 1,        /* the drain has work */
    TS_HS_DONE = 2,     /* handshake over, Connected posted */
    TS_FIN = 4,         /* the peer's FIN taken: the ciphertext ends with rx */
    TS_CLOSED = 8,      /* the terminal Closed posted (any reason but Normal) */
    TS_SHUT = 16,       /* Shutdown asked: close_notify after the writes */
    TS_SHUT_DONE = 32,  /* close_notify acknowledged, FIN queued */
    TS_DEAD = 64,       /* failed: reset at the next drain */
    TS_RETRY = 128,     /* a Data waits for the owner's room until retry_at */
    TS_EOF = 256        /* Closed{Normal} posted (the peer finished sending) */
};

typedef struct TlsConn {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    void *verify;               /* the platform's (platform_tls_verify_setup) */
    const Vm *vm;               /* the Vm whose port this is (its wall clock) */
    TcpPcb *c;
    /* Output in flight (bio_send): mbedTLS's own buffer from tx_base, tx_len
     * bytes, of which tx_acked are acknowledged but not yet reported. */
    const uint8_t *tx_base;
    uint32_t tx_len, tx_acked;
    uint32_t retry_at;
    uint64_t hs_t0;             /* platform_tls_clock_us at the first handshake slice */
    uint16_t rx_off, rx_len;    /* ciphertext waiting for mbedTLS */
    uint16_t flags;
    uint8_t verify_set;
    uint8_t rx[RX_CAP];
} TlsConn;

#define TLS_CHARGE_BYTES (sizeof(TlsConn) + MBEDTLS_SSL_IN_CONTENT_LEN + MBEDTLS_SSL_OUT_CONTENT_LEN + \
                          2u * RECORD_SLACK)
#define TLS_CHARGE_WORDS ((uint16_t)((TLS_CHARGE_BYTES + 3u) / 4u))
/* Room for one Data (DATA_MAX bytes) and the control events after it. */
#define DATA_WORDS (3u + 2u + (DATA_MAX + 3u) / 4u)
#define ROOM_64W ((uint8_t)((msg_footprint_words(DATA_WORDS) + TCP_EVENT_RESERVE + 63u) / 64u))

static TlsConn *tc;
static TlsStats st;

const TlsStats *tls_stats(void) { return &st; }

/* ---- weak platform defaults ---- */

__attribute__((weak)) void *platform_tls_alloc(size_t n) { return calloc(1, n); }
__attribute__((weak)) void platform_tls_free(void *p) { free(p); }
__attribute__((weak)) int platform_tls_verify_setup(struct mbedtls_ssl_config *conf, void **state) {
    (void)conf; (void)state;
    return -1;
}
__attribute__((weak)) void platform_tls_verify_free(void *state) { (void)state; }
__attribute__((weak)) uint64_t platform_tls_clock_us(void) { return 0; }
__attribute__((weak)) int platform_tls_random(uint8_t *out, size_t n) {
    (void)out; (void)n;
    return -1;
}

/* ---- mbedTLS platform glue ---- */

#if GOGO_TLS_MBEDTLS_HOOKS
static void *tls_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    size_t total = n * size;
    return platform_tls_alloc(total != 0u ? total : 1u);
}

static mbedtls_time_t tls_time(mbedtls_time_t *t) {
    /* mbedTLS asks only while verifying a peer: the open connection's Vm. */
    mbedtls_time_t now = (mbedtls_time_t)(tc ? vm_unix_time_ms(tc->vm) / 1000 : 0);
    if (t) *t = now;
    return now;
}
#endif

#if GOGO_TLS_MBEDTLS_HOOKS
#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)platform_now_ms(); }
#endif

/* PSA's randomness: the platform's cryptographic source
 * (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG); never platform_random, which may be
 * predictable. With GOGO_TLS_MBEDTLS_HOOKS 0 the platform's mbedTLS port
 * defines it (ESP-IDF: esp_fill_random, the source platform_tls_random
 * uses too), and platform_tls_random only guards open. */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *ctx, uint8_t *out,
                                             size_t size, size_t *len) {
    (void)ctx;
    *len = 0;
    if (platform_tls_random(out, size) != 0) return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *len = size;
    return PSA_SUCCESS;
}
#endif

bool tls_init(void) {
    static uint8_t done;
    if (done) return true;
#if GOGO_TLS_MBEDTLS_HOOKS
    mbedtls_platform_set_calloc_free(tls_calloc, platform_tls_free);
    mbedtls_platform_set_time(tls_time);
#endif
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    done = 1;
    return true;
}

/* ---- the bio: TCP underneath ---- */

/* Output stays in mbedTLS's buffer until TCP has it acknowledged: the first
 * call offers it (TCP sends from it), later calls report what was ACKed. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    TlsConn *t = ctx;
    if (!t->tx_len) {
        t->tx_base = buf;
        t->tx_len = (uint32_t)len;
        t->tx_acked = 0;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (buf != t->tx_base || len < t->tx_len) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    t->tx_len = (uint32_t)len;              /* records added behind (a flight) */
    if (!t->tx_acked) return MBEDTLS_ERR_SSL_WANT_WRITE;
    uint32_t n = t->tx_acked;
    t->tx_base += n;
    t->tx_len -= n;
    t->tx_acked = 0;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    TlsConn *t = ctx;
    uint32_t n = (uint32_t)(t->rx_len - t->rx_off);
    if (!n) return (t->flags & TS_FIN) ? 0 : MBEDTLS_ERR_SSL_WANT_READ;   /* 0: EOF */
    if (n > len) n = (uint32_t)len;
    memcpy(buf, t->rx + t->rx_off, n);
    t->rx_off = (uint16_t)(t->rx_off + n);
    if (t->rx_off == t->rx_len) t->rx_off = t->rx_len = 0;
    return (int)n;
}

/* ---- connection state ---- */

static void conn_free(TlsConn *t) {
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    if (t->verify_set) platform_tls_verify_free(t->verify);
    if (tc == t) tc = NULL;
    platform_tls_free(t);
    st.live = 0;
}

/* ECDHE groups in our order of preference. X25519 first where the crypto
 * has it (ESP-IDF): the ECDHE step is not sliced and X25519 is
 * the cheaper one on the ESP32-S3 (StickS3, mbedTLS -O2: 204 ms against
 * P-256's ~300 ms; docs/known-issues.md). */
static const uint16_t groups[] = {
#if defined(PSA_WANT_ECC_MONTGOMERY_255) && !defined(GOGO_TLS_NO_X25519)
    MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
#endif
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1, MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

/* AEAD suites with ECDHE only (forward secrecy, no CBC), whatever else the
 * platform's mbedTLS has enabled (ESP-IDF's is shared and configured for
 * more); a suite the build lacks is skipped. The host config
 * (MBEDTLS_SSL_CIPHERSUITES) is the same list. */
static const int suites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, 0
};

static TlsConn *conn_new(const char *host) {
    TlsConn *t = platform_tls_alloc(sizeof *t);
    if (!t) return NULL;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;
    if (platform_tls_verify_setup(&t->conf, &t->verify) != 0) goto fail;
    t->verify_set = 1;
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_groups(&t->conf, groups);
    mbedtls_ssl_conf_ciphersuites(&t->conf, suites);
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);   /* 1.3 cannot be sliced */
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0 || mbedtls_ssl_set_hostname(&t->ssl, host) != 0) goto fail;
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    psa_interruptible_set_max_ops(GOGO_TLS_MAX_OPS);
    return t;
fail:
    conn_free(t);
    return NULL;
}

/* Closed{reason} to the owner: Normal at most once, then the terminal
 * one (any other reason) at most once; after the terminal one TCP posts no
 * Closed of its own (u_closed). */
static void post_closed(Vm *vm, TlsConn *t, int32_t reason) {
    uint16_t f = reason == CR_NORMAL ? TS_EOF : TS_CLOSED;
    if (t->flags & (f | TS_CLOSED)) return;
    t->flags |= f;
    tcp_post(vm, t->c, TYPE_NET_CLOSED, reason);
}

/* A failure: Closed{reason} now, the reset at the next drain (callers
 * inside TCP's input must not end the connection). */
static void fail(Vm *vm, TlsConn *t, int32_t reason) {
    post_closed(vm, t, reason);
    t->flags |= TS_DEAD | TS_KICK;
}

static int32_t reason_of(int r) {
    st.last_error = r;
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) return CR_BADCERT;
    if (r == MBEDTLS_ERR_SSL_CONN_EOF) return CR_RESET;
    return CR_TLS;
}

static uint64_t slice_start(void) { return platform_tls_clock_us(); }
static uint32_t slice_us(uint64_t t0) {
    uint64_t t1 = platform_tls_clock_us();
    return t1 > t0 ? (uint32_t)(t1 - t0) : 0u;
}
static void io_slice(uint64_t t0) {
    uint32_t us = slice_us(t0);
    if (us > st.max_io_us) st.max_io_us = us;
}

/* ---- the drain's work ---- */

static void handshake(Vm *vm, TlsConn *t) {
    int32_t state = t->ssl.MBEDTLS_PRIVATE(state);
    uint64_t t0 = slice_start();
    if (!t->hs_t0) t->hs_t0 = t0 ? t0 : 1u;
    int r = mbedtls_ssl_handshake_step(&t->ssl);
    uint32_t us = slice_us(t0);
    st.steps++;
    if (us > st.max_step_us) {
        st.max_step_us = us;
        st.max_step_state = state;
    }
    if (r == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) {
        st.restarts++;
        t->flags |= TS_KICK;
        return;
    }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return;   /* rx or an ACK kicks */
    if (r != 0) {
        fail(vm, t, reason_of(r));
        return;
    }
    t->flags |= TS_KICK;
    if (!mbedtls_ssl_is_handshake_over(&t->ssl)) return;
    t->flags |= TS_HS_DONE;
    st.handshakes++;
    st.last_hs_us = slice_us(t->hs_t0);
    tcp_until_clear(t->c);
    tcp_post(vm, t->c, TYPE_NET_CONNECTED, -1);
}

/* The Writes in order, a record (at most OUT_CONTENT_LEN) at a time; a
 * Write is Written once its last record is acknowledged. */
static void pump_write(Vm *vm, TlsConn *t) {
    TcpPcb *c = t->c;
    while (c->wr[0] && !(t->flags & TS_DEAD)) {
        uint32_t len = 0;
        const uint8_t *d = bytes_data(vm, c->wr[0], &len);
        if (!d || c->wr_off >= len) {
            tcp_write_done(vm, c);
            continue;
        }
        uint32_t n = len - c->wr_off;
        if (n > MBEDTLS_SSL_OUT_CONTENT_LEN) n = MBEDTLS_SSL_OUT_CONTENT_LEN;
        uint64_t t0 = slice_start();
        int r = mbedtls_ssl_write(&t->ssl, d + c->wr_off, n);   /* the same call again after WANT_WRITE */
        io_slice(t0);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) return;
        if (r < 0) {
            fail(vm, t, reason_of(r));
            return;
        }
        c->wr_off += (uint32_t)r;
    }
}

/* Decrypted data to the owner while it has room. 0: nothing more for now;
 * 1: blocked on the owner's room (retried); -1: failed. */
static int pump_read(Vm *vm, TlsConn *t) {
    Port *p = t->c->port;
    while (!(t->flags & (TS_EOF | TS_CLOSED | TS_DEAD))) {
        uint64_t t0 = slice_start();
        int r = mbedtls_ssl_read(&t->ssl, NULL, 0);   /* decrypts the next record, takes nothing */
        io_slice(t0);
        size_t avail = mbedtls_ssl_get_bytes_avail(&t->ssl);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            post_closed(vm, t, CR_NORMAL);
            return 0;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r < 0) {
            fail(vm, t, reason_of(r));
            return -1;
        }
        if (!avail) {
            if ((t->flags & TS_FIN) && t->rx_len == t->rx_off) {   /* EOF at a record boundary */
                post_closed(vm, t, CR_NORMAL);
                return 0;
            }
            continue;                               /* an empty record */
        }
        uint32_t n = avail < DATA_MAX ? (uint32_t)avail : DATA_MAX;
        uint32_t bw = 2u + (n + 3u) / 4u;
        Actor *o = port_owner(vm, p);
        if (!o) return 0;                           /* exiting: its close frees us */
        Msg *m = tcp_flush(vm, p) ? net_msg_new(vm, 3u + bw) : NULL;
        if (!m) return 1;
        if (!net_room(vm, o, msg_charge_words(vm, m), TCP_EVENT_RESERVE)) {
            msg_free(vm, m);
            return 1;
        }
        uint32_t *d = m->data;
        d[0] = hdr_make(3, K_STRUCT);
        d[1] = TYPE_NET_DATA;
        d[2] = term_from_ref(3);
        d[3] = hdr_make(bw, K_BYTES);
        d[4] = n;
        d[3u + bw - 1u] = 0;                        /* padding */
        t0 = slice_start();
        r = mbedtls_ssl_read(&t->ssl, (unsigned char *)&d[5], n);
        io_slice(t0);
        if (r != (int)n || !net_deliver(vm, o, m, TCP_EVENT_RESERVE)) {   /* cannot happen */
            msg_free(vm, m);
            fail(vm, t, CR_TLS);
            return -1;
        }
    }
    return 0;
}

static void drain_conn(Vm *vm, TlsConn *t) {
    TcpPcb *c = t->c;
    t->flags &= (uint16_t)~(TS_KICK | TS_RETRY);
    if (!(t->flags & TS_DEAD) && c->state != TCP_SYN_SENT) {
        if (!(t->flags & TS_HS_DONE)) {
            handshake(vm, t);
        } else {
            pump_write(vm, t);
            if (pump_read(vm, t) > 0) {
                t->flags |= TS_RETRY;
                t->retry_at = (uint32_t)platform_now_ms() + RETRY_MS;
            }
            if ((t->flags & (TS_SHUT | TS_SHUT_DONE | TS_DEAD)) == TS_SHUT && !c->wr[0]) {
                int r = mbedtls_ssl_close_notify(&t->ssl);
                if (r == 0) {
                    t->flags |= TS_SHUT_DONE;
                    tcp_shutdown(vm, c);
                } else if (r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_WANT_READ) {
                    fail(vm, t, reason_of(r));
                }
            }
        }
    }
    if (t->flags & TS_DEAD) {
        tcp_abort(vm, c, -1, true);                 /* ended() frees t */
        return;
    }
    tcp_output(vm, c);
}

/* ---- TcpUpper ---- */

/* The layer's connection, if c is it (GOGO_TLS_MAX 1: the only one). */
static TlsConn *conn_of(const TcpPcb *c) { return tc && tc->c == c ? tc : NULL; }

static bool u_rx(Vm *vm, TcpPcb *c, const uint8_t *p, uint32_t n) {
    (void)vm;
    TlsConn *t = conn_of(c);
    if (!t) return true;
    if (t->rx_off) {
        memmove(t->rx, t->rx + t->rx_off, (size_t)(t->rx_len - t->rx_off));
        t->rx_len = (uint16_t)(t->rx_len - t->rx_off);
        t->rx_off = 0;
    }
    if (t->rx_len + n > RX_CAP) return false;      /* not ACKed: the peer resends */
    memcpy(t->rx + t->rx_len, p, n);
    t->rx_len = (uint16_t)(t->rx_len + n);
    t->flags |= TS_KICK;
    return true;
}

/* The peer's FIN. After the handshake, what is left is delivered now (a
 * connection that sent its own FIN may end right after this); refused (not
 * ACKed, the peer resends) while the owner has no room for it. */
static bool u_fin(Vm *vm, TcpPcb *c) {
    TlsConn *t = conn_of(c);
    if (!t) return true;
    t->flags |= TS_FIN | TS_KICK;
    if ((t->flags & (TS_HS_DONE | TS_DEAD)) == TS_HS_DONE && pump_read(vm, t) > 0) {
        t->flags &= (uint16_t)~TS_FIN;
        t->flags |= TS_RETRY;
        t->retry_at = (uint32_t)platform_now_ms() + RETRY_MS;
        return false;
    }
    return true;
}

static void u_connected(Vm *vm, TcpPcb *c) {
    (void)vm;
    TlsConn *t = conn_of(c);
    if (t) t->flags |= TS_KICK;
}

static void u_acked(Vm *vm, TcpPcb *c, uint32_t n) {
    (void)vm;
    TlsConn *t = conn_of(c);
    if (!t) return;
    t->tx_acked += n;
    t->flags |= TS_KICK;
}

static const uint8_t *u_tx(const TcpPcb *c, uint32_t *len) {
    TlsConn *t = conn_of(c);
    if (!t) {
        *len = 0;
        return NULL;
    }
    *len = t->tx_len - t->tx_acked;
    return t->tx_base + t->tx_acked;
}

static void u_ended(TcpPcb *c) {
    TlsConn *t = conn_of(c);
    if (t) conn_free(t);
}

static bool u_closed(const TcpPcb *c) {
    const TlsConn *t = conn_of(c);
    return t && (t->flags & TS_CLOSED);
}

static bool retry_due(const TlsConn *t) {
    return (t->flags & TS_RETRY) && (int32_t)((uint32_t)platform_now_ms() - t->retry_at) >= 0;
}

static bool u_pending(void) {
    return tc && ((tc->flags & (TS_KICK | TS_DEAD)) || retry_due(tc));
}

static void u_drain(Vm *vm) {
    if (tc) drain_conn(vm, tc);
}

/* Only the blocked-delivery retry is a deadline: work left for the next
 * drain (TS_KICK, TS_DEAD) is pending(), which the scheduler checks before
 * it sleeps (vm_run). */
static bool u_next_deadline(uint64_t *deadline) {
    if (!tc || !(tc->flags & TS_RETRY)) return false;
    uint64_t now = platform_now_ms();
    int32_t left = (int32_t)(tc->retry_at - (uint32_t)now);
    *deadline = left > 0 ? now + (uint32_t)left : now;
    return true;
}

static const TcpUpper upper = {
    &net_tls_driver, u_rx, u_fin, u_connected, u_acked, u_tx, u_ended, u_pending, u_drain, u_next_deadline, u_closed
};

/* ---- the "tls" port ---- */

/* DialTLS{IP, Port, TimeoutMs, Host}. */
static int t_open(Vm *vm, const Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)index;
    const uint32_t *o = obj_of(vm, owner, cfg);
    int32_t f[3];
    const uint8_t *host;
    uint32_t hlen;
    if (!o || hdr_words(o[0]) != 6u) return -1;
    for (uint32_t i = 0; i < 3u; i++) {
        if (!int_val(vm, owner, o[2 + i], &f[i])) return -1;
    }
    if (!name_bytes(vm, owner, o[5], &host, &hlen) || hlen == 0 || hlen > HOST_MAX || tc) return -1;
    for (uint32_t i = 0; i < hlen; i++) {
        if (!host[i]) return -1;                /* not a C string */
    }
    uint8_t probe;
    if (!tls_init() || platform_tls_random(&probe, 1) != 0) return -1;   /* no secure randomness: no TLS */
    net_s.upper = &upper;
    if (vm_unix_time_ms(vm) <= 0) {             /* certificates cannot be checked */
        tcp_hold_closed(p, CR_NOTIME);
        return 0;
    }
    char name[HOST_MAX + 1u];
    memcpy(name, host, hlen);
    name[hlen] = 0;
    if (f[2] == 0) f[2] = (int32_t)GOGO_TLS_HANDSHAKE_MS;
    TcpPcb *c = tcp_dial(p, f, 3);
    if (!c) return -1;
    TlsConn *t = conn_new(name);
    if (!t) {
        tcp_close_port(vm, p);                  /* SYN_SENT: just frees the slot */
        return -1;
    }
    t->c = c;
    t->vm = vm;
    tc = t;
    c->flags |= TF_UPPER;
    p->charge_words = (uint16_t)(p->charge_words + TLS_CHARGE_WORDS);
    p->room_64w = ROOM_64W;
    st.opens++;
    st.live = 1;
    return 0;
}

/* The TLS state goes with the port. TCP then resets the connection unless
 * it was shut down, which this layer does only once its close_notify is
 * acknowledged (TS_SHUT_DONE): then TCP finishes the close as an orphan. */
static void t_close(Vm *vm, Port *p) {
    TcpPcb *c = tcp_pcb_of(p);
    if (c && (c->flags & TF_UPPER)) {
        c->flags &= (uint8_t)~TF_UPPER;
        TlsConn *t = conn_of(c);
        if (t) conn_free(t);
    }
    tcp_close_port(vm, p);
}

/* Write{Data} and Shutdown{}. */
static void t_command(Vm *vm, const Actor *from, Port *p, term msg) {
    TcpPcb *c = tcp_pcb_of(p);
    const uint32_t *o = obj_of(vm, from, msg);
    TlsConn *t = tc;
    if (!c || !t || t->c != c) return;          /* the connection has ended */
    if (!o || hdr_kind(o[0]) != K_STRUCT) {
        net_s.st.tx_bad++;
        return;
    }
    switch (o[1]) {
    case TYPE_NET_WRITE:
        if (t->flags & TS_SHUT) break;
        if (tcp_write_cmd(vm, from, p, c, o)) t->flags |= TS_KICK;   /* else it may have ended */
        return;
    case TYPE_NET_SHUTDOWN:
        if (hdr_words(o[0]) != 2u) break;
        t->flags |= TS_SHUT | TS_KICK;
        return;
    default:
        break;
    }
    net_s.st.tx_bad++;
}

const PortDriver net_tls_driver = {
    "tls", TYPE_NET_DIALTLS, TYPE_NET_DATA, NULL, NULL, t_close, tcp_port_can_event, NULL, t_command, t_open
};
