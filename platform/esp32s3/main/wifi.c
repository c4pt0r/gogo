/* Wi-Fi station and the platform network interface (net/netif.h) on the
 * ESP32-S3 (spec M5 §5.1, §5.4, §9.8, §9.9).
 *
 * Raw Ethernet frames go straight between the Wi-Fi driver and gogo's own
 * stack (net/) through the driver's internal interface:
 *  - receive: esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb); rx_cb runs in
 *    the Wi-Fi task (CPU0), offers the frame to the stack's single RX slot
 *    (net_rx_offer copies it or drops it) and frees the driver buffer at
 *    once (esp_wifi_internal_free_rx_buffer);
 *  - send: esp_wifi_internal_tx(WIFI_IF_STA, ...), which copies the frame,
 *    from the VM task.
 * esp_netif and lwIP are never initialised (esp_wifi links against them,
 * but only code it references is kept; no lwIP task or pools exist).
 *
 * Station: the SSID and password come from the config store (wifi.ssid,
 * wifi.password; config_esp.c). It connects at start and reconnects after
 * every disconnect with exponential backoff (1 s doubling to 32 s, reset
 * on a successful connection); `gogo config set wifi.*` reconnects at once
 * with the new settings. Link changes go to net_link_event (the stack's
 * single link slot: the latest state wins). */
#include "platform.h"
#include "platform_esp.h"
#include "netif.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

#define BACKOFF_MIN_MS 1000u
#define BACKOFF_MAX_MS 32000u

static bool wifi_ok;              /* driver initialised and started */
static volatile bool link_up;     /* associated (data frames may flow) */
static uint8_t sta_mac[6];
static esp_timer_handle_t retry_timer;
static uint32_t backoff_ms = BACKOFF_MIN_MS;
static bool fail_said;            /* the first failure of this attempt was logged */
static volatile bool scanning;    /* written by the config task only */
static bool changed;              /* the next disconnect is wifi_esp_reconfigure's */

ESP_EVENT_DEFINE_BASE(GOGO_WIFI_EVENT);
enum { EV_RETRY, EV_RECONFIG, EV_SCAN_START, EV_SCAN_DONE };

/* Memory measured at start (serial report). */
static size_t heap_boot, heap_inited, heap_started;

static size_t heap_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

/* ---- receive ---- */

static esp_err_t rx_cb(void *buffer, uint16_t len, void *eb) {
    net_rx_offer((const uint8_t *)buffer, len);
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

/* ---- connect / reconnect ---- */

static void retry_in(uint32_t ms);
static void retry_later(void);

/* Loads wifi.ssid / wifi.password and connects; false if not configured.
 * If the driver refuses, retries later (no DISCONNECTED event follows). */
static bool connect_now(void) {
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    int32_t sl = config_esp_get("wifi.ssid", wc.sta.ssid, sizeof wc.sta.ssid);
    if (sl <= 0 || sl > (int32_t)sizeof wc.sta.ssid) {
        platform_esp_say("gogo: wifi not configured (gogo config set wifi.ssid ...)");
        return false;
    }
    int32_t pl = config_esp_get("wifi.password", wc.sta.password, sizeof wc.sta.password - 1u);
    if (pl > (int32_t)sizeof wc.sta.password - 1) {
        platform_esp_say("gogo: wifi.password too long (at most 63 bytes)");
        return false;
    }
    if (pl < 0) pl = 0;
    wc.sta.password[pl] = 0;
    /* A password means at least WPA2 (no silent downgrade to an open AP of
     * the same name); the strongest AP of that name on any channel. */
    wc.sta.threshold.authmode = pl > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc.sta.password, 0, sizeof wc.sta.password);
    if (e == ESP_OK) e = esp_wifi_connect();
    if (e != ESP_OK) {
        platform_esp_log("gogo: wifi connect: %s", esp_err_to_name(e));
        retry_later();
    }
    return true;
}

/* The timer only posts: connecting runs on the event-loop task. */
static void retry_cb(void *arg) {
    (void)arg;
    if (esp_event_post(GOGO_WIFI_EVENT, EV_RETRY, NULL, 0, 0) != ESP_OK) {
        (void)esp_timer_start_once(retry_timer, 100000);   /* loop queue full: try again soon */
    }
}

static void retry_in(uint32_t ms) {
    (void)esp_timer_stop(retry_timer);
    (void)esp_timer_start_once(retry_timer, (uint64_t)ms * 1000u);
}

/* Next attempt after the current backoff, which then doubles. */
static void retry_later(void) {
    platform_esp_log("gogo: wifi retry in %lu ms", (unsigned long)backoff_ms);
    retry_in(backoff_ms);
    backoff_ms = backoff_ms >= BACKOFF_MAX_MS / 2u ? BACKOFF_MAX_MS : backoff_ms * 2u;
}

static const char *reason_text(uint8_t r) {
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL: return "wrong password or refused";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "access point busy";
    default: return "disconnected";
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        heap_started = heap_free();
        (void)connect_now();
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *c = data;
        (void)esp_timer_stop(retry_timer);   /* a pending retry would reconnect */
        backoff_ms = BACKOFF_MIN_MS;
        fail_said = false;
        changed = false;
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        platform_esp_say("gogo: wifi connected (%.*s, ch %u, %d dBm)", (int)c->ssid_len, (const char *)c->ssid,
                         (unsigned)c->channel, rssi);
        (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb);
        link_up = true;
        net_link_event(true);
        platform_esp_log("gogo: wifi heap: after start %u, connected %u + VM arena %u (internal free)",
                         (unsigned)heap_started, (unsigned)heap_free(), (unsigned)platform_esp_arena_used());
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        bool was_up = link_up;
        link_up = false;
        if (changed) {
            changed = false;
            if (was_up) net_link_event(false);
            platform_esp_say("gogo: wifi settings changed, reconnecting");
            retry_in(300);   /* even if the reconfigure's own retry already fired */
            break;
        }
        if (was_up) {
            net_link_event(false);
            platform_esp_say("gogo: wifi lost (%s, reason %u), reconnecting", reason_text(d->reason),
                             (unsigned)d->reason);
        } else if (!fail_said) {
            fail_said = true;
            platform_esp_say("gogo: wifi: %s (reason %u), retrying", reason_text(d->reason), (unsigned)d->reason);
        }
        retry_later();
        break;
    }
    default:
        break;
    }
}

/* gogo's own events, posted by the retry timer, wifi_esp_reconfigure and
 * wifi_esp_scan: all station state (backoff, fail_said, changed, the retry
 * timer) is changed only here and in on_wifi, i.e. on the event-loop task. */
static void on_gogo(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    switch (id) {
    case EV_RETRY:
        if (link_up) break;               /* connected meanwhile */
        if (scanning) { retry_in(1000); break; }
        (void)connect_now();
        break;
    case EV_RECONFIG:
        (void)esp_timer_stop(retry_timer);
        backoff_ms = BACKOFF_MIN_MS;
        fail_said = false;
        changed = link_up;                /* a DISCONNECTED follows only if associated */
        (void)esp_wifi_disconnect();
        retry_in(300);
        break;
    case EV_SCAN_START:
        (void)esp_timer_stop(retry_timer);
        if (!link_up) (void)esp_wifi_disconnect();   /* stop a connection attempt */
        break;
    case EV_SCAN_DONE:
        if (!link_up) retry_in(100);
        break;
    default:
        break;
    }
}

void wifi_esp_start(void) {
    heap_boot = heap_free();
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_err_t e = esp_timer_create(&ta, &retry_timer);
    if (e == ESP_OK) e = esp_event_loop_create_default();
    if (e == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable = 0;   /* settings come from gogo's store, not the driver's */
        e = esp_wifi_init(&cfg);
    }
    heap_inited = heap_free();
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e == ESP_OK) e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    if (e == ESP_OK) e = esp_event_handler_register(GOGO_WIFI_EVENT, ESP_EVENT_ANY_ID, on_gogo, NULL);
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    /* No modem sleep: a served page or a ping answers at once. */
    if (e == ESP_OK) e = esp_wifi_set_ps(WIFI_PS_NONE);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e != ESP_OK) {
        platform_esp_say("gogo: wifi unavailable (%s)", esp_err_to_name(e));
        return;
    }
    wifi_ok = true;
}

void wifi_esp_reconfigure(void) {
    if (wifi_ok) (void)esp_event_post(GOGO_WIFI_EVENT, EV_RECONFIG, NULL, 0, portMAX_DELAY);
}

/* Nearby networks for `gogo config scan`: one kv line each
 * ("<ssid>  ch <n>  <rssi> dBm  <auth>"). -1 if the driver is busy. */
int wifi_esp_scan(char *line, size_t cap, void (*out)(const char *tag, const char *text, size_t n)) {
    if (!wifi_ok) return -1;
    scanning = true;
    (void)esp_event_post(GOGO_WIFI_EVENT, EV_SCAN_START, NULL, 0, portMAX_DELAY);
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = ESP_FAIL;
    for (int tries = 0; tries < 5 && e != ESP_OK; tries++) {
        e = esp_wifi_scan_start(&sc, true);
        if (e != ESP_OK) vTaskDelay(pdMS_TO_TICKS(300));
    }
    int n = -1;
    if (e == ESP_OK) {
        uint16_t num = 20;
        wifi_ap_record_t *recs = calloc(num, sizeof *recs);
        if (recs && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
            n = num;
            for (uint16_t i = 0; i < num; i++) {
                int l = snprintf(line, cap, "%-32s ch %2u %4d dBm %s", (const char *)recs[i].ssid,
                                 (unsigned)recs[i].primary, recs[i].rssi,
                                 recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "secured");
                if (l > 0) out("kv", line, (size_t)l < cap ? (size_t)l : cap - 1u);
            }
        } else {
            (void)esp_wifi_clear_ap_list();
        }
        free(recs);
    }
    scanning = false;
    (void)esp_event_post(GOGO_WIFI_EVENT, EV_SCAN_DONE, NULL, 0, portMAX_DELAY);
    return n;
}

/* ---- net/netif.h ---- */

bool platform_net_mac(uint8_t mac[6]) {
    if (!wifi_ok) return false;
    memcpy(mac, sta_mac, 6);
    return true;
}

int platform_net_send(const uint8_t *frame, uint32_t len) {
    if (!link_up || len > 0xFFFFu) return -1;
    return esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, (uint16_t)len) == ESP_OK ? 0 : -1;
}

bool platform_net_link_up(void) { return link_up; }

void platform_net_report(bool up, uint32_t ip, uint32_t gw, uint32_t dns) {
    (void)dns;
    if (up) {
        (void)esp_wifi_internal_set_sta_ip();
        platform_esp_say("gogo: ip %lu.%lu.%lu.%lu (gateway %lu.%lu.%lu.%lu)", (unsigned long)(ip >> 24),
                         (unsigned long)(ip >> 16 & 255u), (unsigned long)(ip >> 8 & 255u),
                         (unsigned long)(ip & 255u), (unsigned long)(gw >> 24), (unsigned long)(gw >> 16 & 255u),
                         (unsigned long)(gw >> 8 & 255u), (unsigned long)(gw & 255u));
    } else {
        platform_esp_say("gogo: network down");
    }
}

/* Serial: the measured Wi-Fi memory at start. */
void wifi_esp_report_heap(void) {
    platform_esp_log("gogo: wifi heap: boot %u, after esp_wifi_init %u, after start %u (internal free)",
               (unsigned)heap_boot, (unsigned)heap_inited, (unsigned)heap_started);
}
