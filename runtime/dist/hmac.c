/* SHA-256 and HMAC-SHA-256 (hmac.h). */
#include "hmac.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], v[8];
    for (uint32_t i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (uint32_t i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K[i] + w[i];
        uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (uint32_t i = 0; i < 8; i++) h[i] += v[i];
}

void sha256_init(Sha256 *s) {
    static const uint32_t h0[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(s->h, h0, sizeof s->h);
    s->len = 0;
    s->n = 0;
}

void sha256_update(Sha256 *s, const uint8_t *p, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64u - s->n;
        if (k > n) k = n;
        memcpy(s->buf + s->n, p, k);
        s->n += (uint32_t)k;
        p += k;
        n -= k;
        if (s->n == 64u) {
            block(s->h, s->buf);
            s->n = 0;
        }
    }
}

void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8u;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56u) sha256_update(s, &pad, 1);
    uint8_t l[8];
    for (uint32_t i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56u - 8u * i));
    sha256_update(s, l, 8);
    for (uint32_t i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void hmac_key(HmacKey *k, const uint8_t *key, size_t n) {
    uint8_t ip[64], op[64];
    for (uint32_t i = 0; i < 64; i++) {
        uint8_t b = i < n ? key[i] : 0;
        ip[i] = b ^ 0x36u;
        op[i] = b ^ 0x5cu;
    }
    sha256_init(&k->in);
    sha256_update(&k->in, ip, 64);
    sha256_init(&k->out);
    sha256_update(&k->out, op, 64);
}

void hmac_mac(const HmacKey *k, const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]) {
    Sha256 s = k->in;
    uint8_t inner[32];
    sha256_update(&s, a, an);
    sha256_update(&s, b, bn);
    sha256_final(&s, inner);
    s = k->out;
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}
