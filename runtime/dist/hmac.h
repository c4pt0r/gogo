/* SHA-256 (FIPS 180-4) and HMAC-SHA-256 (RFC 2104), small and dependency
 * free: the node port MACs every frame of a link with its session key.
 * Not constant-time beyond what the algorithm is; no secret-dependent
 * branches or table lookups. */
#ifndef GOGO_DIST_HMAC_H
#define GOGO_DIST_HMAC_H

#include <stdint.h>
#include <stddef.h>

typedef struct Sha256 {
    uint32_t h[8];
    uint64_t len;            /* bytes hashed so far */
    uint8_t buf[64];
    uint32_t n;              /* bytes in buf */
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const uint8_t *p, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);

/* An HMAC key's two precomputed states (after the inner and the outer
 * padded key block): a MAC costs only the message's blocks and two. */
typedef struct HmacKey {
    Sha256 in, out;
} HmacKey;

void hmac_key(HmacKey *k, const uint8_t *key, size_t n);   /* n <= 64 */
/* HMAC of the concatenation a || b (either may be empty). */
void hmac_mac(const HmacKey *k, const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]);

#endif
