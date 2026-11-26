/* The "tls" port: a TLS 1.2 client over the net stack's TCP (spec HTTPS §2),
 * in shared C on the scheduler thread, using mbedTLS (4.x, TF-PSA-Crypto).
 *
 * Port "tls", cfg net.DialTLS{IP, Port, TimeoutMs, Host}: the same messages
 * and flow as "tcp" (Connected, port.Out / Done / Chunk / Closed,
 * Shutdown), the bytes being the TLS plaintext.
 *
 * Design: a mode of the tcp driver, not a second stack. Open makes an
 * ordinary "tcp" connection (tcp_dial) and marks it TF_UPPER, so TCP hands
 * the stream to this layer (TcpUpper, runtime/net/net_int.h) instead of to the
 * owner: received segments are copied into a one-segment ciphertext buffer
 * (refused, so not ACKed, while it is full), and what mbedTLS sends is sent
 * by TCP straight from mbedTLS's own output buffer (no copy: the bio send
 * callback reports bytes as sent only once TCP has them acknowledged, so
 * they stay put for retransmission). Every control event, held-event
 * retry, Out rule (the port's StreamOut: two in flight) and timer is TCP's own; the
 * owner sees exactly the "tcp" port's semantics.
 *
 *  - The handshake runs from the stack's drain, one mbedtls_ssl_handshake_step
 *    per drain; ECDSA signature checks (ServerKeyExchange and the chain) are
 *    sliced by PSA interruptible operations (psa_interruptible_set_max_ops
 *    GOGO_TLS_MAX_OPS), so a step gives up (MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS)
 *    and resumes on the next drain, which runs at once (the layer's
 *    pending() stays true, and the scheduler does not sleep while a source
 *    has work: vm_step).
 *    Connected follows the handshake.
 *    TimeoutMs bounds connect plus handshake (GOGO_TLS_HANDSHAKE_MS when 0).
 *    NOT sliced (docs/known-issues.md): the ECDHE key
 *    generation plus agreement (one ClientKeyExchange step: mbedTLS 4.x does
 *    it with plain PSA calls; ~1 ms on the host, tens of ms expected on an
 *    ESP32-S3 with software P-256) and RSA signature checks (an RSA chain;
 *    cheap, e = 65537). Measured on the StickS3: the ECDHE step is 204 ms
 *    with X25519, above the 50 ms cap,
 *    accepted as an exception; the fix is mbedTLS's
 *    interruptible key generation and agreement (psa_*_iop) WITH P-256 as
 *    the preferred group (X25519's multiplication is not restartable).
 *  - TLS 1.2 only: mbedTLS 4.x's TLS 1.3 key exchange and
 *    CertificateVerify cannot be sliced either. A server that requires 1.3
 *    gives Closed{TLS}.
 *  - Verification is always on: the board's trust anchors
 *    (TlsHw.verify_setup), the certificate validity against the wall
 *    clock (vm_unix_time_ms) and the name against Host (SNI too).
 *    Dial before the clock is set gives Closed{NoTime} (no connection is
 *    made); a certificate that fails gives Closed{BadCert}; any other TLS
 *    failure (an alert, a malformed record, memory) Closed{TLS}; the peer
 *    closing during the handshake Closed{Reset}.
 *  - Chunk: one per at most TCP_MSS plaintext bytes, delivered only when
 *    the owner has room (as "tcp": a Chunk leaves headroom for the control
 *    events, a blocked delivery is retried every 50 ms, the owner is never
 *    killed). close_notify, or the peer's FIN at a record boundary (as Go's
 *    crypto/tls treats it), gives Closed{Normal}. TRUNCATION: a FIN at a
 *    record boundary is accepted, so an on-path attacker can cut a stream
 *    short there undetected at this layer; a protocol on top must frame its
 *    data (gogo/http: Content-Length or the chunked terminator, a short
 *    body is an error; never read-until-close over TLS).
 *  - Closed: at most one Normal, then at most one terminal Closed (any
 *    other reason; a graceful close ends with Normal alone) per connection, from this layer (its failures) or from TCP
 *    (reset, timeout, no route), never both (TcpUpper.closed); a terminal
 *    one replaces a Normal still held for a full mailbox. An Out still
 *    in flight when the connection ends gets Done{-1} (as on "tcp").
 *  - Out: up to MBEDTLS_SSL_OUT_CONTENT_LEN bytes per record; Done once
 *    all of an Out's records are acknowledged by the peer's TCP. No Done
 *    goes out while Connected is held for a full mailbox.
 *  - Shutdown: close_notify after the pending writes, then the TCP FIN.
 *    Closing the port (or the owner's exit) before that went out resets the
 *    connection; after it, TCP finishes the close as an orphan.
 *  - Memory: the TLS state (mbedTLS context, record buffers 16 KB in and
 *    4 KB out, the ciphertext buffer) comes from TlsHw.alloc and is
 *    charged to the owner: about 23 KB (TLS_CHARGE_WORDS) on top of the TCP
 *    control block, plus room for one Chunk beside it. An owner whose limit
 *    cannot fit that is refused at open (ok=false): with the MCU default
 *    of 16 KB per actor, the owner must be spawned with a larger MaxMemory
 *    (about 23 KB + one Chunk + its own heap). The measured peak per
 *    connection is about 34 KB: the other ~11 KB are handshake temporaries
 *    (the peer's chain, ECDHE keys, PSA operations), freed when the
 *    handshake ends and not charged (they are bounded: one connection at
 *    a time; the platform sizes its TLS memory for the peak plus its trust
 *    store). At most GOGO_TLS_MAX (1) connection per net stack (each Vm's
 *    NetState has its own): a second open on the same stack is refused. A
 *    connection's memory is freed when it ends, the port closes or its
 *    owner exits; what tls_init sets up (PSA's key store) stays.
 *  - Randomness: only TlsHw.random (never the VM's vm_random); a board
 *    without a secure source has no TLS (open is refused).
 *  - Hooks: the board's TlsHw (VmBoard.tls, host.h; below). A board
 *    without one has no TLS (open is refused).
 *  - State: the connection and the counters live in the stack's NetState
 *    (TlsState below; per Vm). The one thing shared by the whole process is
 *    mbedTLS's own platform layer (allocator, time, RNG, PSA): its hooks
 *    take no context, so they are bound once, to the first TlsHw given to
 *    tls_init. ALL VMs IN ONE PROCESS MUST SHARE ONE TlsHw: tls_init with a
 *    different one fails, and that Vm's opens are refused (ok=false; logged
 *    once per process). Where gogo installs the hooks
 *    (GOGO_TLS_MBEDTLS_HOOKS 1, the host), mbedTLS's time hooks read the
 *    clocks of the Vm this thread is driving a connection for (a
 *    thread-local pointer, set around each mbedTLS call), so VMs on
 *    different threads, each on its owner thread, each verify against their
 *    own wall clock. The host's mbedTLS is built thread-safe
 *    (MBEDTLS_THREADING_C, runtime/net/tls/config). tls_init is serialized
 *    (a lock), so Vms on several threads may open tls first at the same
 *    time. The port sets psa_interruptible_set_max_ops once, in tls_init;
 *    hosts must not call it themselves (it is process-wide in mbedTLS, and
 *    the ThreadSanitizer run relies on every store being that one value).
 */
#ifndef GOGO_TLS_H
#define GOGO_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "port.h"

struct NetState;
struct TlsConn;

#ifndef GOGO_TLS_MAX
#define GOGO_TLS_MAX 1u
#endif
/* ECC basic operations per handshake slice (psa_interruptible_set_max_ops). */
#ifndef GOGO_TLS_MAX_OPS
#define GOGO_TLS_MAX_OPS 200u
#endif
/* The handshake's limit when Dial's TimeoutMs is 0. */
#ifndef GOGO_TLS_HANDSHAKE_MS
#define GOGO_TLS_HANDSHAKE_MS 30000u
#endif
/* 1: tls_init installs mbedTLS's process-global allocator (TlsHw.alloc /
 * free) and time source (the wall clock of the open connection's Vm), and
 * tls.c defines mbedTLS's port functions mbedtls_psa_external_get_random
 * (TlsHw.random) and mbedtls_ms_time (the open connection's vm_now). 0: the
 * platform's mbedTLS port provides all of them (ESP-IDF, whose mbedTLS
 * other components share: its allocator must be the TlsHw's
 * (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC), its mbedtls_time must read the gogo
 * wall clock, and its RNG must be cryptographic; TlsHw.random then only
 * guards open). */
#ifndef GOGO_TLS_MBEDTLS_HOOKS
#define GOGO_TLS_MBEDTLS_HOOKS 1
#endif

extern const PortDriver net_tls_driver;

/* The board's TLS hooks (VmBoard.tls, host.h), a static const table.
 * Every field is required but clock_us. They run on the thread of the Vm
 * whose connection needs them, and every Vm of a process shares the one
 * TlsHw (State above): with Vms on several threads, any hook may be called
 * from several threads at once and must be thread-safe (alloc and free
 * also serve mbedTLS's process-wide allocator).
 *
 * alloc: n zeroed bytes for mbedTLS and the connection state, or NULL.
 * PSRAM on boards that have it; a connection peaks at about 34 KB (record
 * buffers 16 KB in + 4 KB out, plus the handshake's temporaries), plus
 * whatever the board's trust store needs. free: frees what alloc returned
 * (NULL ignored). Unix: calloc / free, counted.
 *
 * verify_setup: attaches the trust anchors to conf (a CA chain with
 * mbedtls_ssl_conf_ca_chain, or a verify callback such as ESP-IDF's
 * esp_crt_bundle_attach) for one connection; *state is the board's, handed
 * back to verify_free when the connection ends (only after a 0 return).
 * 0 = ok; anything else refuses the open. Memory it needs should come from
 * alloc. Unix: the PEM file $GOGO_TLS_CA_FILE, else the system bundle
 * (/etc/ssl/cert.pem, then /etc/ssl/certs/ca-certificates.crt).
 *
 * clock_us: a free-running microsecond clock in real time, only for
 * TlsStats (slice timing). NULL: not measured (0).
 *
 * random: fills out[0..n) from a cryptographically secure source (every
 * TLS secret comes from it: the ECDHE key, randoms, nonces) and returns 0,
 * or returns nonzero if it cannot (then the open is refused, or the
 * handshake fails with Closed{TLS}). Never a fallback to a weak generator.
 * Unix: getentropy. ESP: esp_fill_random (a CSPRNG while the radio is on,
 * which it is whenever TLS runs). */
struct mbedtls_ssl_config;
typedef struct TlsHw {
    void *(*alloc)(size_t n);
    void (*free)(void *p);
    int (*verify_setup)(struct mbedtls_ssl_config *conf, void **state);
    void (*verify_free)(void *state);
    uint64_t (*clock_us)(void);
    int (*random)(uint8_t *out, size_t n);
} TlsHw;

/* The process-global mbedTLS / PSA state, set up once (psa_crypto_init and
 * the hooks above); never torn down by the port. mbedTLS's hooks have no
 * context, so the TlsHw of the first successful call serves them for the
 * whole process. The first open calls it with its Vm's board->tls; a board
 * may call it at boot. Idempotent for the same hw; false if PSA failed or
 * hw is not the TlsHw already bound (every Vm of a process must use the
 * same one). */
bool tls_init(const TlsHw *hw);

/* Counters for tests and measurements (since start). A slice is one
 * mbedTLS call made from the drain; times are TlsHw.clock_us. */
typedef struct TlsStats {
    uint32_t opens;         /* connections set up */
    uint32_t handshakes;    /* completed */
    uint32_t steps;         /* handshake slices (mbedtls_ssl_handshake_step calls) */
    uint32_t restarts;      /* slices that stopped early (CRYPTO_IN_PROGRESS): sliced ECC */
    uint32_t max_step_us;   /* longest handshake slice */
    int32_t max_step_state; /* the handshake state that slice started in */
    uint32_t max_io_us;     /* longest read or write slice */
    uint32_t live;          /* connections now on this stack (0 or 1) */
    uint32_t last_hs_us;    /* the last completed handshake, first slice to done (network waits included) */
    int32_t last_error;     /* the mbedTLS error of the last failed connection (0: none yet) */
} TlsStats;

/* One net stack's TLS state (NetState.tls, runtime/net/net_int.h): its
 * connection (GOGO_TLS_MAX of them) and counters. Cleared by net_install. */
typedef struct TlsState {
    struct TlsConn *conn;   /* the open connection, or NULL */
    TlsStats st;
} TlsState;

/* Stack ns's counters. */
const TlsStats *tls_stats(const struct NetState *ns);

#endif
