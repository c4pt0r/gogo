/* Trust anchors from an esp_crt_bundle-format bundle (anchor.h). */
#include "anchor.h"
#include <string.h>
#include "mbedtls/x509_crt.h"

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool tls_bundle_has(const uint8_t *b, size_t size, const uint8_t *subject, size_t subject_len,
                    const uint8_t *key, size_t key_len) {
    if (!b || size < 4u) return false;
    uint32_t n = le32(b) / 4u;
    for (uint32_t i = 0; i < n; i++) {
        if ((size_t)i * 4u + 4u > size) return false;
        uint32_t off = le32(b + (size_t)i * 4u);
        if (off > size || size - off < 4u) return false;
        const uint8_t *c = b + off;
        size_t nl = (size_t)c[0] | (size_t)c[1] << 8, kl = (size_t)c[2] | (size_t)c[3] << 8;
        if (nl + kl > size - off - 4u) return false;
        if (nl == subject_len && kl == key_len && memcmp(c + 4, subject, nl) == 0 && memcmp(c + 4 + nl, key, kl) == 0)
            return true;
    }
    return false;
}

bool tls_anchor_trust(const uint8_t *bundle, size_t size, const struct mbedtls_x509_crt *crt, uint32_t *flags) {
    if ((*flags & ~(uint32_t)MBEDTLS_X509_BADCERT_BAD_MD) != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return false;
    if (!tls_bundle_has(bundle, size, crt->subject_raw.p, crt->subject_raw.len, crt->pk_raw.p, crt->pk_raw.len))
        return false;
    *flags = 0;
    return true;
}
