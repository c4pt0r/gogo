/* The provisioning messages against bytes Python's protobuf produced from
 * network_provisioning's network_config.proto (esp_prov.py's encoder; the
 * credentials are dummies), plus malformed input and the PIN. */
#include "test.h"
#include "provmsg.h"
#include <string.h>

void test_host_arena_forget(void);
void test_host_arena_forget(void) {}   /* test_main.c calls it; no VM here */

#define SAME(buf, n, ...)                                                   \
    do {                                                                    \
        static const uint8_t want_[] = { __VA_ARGS__ };                     \
        CHECK_EQ((n), sizeof want_);                                        \
        CHECK((n) == sizeof want_ && memcmp((buf), want_, sizeof want_) == 0); \
    } while (0)

static const uint8_t set_msg[] = {
    0x08, 0x02, 0x62, 0x2a, 0x0a, 0x09, 0x67, 0x6f, 0x67, 0x6f, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x12,
    0x13, 0x6e, 0x6f, 0x74, 0x2d, 0x61, 0x2d, 0x72, 0x65, 0x61, 0x6c, 0x2d, 0x70, 0x61, 0x73, 0x73,
    0x77, 0x6f, 0x72, 0x64, 0x1a, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x20, 0x06,
};

TEST(prov_decode_set) {
    ProvRequest q;
    CHECK_EQ(prov_decode(set_msg, sizeof set_msg, &q), PROV_DEC_OK);
    CHECK_EQ(q.msg, PROV_MSG_SET_CONFIG);
    CHECK_EQ(q.bad, 0);
    CHECK_EQ(q.ssid_len, 9);
    CHECK(memcmp(q.ssid, "gogo-test", 9) == 0);
    CHECK_EQ(q.pass_len, 19);
    CHECK(memcmp(q.pass, "not-a-real-password", 19) == 0);
}

TEST(prov_decode_get_apply) {
    static const uint8_t get[] = { 0x52, 0x00 }, apply[] = { 0x08, 0x04, 0x72, 0x00 };
    ProvRequest q;
    CHECK_EQ(prov_decode(get, sizeof get, &q), PROV_DEC_OK);
    CHECK_EQ(q.msg, PROV_MSG_GET_STATUS);
    CHECK_EQ(prov_decode(apply, sizeof apply, &q), PROV_DEC_OK);
    CHECK_EQ(q.msg, PROV_MSG_APPLY_CONFIG);
    CHECK_EQ(prov_decode(NULL, 0, &q), PROV_DEC_OK);   /* all defaults: get status */
    CHECK_EQ(q.msg, PROV_MSG_GET_STATUS);
}

TEST(prov_decode_bad) {
    ProvRequest q;
    /* every truncation of the set message is malformed or a short field */
    for (size_t n = 1; n < sizeof set_msg; n++) CHECK(prov_decode(set_msg, n, &q) != PROV_DEC_OK || q.bad);
    static const uint8_t thread[] = { 0x08, 0x06 }, resp[] = { 0x08, 0x01 };
    CHECK_EQ(prov_decode(thread, sizeof thread, &q), PROV_DEC_UNKNOWN);
    CHECK_EQ(prov_decode(resp, sizeof resp, &q), PROV_DEC_UNKNOWN);
    static const uint8_t group[] = { 0x0b }, longvar[] = { 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
    CHECK_EQ(prov_decode(group, sizeof group, &q), PROV_DEC_MALFORMED);
    CHECK_EQ(prov_decode(longvar, sizeof longvar, &q), PROV_DEC_MALFORMED);
    static const uint8_t zero_key[] = { 0x00, 0x00 };
    CHECK_EQ(prov_decode(zero_key, sizeof zero_key, &q), PROV_DEC_MALFORMED);
    /* set without a payload, an empty SSID, a 33-byte SSID, a 64-byte passphrase */
    static const uint8_t nopay[] = { 0x08, 0x02 }, nossid[] = { 0x08, 0x02, 0x62, 0x02, 0x12, 0x00 };
    CHECK_EQ(prov_decode(nopay, sizeof nopay, &q), PROV_DEC_OK);
    CHECK(q.bad);
    CHECK_EQ(prov_decode(nossid, sizeof nossid, &q), PROV_DEC_OK);
    CHECK(q.bad);
    uint8_t m[80] = { 0x08, 0x02, 0x62, 35, 0x0a, 33 };
    memset(m + 6, 'a', 33);
    CHECK_EQ(prov_decode(m, 6 + 33, &q), PROV_DEC_OK);
    CHECK(q.bad);
    uint8_t p[80] = { 0x08, 0x02, 0x62, 69, 0x0a, 1, 'x', 0x12, 64 };
    memset(p + 9, 'b', 64);
    CHECK_EQ(prov_decode(p, 9 + 64, &q), PROV_DEC_OK);
    CHECK(q.bad);
    p[3] = 68;
    p[8] = 63;   /* 63 bytes: the longest passphrase */
    CHECK_EQ(prov_decode(p, 9 + 63, &q), PROV_DEC_OK);
    CHECK(!q.bad);
    CHECK_EQ(q.pass_len, 63);
    /* an open network: no passphrase field */
    static const uint8_t open[] = { 0x08, 0x02, 0x62, 0x03, 0x0a, 0x01, 'o' };
    CHECK_EQ(prov_decode(open, sizeof open, &q), PROV_DEC_OK);
    CHECK(!q.bad);
    CHECK_EQ(q.pass_len, 0);
}

TEST(prov_encode) {
    uint8_t b[64];
    size_t n = prov_encode_status(b, sizeof b, PROV_STA_CONNECTING, 0, NULL, 0);
    SAME(b, n, 0x08, 0x01, 0x5a, 0x02, 0x10, 0x01);
    n = prov_encode_status(b, sizeof b, PROV_STA_FAILED, PROV_FAIL_AUTH, NULL, 0);
    SAME(b, n, 0x08, 0x01, 0x5a, 0x04, 0x10, 0x03, 0x50, 0x00);
    n = prov_encode_status(b, sizeof b, PROV_STA_FAILED, PROV_FAIL_NOT_FOUND, NULL, 0);
    SAME(b, n, 0x08, 0x01, 0x5a, 0x04, 0x10, 0x03, 0x50, 0x01);
    n = prov_encode_status(b, sizeof b, PROV_STA_CONNECTED, 0, (const uint8_t *)"gogo-test", 9);
    SAME(b, n, 0x08, 0x01, 0x5a, 0x0d, 0x5a, 0x0b, 0x1a, 0x09, 0x67, 0x6f, 0x67, 0x6f, 0x2d, 0x74, 0x65, 0x73, 0x74);
    n = prov_encode_resp(b, sizeof b, PROV_MSG_SET_CONFIG, PROV_STATUS_OK);
    SAME(b, n, 0x08, 0x03, 0x6a, 0x00);
    n = prov_encode_resp(b, sizeof b, PROV_MSG_SET_CONFIG, PROV_STATUS_INVALID_ARG);
    SAME(b, n, 0x08, 0x03, 0x6a, 0x02, 0x08, 0x04);
    n = prov_encode_resp(b, sizeof b, PROV_MSG_APPLY_CONFIG, PROV_STATUS_OK);
    SAME(b, n, 0x08, 0x05, 0x7a, 0x00);
    CHECK_EQ(prov_encode_resp(b, sizeof b, PROV_MSG_GET_STATUS, 0), 0);
    CHECK_EQ(prov_encode_status(b, 16, PROV_STA_CONNECTED, 0, (const uint8_t *)"gogo-test", 9), 0);   /* 17 bytes */
}

TEST(prov_version) {
    char s[128];
    size_t n = prov_version_json(s, sizeof s, 2, 1);
    CHECK_EQ(n, strlen(s));
    CHECK(strcmp(s, "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":2,\"sec_patch_ver\":1,\"cap\":[\"wifi_prov\"]}}") == 0);
    CHECK_EQ(prov_version_json(s, 20, 2, 1), 0);
}

static uint32_t seq[4];
static int seq_i;
static uint32_t fake_rnd(void) { return seq[seq_i++]; }

TEST(prov_pin) {
    char pin[7];
    seq[0] = 4294000000u;   /* rejected */
    seq[1] = 0xFFFFFFFFu;   /* rejected */
    seq[2] = 1234567u;
    seq_i = 0;
    prov_pin(pin, fake_rnd);
    CHECK(strcmp(pin, "234567") == 0);
    CHECK_EQ(seq_i, 3);
    seq[0] = 42;
    seq_i = 0;
    prov_pin(pin, fake_rnd);
    CHECK(strcmp(pin, "000042") == 0);
}
