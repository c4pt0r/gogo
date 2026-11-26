/* Known answers for runtime/dist/hmac.c: SHA-256 (FIPS 180-4 examples)
 * and HMAC-SHA-256 (RFC 4231 cases 1-4), each whole and fed in pieces.
 * Run by make test (test-dist-c). */
#include <stdio.h>
#include <string.h>
#include "hmac.h"

static int fails;

static void hex(const uint8_t *d, char out[65]) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void expect(const char *name, const uint8_t got[32], const char *want) {
    char h[65];
    hex(got, h);
    if (strcmp(h, want) != 0) {
        printf("  FAIL %s\n    got  %s\n    want %s\n", name, h, want);
        fails++;
    }
}

static void sha(const char *name, const uint8_t *m, size_t n, size_t step, const char *want) {
    Sha256 s;
    uint8_t out[32];
    sha256_init(&s);
    for (size_t i = 0; i < n; i += step) sha256_update(&s, m + i, n - i < step ? n - i : step);
    sha256_final(&s, out);
    expect(name, out, want);
}

static void mac(const char *name, const uint8_t *key, size_t kn, const uint8_t *m, size_t n, const char *want) {
    HmacKey k;
    uint8_t out[32];
    hmac_key(&k, key, kn);
    hmac_mac(&k, m, n, NULL, 0, out);
    expect(name, out, want);
    for (size_t split = 0; split <= n; split += n / 3u + 1u) {   /* a || b, as frames are MACed */
        hmac_mac(&k, m, split, m + split, n - split, out);
        expect(name, out, want);
    }
}

int main(void) {
    static uint8_t million[1000000];
    const uint8_t *abc = (const uint8_t *)"abc";
    const char *l448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha("empty", NULL, 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha("abc", abc, 3, 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha("abc bytewise", abc, 3, 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha("448 bits", (const uint8_t *)l448, strlen(l448), 7,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    memset(million, 'a', sizeof million);
    sha("a million a", million, sizeof million, 4096, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    uint8_t k1[20];
    memset(k1, 0x0b, sizeof k1);
    mac("rfc4231 1", k1, sizeof k1, (const uint8_t *)"Hi There", 8,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    mac("rfc4231 2", (const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t k3[20], m3[50];
    memset(k3, 0xaa, sizeof k3);
    memset(m3, 0xdd, sizeof m3);
    mac("rfc4231 3", k3, sizeof k3, m3, sizeof m3,
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    uint8_t k4[25], m4[50];
    for (int i = 0; i < 25; i++) k4[i] = (uint8_t)(i + 1);
    memset(m4, 0xcd, sizeof m4);
    mac("rfc4231 4", k4, sizeof k4, m4, sizeof m4,
        "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    printf("hmac: %s\n", fails ? "FAILED" : "known answers ok");
    return fails != 0;
}
