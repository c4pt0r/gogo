/* Wi-Fi provisioning messages (provmsg.h). */
#include "provmsg.h"

#include <stdio.h>
#include <string.h>

/* ---- decoding ---- */

typedef struct {
    const uint8_t *p, *end;
} Rd;

static int rd_varint(Rd *r, uint64_t *v) {
    uint64_t x = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return -1;
        uint8_t b = *r->p++;
        x |= (uint64_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *v = x;
            return 0;
        }
    }
    return -1;
}

/* The next field: its number, wire type and, for a length-delimited one,
 * the bytes (*sub); for a varint, its value. -1 at a malformed field. */
static int rd_field(Rd *r, uint32_t *num, uint32_t *wt, uint64_t *val, Rd *sub) {
    uint64_t key;
    if (rd_varint(r, &key) != 0 || key >> 3 == 0 || key >> 3 > 0x1FFFFFFFu) return -1;
    *num = (uint32_t)(key >> 3);
    *wt = (uint32_t)(key & 7u);
    switch (*wt) {
    case 0: return rd_varint(r, val);
    case 1:
        if (r->end - r->p < 8) return -1;
        r->p += 8;
        return 0;
    case 2: {
        uint64_t len;
        if (rd_varint(r, &len) != 0 || len > (uint64_t)(r->end - r->p)) return -1;
        sub->p = r->p;
        sub->end = r->p + len;
        r->p += len;
        return 0;
    }
    case 5:
        if (r->end - r->p < 4) return -1;
        r->p += 4;
        return 0;
    default: return -1;   /* groups (3, 4) are not proto3 */
    }
}

static int decode_set(Rd m, ProvRequest *q) {
    while (m.p < m.end) {
        uint32_t num, wt;
        uint64_t v;
        Rd s;
        if (rd_field(&m, &num, &wt, &v, &s) != 0) return PROV_DEC_MALFORMED;
        if (wt != 2 || (num != 1 && num != 2)) continue;   /* bssid, channel: unused */
        size_t len = (size_t)(s.end - s.p);
        if (num == 1) {
            if (len == 0 || len > PROV_SSID_MAX) {
                q->bad = 1;
                continue;
            }
            memcpy(q->ssid, s.p, len);
            q->ssid_len = (uint8_t)len;
        } else {
            if (len > PROV_PASS_MAX) {
                q->bad = 1;
                continue;
            }
            memcpy(q->pass, s.p, len);
            q->pass_len = (uint8_t)len;
        }
    }
    if (q->ssid_len == 0) q->bad = 1;
    return PROV_DEC_OK;
}

int prov_decode(const uint8_t *in, size_t n, ProvRequest *q) {
    memset(q, 0, sizeof *q);
    Rd r = { in, in + n }, set = { NULL, NULL };
    int have_set = 0;
    while (r.p < r.end) {
        uint32_t num, wt;
        uint64_t v;
        Rd s;
        if (rd_field(&r, &num, &wt, &v, &s) != 0) return PROV_DEC_MALFORMED;
        if (num == 1 && wt == 0) q->msg = v > 255u ? 255 : (int)v;
        else if (num == 12 && wt == 2) {
            set = s;
            have_set = 1;
        }
    }
    switch (q->msg) {
    case PROV_MSG_GET_STATUS:
    case PROV_MSG_APPLY_CONFIG: return PROV_DEC_OK;
    case PROV_MSG_SET_CONFIG:
        if (!have_set) {
            q->bad = 1;
            return PROV_DEC_OK;
        }
        return decode_set(set, q);
    default: return PROV_DEC_UNKNOWN;
    }
}

/* ---- encoding ---- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int over;
} Wr;

static void wr_byte(Wr *w, uint8_t b) {
    if (w->n < w->cap) w->p[w->n] = b;
    else w->over = 1;
    w->n++;
}

static void wr_varint(Wr *w, uint64_t v) {
    while (v >= 0x80u) {
        wr_byte(w, (uint8_t)(v | 0x80u));
        v >>= 7;
    }
    wr_byte(w, (uint8_t)v);
}

static size_t varint_len(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

static void wr_key(Wr *w, uint32_t num, uint32_t wt) { wr_varint(w, (uint64_t)num << 3 | wt); }

/* A varint field; proto3 leaves a 0 out unless forced (a oneof member). */
static void wr_uint(Wr *w, uint32_t num, uint64_t v, int force) {
    if (!v && !force) return;
    wr_key(w, num, 0);
    wr_varint(w, v);
}

static size_t uint_len(uint32_t num, uint64_t v, int force) {
    if (!v && !force) return 0;
    return varint_len((uint64_t)num << 3) + varint_len(v);
}

static void wr_bytes(Wr *w, uint32_t num, const uint8_t *b, size_t len) {
    wr_key(w, num, 2);
    wr_varint(w, len);
    for (size_t i = 0; i < len; i++) wr_byte(w, b[i]);
}

static size_t bytes_len(uint32_t num, size_t len) { return varint_len((uint64_t)num << 3) + varint_len(len) + len; }

static size_t wr_done(Wr *w) { return w->over ? 0 : w->n; }

size_t prov_encode_status(uint8_t *out, size_t cap, int sta_state, int fail_reason, const uint8_t *ssid,
                          size_t ssid_len) {
    /* the inner message's length first: RespGetWifiStatus, and for
     * Connected its WifiConnectedState (ip4_addr "" left out, ssid) */
    size_t conn = ssid_len ? bytes_len(3, ssid_len) : 0;
    size_t inner = uint_len(2, (uint64_t)sta_state, 0);
    if (sta_state == PROV_STA_FAILED) inner += uint_len(10, (uint64_t)fail_reason, 1);
    else if (sta_state == PROV_STA_CONNECTED) inner += bytes_len(11, conn);
    Wr w = { out, 0, cap, 0 };
    wr_uint(&w, 1, 1, 0);   /* TypeRespGetWifiStatus */
    wr_key(&w, 11, 2);
    wr_varint(&w, inner);
    wr_uint(&w, 2, (uint64_t)sta_state, 0);
    if (sta_state == PROV_STA_FAILED) wr_uint(&w, 10, (uint64_t)fail_reason, 1);
    else if (sta_state == PROV_STA_CONNECTED) {
        wr_key(&w, 11, 2);
        wr_varint(&w, conn);
        if (ssid_len) wr_bytes(&w, 3, ssid, ssid_len);
    }
    return wr_done(&w);
}

size_t prov_encode_resp(uint8_t *out, size_t cap, int req, int status) {
    if (req != PROV_MSG_SET_CONFIG && req != PROV_MSG_APPLY_CONFIG) return 0;
    uint32_t field = req == PROV_MSG_SET_CONFIG ? 13u : 15u;
    Wr w = { out, 0, cap, 0 };
    wr_uint(&w, 1, (uint64_t)req + 1u, 0);
    wr_key(&w, field, 2);
    wr_varint(&w, uint_len(1, (uint64_t)status, 0));
    wr_uint(&w, 1, (uint64_t)status, 0);
    return wr_done(&w);
}

size_t prov_version_json(char *out, size_t cap, int sec_ver, int sec_patch_ver) {
    int n = snprintf(out, cap, "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":%d,\"sec_patch_ver\":%d,\"cap\":[\"wifi_prov\"]}}",
                     sec_ver, sec_patch_ver);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

void prov_pin(char out[7], uint32_t (*rnd)(void)) {
    /* 4294000000 = 4294 * 10^6: values at or above it would favour the
     * low PINs */
    uint32_t v;
    do v = rnd();
    while (v >= 4294000000u);
    v %= 1000000u;
    for (int i = 5; i >= 0; i--) {
        out[i] = (char)('0' + v % 10u);
        v /= 10u;
    }
    out[6] = 0;
}
