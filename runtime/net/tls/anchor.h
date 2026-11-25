/* Trust anchors from a certificate bundle in ESP-IDF's esp_crt_bundle format
 * (shared C: the ESP firmware uses it on IDF's bundle in flash, the unix
 * VM on a bundle file for the tests, $GOGO_TLS_BUNDLE_FILE).
 *
 * The bundle (IDF v6.1.1 components/mbedtls/esp_crt_bundle/esp_crt_bundle.c,
 * "Format of certificate bundle", written by its gen_crt_bundle.py): n u32
 * little-endian offsets, the first one being the table's size (so n =
 * first / 4), then per certificate a u16 LE subject length, a u16 LE key
 * length, the subject (DER Name, as x509 subject_raw) and the public key
 * (DER SubjectPublicKeyInfo, as x509 pk_raw). Sorted by subject; this code
 * does not rely on the order. An IDF upgrade that changes the format must
 * change this file (tests/net/tls_test.go writes the same format).
 *
 * The rule: the platform gives mbedTLS a dummy CA chain, so the
 * peer's chain is checked link by link (restartably, sliced) up to the last
 * certificate the peer sent, which then carries NOT_TRUSTED. That
 * certificate is trusted if it IS a bundle root: the same subject and the
 * same public key (a trust anchor is a name and a key; the chain below was
 * verified up to that key). Its own signature is not checked (a cross-signed
 * root is signed by another CA, often one not in the bundle). Any other flag
 * (expired, not yet valid, wrong name, bad key) still fails it; BAD_MD is
 * ignored, as IDF's own callback does. */
#ifndef GOGO_TLS_ANCHOR_H
#define GOGO_TLS_ANCHOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mbedtls_x509_crt;

/* True if bundle[0..size) holds an entry with exactly this subject and key.
 * Every offset and length is bounds-checked: a truncated or corrupt bundle
 * matches nothing it does not fully contain. */
bool tls_bundle_has(const uint8_t *bundle, size_t size, const uint8_t *subject, size_t subject_len,
                    const uint8_t *key, size_t key_len);

/* The verify-callback decision for crt with flags (mbedTLS's, for a
 * certificate of the built chain): true, and *flags cleared, if the rule
 * above trusts it; false (flags untouched) otherwise. */
bool tls_anchor_trust(const uint8_t *bundle, size_t size, const struct mbedtls_x509_crt *crt, uint32_t *flags);

#endif
