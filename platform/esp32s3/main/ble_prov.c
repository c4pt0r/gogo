/* Wi-Fi provisioning over BLE (ble_prov.h; CONFIG_GOGO_BLE_PROV, which
 * needs GOGO_NET and GOGO_BLE: the NimBLE host of ble_host.c).
 *
 * Entry: ble_prov_boot reads the board's setup key once, early at boot
 * (StickS3: button A, KEY1 on G11, active low; Cardputer: the W key, read
 * from the TCA8418 as a plain GPIO pair so a key held across a reset is
 * seen). Not held: nothing else happens, the radio is never touched.
 *
 * Session (at most SESSION_S seconds):
 *  - a one-time six-digit PIN from the hardware RNG; the SRP6a salt and
 *    verifier for user "wifiprov" and that PIN are computed on the board
 *    (protocomm's esp_srp_gen_salt_verifier), and the PIN is shown on the
 *    screen and the serial console, with a countdown;
 *  - protocomm with security 2 (SRP6a + AES-GCM) on "prov-session", the
 *    "proto-ver" document and the "prov-config" endpoint (runtime/prov:
 *    get status, set config, apply), over a GATT service of our own on the
 *    shared NimBLE host (ble_host_acquire): the service, characteristic
 *    UUIDs and user descriptions of ESP-IDF's wifi_provisioning BLE scheme,
 *    advertised as PROV_gogo_XXXX, so Espressif's phone apps and esp_prov.py
 *    work unchanged;
 *  - apply: the station joins the network with the received settings
 *    (the driver only, as wifi.c: up to CONNECT_TRIES attempts); the phone
 *    polls the status and sees Connected, or ConnectionFailed with the
 *    reason (and may send other settings);
 *  - success (the phone saw Connected): the settings are written to gogo's
 *    config store, wifi.password then wifi.ssid, as `gogo setup` does
 *    (never to the driver's own NVS: ESP_WIFI_NVS_ENABLED=n), and the board
 *    restarts; the next boot joins with them. Timeout, too many refused
 *    sessions (a wrong PIN): nothing is written, the board restarts.
 * Always a restart at the end: the radio memory and the Wi-Fi driver are
 * left exactly as a normal boot sets them up.
 *
 * CONFIG_GOGO_PROV_DRYRUN (test only): nothing is written and the
 * station does not join; apply reports Connected at once and the board
 * prints the SSID and the password's length. CONFIG_GOGO_PROV_TEST_ENTER
 * (test only, needs DRYRUN): the session starts without the key after a
 * power-on or flash (not after its own restart).
 *
 * The password is never printed, and it is wiped from RAM after use. */
#include "sdkconfig.h"
#include "ble_prov.h"
#include "ble_host.h"
#include "board.h"
#include "platform_esp.h"
#include "provmsg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_srp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "protocomm.h"
#include "protocomm_security2.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#if defined(GOGO_BOARD_CARDPUTER) && CONFIG_GOGO_KBD
#include "driver/i2c_master.h"
#endif

#define SESSION_S      120
#define COUNTDOWN_S    15      /* the PIN line is repeated this often */
#define USERNAME       "wifiprov"   /* the Espressif apps' security 2 user */
#define SALT_LEN       16
#define CONNECT_TRIES  3
#define MAX_REFUSED    5       /* refused sessions (wrong PIN) before closing */
#define REPORT_LINGER_MS 1500  /* after the phone saw Connected */
#define UNREPORTED_MS  10000   /* connected, but the phone never asked */
#define IN_MAX         1024    /* a write; SRP's public key is 384 bytes */
#define OUT_MAX        512     /* a status or config response */

static volatile bool active;

bool ble_prov_active(void) { return active; }

/* ---- the setup key ---- */

#if defined(GOGO_BOARD_STICKS3)

#define KEY_GPIO 11   /* KEY1, "button A" (tui.ButtonA), active low */
#define KEY_NAME "button A"

static int key_read(void) {
    gpio_config_t c = { .pin_bit_mask = 1ull << KEY_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    if (gpio_config(&c) != ESP_OK) return -1;
    vTaskDelay(pdMS_TO_TICKS(5));
    int low = 0;
    for (int i = 0; i < 5; i++) {
        low += gpio_get_level(KEY_GPIO) == 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    (void)gpio_reset_pin(KEY_GPIO);   /* back to the default: the gpio port may claim it */
    return low == 5;
}

#elif defined(GOGO_BOARD_CARDPUTER) && CONFIG_GOGO_KBD

/* W is TCA8418 key 12 (input_esp.c's table: HID 0x1A), row 1 / column 1.
 * The chip keeps scanning across an ESP32 reset, so a key held since
 * before it produced its press event long ago: instead the matrix pins
 * are switched to GPIO for a moment, column 1 driven low, and row 1 read
 * (and the other way round); then keypad mode again (kbd_open reconfigures
 * and flushes it in any case). */
#define KEY_NAME "W"
#define TCA_ADDR 0x34
#define TCA_CFG 0x01
#define TCA_INT_STAT 0x02
#define TCA_KEY_EVENT_A 0x04
#define TCA_DAT_STAT1 0x14   /* rows R0-R7 */
#define TCA_DAT_STAT2 0x15   /* columns C0-C7 */
#define TCA_DAT_OUT1 0x17
#define TCA_DAT_OUT2 0x18
#define TCA_KP_GPIO1 0x1D
#define TCA_KP_GPIO2 0x1E
#define TCA_KP_GPIO3 0x1F
#define TCA_DIR1 0x23
#define TCA_DIR2 0x24
#define KEY_BIT 0x02         /* R1, C1 */

static i2c_master_dev_handle_t tca;

static int tca_rd(uint8_t reg, uint8_t *v) {
    return i2c_master_transmit_receive(tca, &reg, 1, v, 1, 20) == ESP_OK ? 0 : -1;
}

static int tca_wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(tca, b, 2, 20) == ESP_OK ? 0 : -1;
}

/* Drives bit KEY_BIT of the out_dir / out_dat pair low and reads it on
 * in_stat: 1 if the key connects them, 0 if not, -1 on an I2C error. */
static int tca_probe(uint8_t out_dir, uint8_t out_dat, uint8_t in_stat) {
    uint8_t dir, dat, v;
    if (tca_rd(out_dir, &dir) != 0 || tca_rd(out_dat, &dat) != 0) return -1;
    if (tca_wr(out_dat, (uint8_t)(dat & ~KEY_BIT)) != 0 || tca_wr(out_dir, (uint8_t)(dir | KEY_BIT)) != 0) return -1;
    esp_rom_delay_us(500);
    int r = tca_rd(in_stat, &v) == 0 ? !(v & KEY_BIT) : -1;
    if (tca_wr(out_dir, dir) != 0 || tca_wr(out_dat, dat) != 0) return -1;
    return r;
}

static int key_read(void) {
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return -1;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &dc, &tca) != ESP_OK) return -1;
    int held = -1;
    if (tca_wr(TCA_KP_GPIO1, 0) == 0 && tca_wr(TCA_KP_GPIO2, 0) == 0 && tca_wr(TCA_KP_GPIO3, 0) == 0) {
        held = 1;
        for (int i = 0; i < 3 && held == 1; i++) {   /* down for 3 samples 10 ms apart */
            int a = tca_probe(TCA_DIR2, TCA_DAT_OUT2, TCA_DAT_STAT1);
            int b = a == 1 ? 1 : tca_probe(TCA_DIR1, TCA_DAT_OUT1, TCA_DAT_STAT2);
            held = a < 0 || b < 0 ? -1 : (a == 1 || b == 1);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    /* keypad mode again, events of the switch dropped */
    uint8_t ev;
    (void)tca_wr(TCA_KP_GPIO1, 0x7F);
    (void)tca_wr(TCA_KP_GPIO2, 0xFF);
    for (int i = 0; i < 20 && tca_rd(TCA_KEY_EVENT_A, &ev) == 0 && ev != 0; i++) {}
    (void)tca_wr(TCA_INT_STAT, 0x1F);
    (void)i2c_master_bus_rm_device(tca);
    tca = NULL;
    return held;
}

#else

/* A Cardputer firmware without the keyboard driver (GOGO_KBD=n) cannot
 * read W: no provisioning. */
#define KEY_NAME "W"
static int key_read(void) { return -1; }

#endif

/* ---- session state ---- */

typedef struct {
    int sta;            /* PROV_STA_* */
    int fail;           /* PROV_FAIL_*, with PROV_STA_FAILED */
    int tries;          /* connection attempts of this apply */
    bool have;          /* settings received */
    bool applying;      /* apply sent, not yet connected or failed */
    bool reported;      /* the phone read Connected */
    int64_t connected_us;
    uint8_t ssid[PROV_SSID_MAX], pass[PROV_PASS_MAX];
    uint8_t ssid_len, pass_len;
} Session;

static Session ss;
static portMUX_TYPE ss_mux = portMUX_INITIALIZER_UNLOCKED;
static protocomm_t *pc;
static volatile bool accepting;     /* advertising again after a disconnect */
static volatile int refused;        /* sessions refused (a wrong PIN) */
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t own_addr_type;
static char dev_name[16];           /* PROV_gogo_XXXX */
static uint8_t *resp[3];            /* the last response per characteristic (host task) */
static uint16_t resp_len[3];

/* ---- prov-config ---- */

static void wipe(void *p, size_t n) {
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

#if !CONFIG_GOGO_PROV_DRYRUN
/* Starts one connection attempt with the received settings, as wifi.c's
 * connect_now does (a password means at least WPA2). */
static esp_err_t join(void) {
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    taskENTER_CRITICAL(&ss_mux);
    memcpy(wc.sta.ssid, ss.ssid, ss.ssid_len);
    memcpy(wc.sta.password, ss.pass, ss.pass_len);
    bool open = ss.pass_len == 0;
    taskEXIT_CRITICAL(&ss_mux);
    wc.sta.threshold.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    wipe(&wc, sizeof wc);
    return e == ESP_OK ? esp_wifi_connect() : e;
}
#endif

static esp_err_t on_config(uint32_t session_id, const uint8_t *in, ssize_t inlen, uint8_t **out, ssize_t *outlen,
                           void *priv) {
    (void)session_id;
    (void)priv;
    ProvRequest *q = calloc(1, sizeof *q);
    uint8_t *o = malloc(OUT_MAX);
    if (!q || !o || inlen < 0) {
        free(q);
        free(o);
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    int rc = prov_decode(in, (size_t)inlen, q);
    if (rc == PROV_DEC_OK && q->msg == PROV_MSG_GET_STATUS) {
        taskENTER_CRITICAL(&ss_mux);
        int sta = ss.sta, fail = ss.fail;
        uint8_t ssid[PROV_SSID_MAX], sl = ss.ssid_len;
        memcpy(ssid, ss.ssid, sl);
        if (sta == PROV_STA_CONNECTED) ss.reported = true;
        taskEXIT_CRITICAL(&ss_mux);
        n = prov_encode_status(o, OUT_MAX, sta, fail, ssid, sl);
    } else if (rc == PROV_DEC_OK && q->msg == PROV_MSG_SET_CONFIG) {
        int status = q->bad ? PROV_STATUS_INVALID_ARG : PROV_STATUS_OK;
        if (!q->bad) {
            taskENTER_CRITICAL(&ss_mux);
            bool busy = ss.applying || ss.sta == PROV_STA_CONNECTED;
            if (!busy) {
                memcpy(ss.ssid, q->ssid, q->ssid_len);
                memcpy(ss.pass, q->pass, q->pass_len);
                ss.ssid_len = q->ssid_len;
                ss.pass_len = q->pass_len;
                ss.have = true;
                ss.sta = PROV_STA_DISCONNECTED;
            }
            taskEXIT_CRITICAL(&ss_mux);
            if (busy) status = PROV_STATUS_INTERNAL;
            else platform_esp_say("gogo: Wi-Fi setup: got network \"%.*s\"", (int)q->ssid_len, (const char *)q->ssid);
        }
        n = prov_encode_resp(o, OUT_MAX, PROV_MSG_SET_CONFIG, status);
    } else if (rc == PROV_DEC_OK && q->msg == PROV_MSG_APPLY_CONFIG) {
        taskENTER_CRITICAL(&ss_mux);
        bool go = ss.have && !ss.applying && ss.sta != PROV_STA_CONNECTED;
        if (go) {
            ss.applying = true;
            ss.tries = 1;
            ss.sta = PROV_STA_CONNECTING;
        }
        taskEXIT_CRITICAL(&ss_mux);
        int status = go ? PROV_STATUS_OK : PROV_STATUS_INTERNAL;
        if (go) {
#if CONFIG_GOGO_PROV_DRYRUN
            platform_esp_say("gogo: dry run: ssid \"%.*s\", password %u bytes; nothing is saved", (int)ss.ssid_len,
                             (const char *)ss.ssid, (unsigned)ss.pass_len);
            taskENTER_CRITICAL(&ss_mux);
            ss.applying = false;
            ss.sta = PROV_STA_CONNECTED;
            ss.connected_us = esp_timer_get_time();
            taskEXIT_CRITICAL(&ss_mux);
#else
            platform_esp_say("gogo: Wi-Fi setup: joining \"%.*s\"", (int)ss.ssid_len, (const char *)ss.ssid);
            if (join() != ESP_OK) {
                taskENTER_CRITICAL(&ss_mux);
                ss.applying = false;
                ss.sta = PROV_STA_FAILED;
                ss.fail = PROV_FAIL_NOT_FOUND;
                taskEXIT_CRITICAL(&ss_mux);
            }
#endif
        }
        n = prov_encode_resp(o, OUT_MAX, PROV_MSG_APPLY_CONFIG, status);
    }
    wipe(q, sizeof *q);
    free(q);
    if (n == 0) {   /* malformed, a message we do not serve, or too long */
        free(o);
        return ESP_FAIL;
    }
    *out = o;
    *outlen = (ssize_t)n;
    return ESP_OK;
}

#if !CONFIG_GOGO_PROV_DRYRUN
/* The station's events while a session applies settings (event-loop task). */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_CONNECTED) {
        taskENTER_CRITICAL(&ss_mux);
        bool was = ss.applying;
        if (was) {
            ss.applying = false;
            ss.sta = PROV_STA_CONNECTED;
            ss.connected_us = esp_timer_get_time();
        }
        taskEXIT_CRITICAL(&ss_mux);
        if (was) platform_esp_say("gogo: Wi-Fi setup: connected");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        bool again = false, failed = false;
        taskENTER_CRITICAL(&ss_mux);
        if (ss.applying) {
            if (ss.tries < CONNECT_TRIES) {
                ss.tries++;
                again = true;
            } else {
                ss.applying = false;
                ss.sta = PROV_STA_FAILED;
                ss.fail = d->reason == WIFI_REASON_NO_AP_FOUND ? PROV_FAIL_NOT_FOUND : PROV_FAIL_AUTH;
                failed = true;
            }
        }
        taskEXIT_CRITICAL(&ss_mux);
        if (again && join() != ESP_OK) again = false;
        if (failed)
            platform_esp_say("gogo: Wi-Fi setup: %s (reason %u); send it again",
                             d->reason == WIFI_REASON_NO_AP_FOUND ? "network not found" : "could not join",
                             (unsigned)d->reason);
    }
}
#endif

/* ---- the GATT service (ESP-IDF's provisioning BLE scheme) ---- */

/* The service UUID (wifi_provisioning / network_provisioning's default,
 * 021a9004-... little-endian bytes); a characteristic's UUID is the
 * service's with bytes 12-13 replaced by its endpoint's 16-bit number. */
#define PROV_UUID(lo, hi) \
    BLE_UUID128_INIT(0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06, 0x7c, 0x87, 0x9b, 0x43, 0x43, 0x6b, lo, hi, 0x75, 0x17)
static const ble_uuid128_t svc_uuid = PROV_UUID(0x4d, 0x24);
static const ble_uuid128_t chr_uuid[3] = { PROV_UUID(0x51, 0xff), PROV_UUID(0x52, 0xff), PROV_UUID(0x53, 0xff) };
static const char *const ep_name[3] = { "prov-session", "prov-config", "proto-ver" };
static const ble_uuid16_t user_desc = BLE_UUID16_INIT(0x2901);

static int dsc_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)ch;
    (void)attr;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_UNLIKELY;
    const char *s = arg;
    return os_mbuf_append(ctxt->om, s, (uint16_t)strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_access(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    int i = (int)(intptr_t)arg;
    if (!pc || i < 0 || i > 2) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (!resp[i] || !resp_len[i]) return 0;
        return os_mbuf_append(ctxt->om, resp[i], resp_len[i]) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om), got = 0;
    if (n == 0) return 0;
    if (n > IN_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t *in = malloc(n);
    if (!in) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (ble_hs_mbuf_to_flat(ctxt->om, in, n, &got) != 0 || got != n) {
        free(in);
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t *out = NULL;
    ssize_t outlen = 0;
    esp_err_t e = protocomm_req_handle(pc, ep_name[i], ch, in, n, &out, &outlen);
    wipe(in, n);
    free(in);
    if (e != ESP_OK || outlen < 0 || outlen > 0xFFFF) {
        free(out);
        if (i == 0) {
            refused++;
            platform_esp_say("gogo: Wi-Fi setup: session refused (wrong PIN?)");
        }
        return BLE_ATT_ERR_INVALID_PDU;
    }
    free(resp[i]);
    resp[i] = out;
    resp_len[i] = (uint16_t)outlen;
    return 0;
}

#define DSC(i) { { .uuid = &user_desc.u, .att_flags = BLE_ATT_F_READ, .access_cb = dsc_access, .arg = (void *)ep_name[i] }, { 0 } }
static struct ble_gatt_dsc_def dscs[3][2] = { DSC(0), DSC(1), DSC(2) };
#define CHR(i) { .uuid = &chr_uuid[i].u, .access_cb = chr_access, .arg = (void *)(intptr_t)(i), \
                 .descriptors = dscs[i], .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE }
static const struct ble_gatt_chr_def chrs[4] = { CHR(0), CHR(1), CHR(2), { 0 } };
static const struct ble_gatt_svc_def svcs[2] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u, .characteristics = chrs },
    { 0 },
};

static void clear_responses(void) {
    for (int i = 0; i < 3; i++) {
        free(resp[i]);
        resp[i] = NULL;
        resp_len[i] = 0;
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void) {
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof r);
    r.name = (const uint8_t *)dev_name;
    r.name_len = (uint8_t)strlen(dev_name);
    r.name_is_complete = 1;
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;   /* 30-60 ms: found quickly */
    p.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc == 0) rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) platform_esp_log("gogo: Wi-Fi setup: advertising failed (%d)", rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn = ev->connect.conn_handle;
            clear_responses();
            if (pc) (void)protocomm_open_session(pc, conn);
            platform_esp_log("gogo: Wi-Fi setup: phone connected");
        } else if (accepting) {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (pc) (void)protocomm_close_session(pc, ev->disconnect.conn.conn_handle);
        clear_responses();
        conn = BLE_HS_CONN_HANDLE_NONE;
        platform_esp_log("gogo: Wi-Fi setup: phone disconnected (reason 0x%x)", (unsigned)ev->disconnect.reason);
        if (accepting) advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (accepting) advertise();
        return 0;
    default:
        return 0;
    }
}

/* ---- the session ---- */

static uint32_t rnd32(void) { return esp_random(); }

static size_t heap_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void restart(void) {
    vTaskDelay(pdMS_TO_TICKS(1500));   /* the last lines reach the screen */
    esp_restart();
}

/* Writes the settings as `gogo setup` does: the password first (removed
 * for an open network), then the SSID. */
static bool save(void) {
#if CONFIG_GOGO_PROV_DRYRUN
    return true;
#else
    int e = ss.pass_len ? config_esp_set("wifi.password", ss.pass, ss.pass_len) : 0;
    if (!ss.pass_len) (void)config_esp_erase("wifi.password");   /* -1 if it was unset */
    if (e == 0) e = config_esp_set("wifi.ssid", ss.ssid, ss.ssid_len);
    return e == 0;
#endif
}

static void session(void) {
    ss.sta = PROV_STA_DISCONNECTED;   /* 0 is Connected */
    char pin[7];
    prov_pin(pin, rnd32);
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) memset(mac, 0, sizeof mac);
    snprintf(dev_name, sizeof dev_name, "PROV_gogo_%02X%02X", mac[4], mac[5]);
    size_t heap_before = heap_free();

    char *salt = NULL, *verifier = NULL;
    int verifier_len = 0;
    esp_err_t e = esp_srp_gen_salt_verifier(USERNAME, (int)strlen(USERNAME), pin, (int)strlen(pin), &salt, SALT_LEN,
                                            &verifier, &verifier_len);
    static protocomm_security2_params_t sec;
    sec.salt = salt;
    sec.salt_len = SALT_LEN;
    sec.verifier = verifier;
    sec.verifier_len = (uint16_t)verifier_len;
    static char version[128];
    int sv = 2;
    uint8_t patch = 0;
    if (e == ESP_OK) e = (pc = protocomm_new()) ? ESP_OK : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = protocomm_set_security(pc, "prov-session", &protocomm_security2, &sec);
    if (e == ESP_OK) (void)protocomm_get_sec_version(pc, &sv, &patch);
    if (e == ESP_OK) e = prov_version_json(version, sizeof version, sv, patch) ? ESP_OK : ESP_FAIL;
    if (e == ESP_OK) e = protocomm_set_version(pc, "proto-ver", version);
    if (e == ESP_OK) e = protocomm_add_endpoint(pc, "prov-config", on_config, NULL);

    /* the station, for joining the network (a dry run brings it up too,
     * so it measures the same memory) */
    if (e == ESP_OK) {
        esp_err_t l = esp_event_loop_create_default();
        if (l != ESP_OK && l != ESP_ERR_INVALID_STATE) e = l;
    }
    if (e == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = 0;   /* gogo's store, not the driver's */
        e = esp_wifi_init(&cfg);
    }
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
#if !CONFIG_GOGO_PROV_DRYRUN
    if (e == ESP_OK) e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
#endif
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_start();
    size_t heap_wifi = heap_free();

    /* the radio */
    bool host = false;
    if (e == ESP_OK) {
        host = ble_host_acquire() == 0;
        if (!host) e = ESP_FAIL;
    }
    if (e == ESP_OK) {
        active = true;
        platform_esp_log("gogo: Wi-Fi setup: ble_prov_active() = %d", (int)ble_prov_active());
        int rc = ble_gatts_reset();
        if (rc == 0) {
            ble_svc_gap_init();
            ble_svc_gatt_init();
            rc = ble_gatts_count_cfg(svcs);
        }
        if (rc == 0) rc = ble_gatts_add_svcs(svcs);
        if (rc == 0) rc = ble_gatts_start();
        if (rc == 0) rc = ble_svc_gap_device_name_set(dev_name);
        if (rc == 0) rc = ble_hs_util_ensure_addr(0);
        if (rc == 0) rc = ble_hs_id_infer_auto(0, &own_addr_type);
        if (rc != 0) {
            platform_esp_log("gogo: Wi-Fi setup: GATT setup failed (%d)", rc);
            e = ESP_FAIL;
        }
    }
    if (e != ESP_OK) {
        platform_esp_say("gogo: Wi-Fi setup unavailable (%s)", esp_err_to_name(e));
    } else {
        accepting = true;
        advertise();
        platform_esp_say("gogo: Wi-Fi setup over Bluetooth, %d s", SESSION_S);
        platform_esp_say("gogo: app: ESP BLE Provisioning");
        platform_esp_say("gogo: device %s", dev_name);
        platform_esp_say("gogo: PIN %s", pin);
        platform_esp_log("gogo: Wi-Fi setup heap (internal free): before %u, wifi up %u, ble up %u (largest %u)",
                         (unsigned)heap_before, (unsigned)heap_wifi, (unsigned)heap_free(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    /* until done, too many refusals or the time is up */
    int64_t start = esp_timer_get_time(), reported_us = 0;
    int last_said = SESSION_S;
    bool ok = false;
    const char *why = "timed out";
    while (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        int left = SESSION_S - (int)((now - start) / 1000000);
        taskENTER_CRITICAL(&ss_mux);
        bool rep = ss.reported, connected = ss.sta == PROV_STA_CONNECTED;
        int64_t cus = ss.connected_us;
        taskEXIT_CRITICAL(&ss_mux);
        if (rep && !reported_us) reported_us = now;
        if (reported_us && now - reported_us >= REPORT_LINGER_MS * 1000ll) {
            ok = true;
            break;
        }
        if (connected && !rep && cus && now - cus >= UNREPORTED_MS * 1000ll) {
            ok = true;   /* joined: the phone went away before it asked */
            break;
        }
        if (refused >= MAX_REFUSED) {
            why = "closed after refused sessions";
            break;
        }
        if (left <= 0) break;
        if (left <= last_said - COUNTDOWN_S) {
            last_said = left - left % COUNTDOWN_S;
            platform_esp_say("gogo: Wi-Fi setup PIN %s, %d s left", pin, left);
        }
    }

    /* close: stop advertising, drop the phone, give the radio back */
    accepting = false;
    if (host) {
        (void)ble_gap_adv_stop();
        uint16_t c = conn;
        if (c != BLE_HS_CONN_HANDLE_NONE) (void)ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 50 && conn != BLE_HS_CONN_HANDLE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(10));
        platform_esp_log("gogo: Wi-Fi setup heap: least internal free since boot %u",
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ble_host_release();
    }
    active = false;
    platform_esp_log("gogo: Wi-Fi setup: ble_prov_active() = %d", (int)ble_prov_active());
    clear_responses();
    if (pc) protocomm_delete(pc);
    pc = NULL;
    free(salt);
    free(verifier);

    if (ok) {
        bool saved = save();
#if CONFIG_GOGO_PROV_DRYRUN
        (void)saved;
        platform_esp_say("gogo: dry run: Wi-Fi settings received, not saved");
#else
        if (saved) platform_esp_say("gogo: Wi-Fi settings saved (%.*s)", (int)ss.ssid_len, (const char *)ss.ssid);
        else platform_esp_say("gogo: Wi-Fi settings could not be saved");
#endif
    } else if (e == ESP_OK) {
        platform_esp_say("gogo: Wi-Fi setup %s, nothing changed", why);
    }
    wipe(ss.pass, sizeof ss.pass);
    wipe(pin, sizeof pin);
    platform_esp_say("gogo: restarting");
    restart();
}

void ble_prov_boot(void) {
    int held = key_read();
    platform_esp_log("gogo: Wi-Fi setup key (%s): %s", KEY_NAME, held < 0 ? "unreadable" : held ? "held" : "not held");
#if CONFIG_GOGO_PROV_TEST_ENTER
    if (held != 1 && esp_reset_reason() != ESP_RST_SW) {
        platform_esp_say("gogo: test build: Wi-Fi setup without the key");
        held = 1;
    }
#endif
    if (held != 1) return;
    session();
}
