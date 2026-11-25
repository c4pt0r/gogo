/* Unix hooks of the "tls" port (runtime/net/tls/tls.h), linked into gogovm when it is
 * built with TLS (Makefile: GOGO_TLS):
 *  - platform_tls_alloc / platform_tls_free: calloc / free with a size
 *    header, counting the bytes outstanding and their peak
 *    (platform_unix_tls_heap: tests check that every connection gives all
 *    of it back);
 *  - platform_tls_verify_setup: trust anchors from the PEM file
 *    $GOGO_TLS_CA_FILE, else the system bundle; or, with
 *    $GOGO_TLS_BUNDLE_FILE, from an esp_crt_bundle-format bundle with the
 *    ESP firmware's anchor rule (runtime/net/tls/anchor.h; tests only);
 *  - platform_tls_clock_us: CLOCK_MONOTONIC (real time, also under
 *    --sim-clock), for the slice timing;
 *  - platform_tls_random: getentropy, failing on error (the test switch
 *    $GOGO_TLS_TEST_NO_RANDOM makes it fail always, as the weak default).
 *
 * The CA file is read and parsed again for every connection: with the
 * system bundle (about 150 certificates) that is an unsliced cost in open
 * and some 200 KB of TLS heap on the host. Host only (ESP attaches its
 * flash bundle); tests use a one-certificate file. */
#define _POSIX_C_SOURCE 200809L
#include "tls.h"
#include "tls_unix.h"
#include "anchor.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>      /* getentropy: declared here on macOS and glibc (unistd.h hides it under _POSIX_C_SOURCE) */

#define HDR 16u          /* keeps the payload 16-byte aligned */

static size_t heap_now, heap_peak;

void *platform_tls_alloc(size_t n) {
    if (n > SIZE_MAX - HDR) return NULL;
    unsigned char *p = calloc(1, n + HDR);
    if (!p) return NULL;
    memcpy(p, &n, sizeof n);
    heap_now += n;
    if (heap_now > heap_peak) heap_peak = heap_now;
    return p + HDR;
}

void platform_tls_free(void *q) {
    if (!q) return;
    unsigned char *p = (unsigned char *)q - HDR;
    size_t n;
    memcpy(&n, p, sizeof n);
    heap_now -= n;
    free(p);
}

void platform_unix_tls_heap(size_t *now, size_t *peak) {
    *now = heap_now;
    *peak = heap_peak;
}

int platform_tls_random(uint8_t *out, size_t n) {
    if (getenv("GOGO_TLS_TEST_NO_RANDOM")) return -1;
    while (n) {
        size_t k = n < 256u ? n : 256u;   /* getentropy's limit */
        if (getentropy(out, k) != 0) return -1;
        out += k;
        n -= k;
    }
    return 0;
}

uint64_t platform_tls_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* The whole file plus a NUL (mbedtls_x509_crt_parse wants PEM terminated),
 * or NULL. */
static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096u + 1u > cap) {
            cap = cap ? cap * 2u : 65536u;
            unsigned char *b = realloc(buf, cap);
            if (!b) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = b;
        }
        size_t k = fread(buf + n, 1, 4096u, f);
        n += k;
        if (k < 4096u) break;
    }
    fclose(f);
    buf[n] = 0;
    *len = n + 1u;
    return buf;
}

/* $GOGO_TLS_BUNDLE_FILE: trust anchors from a bundle in ESP-IDF's
 * esp_crt_bundle format instead, with the ESP firmware's rule
 * (runtime/net/tls/anchor.h): a dummy CA chain, and the last certificate the peer sent
 * is trusted only if it is a bundle root (subject and key). The ESP
 * fallback (the top certificate signed by a bundle root's key, IDF's
 * esp_crt_verify_callback) is not here, so such a chain fails. For the
 * tests of that rule. state: the bundle, the size first. */
typedef struct Bundle {
    size_t size;
    uint8_t data[];
} Bundle;

static const mbedtls_x509_crt dummy_ca;   /* as IDF's s_dummy_crt: never a parent */
/* What platform_tls_verify_setup gave the open connection as its state (a
 * Bundle or a CA chain), for platform_tls_verify_free: one connection at a
 * time (GOGO_TLS_MAX 1). */
static int state_is_bundle;

static int bundle_verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)depth;
    const Bundle *b = ctx;
    (void)tls_anchor_trust(b->data, b->size, crt, flags);   /* else the flags fail it */
    return 0;
}

static int bundle_setup(struct mbedtls_ssl_config *conf, void **state, const char *path) {
    size_t len = 0;
    unsigned char *raw = read_file(path, &len);   /* len counts the added NUL */
    if (!raw || len < 1u) {
        free(raw);
        return -1;
    }
    Bundle *b = platform_tls_alloc(sizeof *b + len);
    if (!b) {
        free(raw);
        return -1;
    }
    b->size = len - 1u;
    memcpy(b->data, raw, len - 1u);
    free(raw);
    mbedtls_ssl_conf_ca_chain(conf, (mbedtls_x509_crt *)&dummy_ca, NULL);
    mbedtls_ssl_conf_verify(conf, bundle_verify, b);
    *state = b;
    return 0;
}

int platform_tls_verify_setup(struct mbedtls_ssl_config *conf, void **state) {
    static const char *const bundles[] = { "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt" };
    const char *bf = getenv("GOGO_TLS_BUNDLE_FILE");
    state_is_bundle = bf && *bf;
    if (state_is_bundle) return bundle_setup(conf, state, bf);
    const char *env = getenv("GOGO_TLS_CA_FILE");
    unsigned char *pem = NULL;
    size_t len = 0;
    if (env && *env) {
        pem = read_file(env, &len);
    } else {
        for (size_t i = 0; !pem && i < sizeof bundles / sizeof bundles[0]; i++) pem = read_file(bundles[i], &len);
    }
    if (!pem) return -1;
    mbedtls_x509_crt *ca = platform_tls_alloc(sizeof *ca);
    if (!ca) {
        free(pem);
        return -1;
    }
    mbedtls_x509_crt_init(ca);
    int r = mbedtls_x509_crt_parse(ca, pem, len);   /* > 0: some certificates skipped */
    free(pem);
    if (r < 0 || !ca->raw.p) {
        mbedtls_x509_crt_free(ca);
        platform_tls_free(ca);
        return -1;
    }
    mbedtls_ssl_conf_ca_chain(conf, ca, NULL);
    *state = ca;
    return 0;
}

void platform_tls_verify_free(void *state) {
    if (!state_is_bundle) mbedtls_x509_crt_free(state);
    platform_tls_free(state);
}
