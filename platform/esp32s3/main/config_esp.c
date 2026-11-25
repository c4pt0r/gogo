/* The device configuration store (spec M5 §5.4): NVS namespace "gogo", one
 * blob per key, read by config.Get (platform_config_get; the VM never asks
 * for "wifi." keys) and by the Wi-Fi bring-up (wifi.ssid, wifi.password).
 *
 * `gogo config` reaches it over the USB-Serial/JTAG console: a task reads
 * the console input and answers command lines
 *
 *     \x02gogo-config set <key> <value>\n    (value: the rest of the line)
 *     \x02gogo-config get <key>\n
 *     \x02gogo-config rm <key>\n
 *     \x02gogo-config list\n
 *     \x02gogo-config scan\n                 (nearby Wi-Fi networks)
 *     \x02gogo-config stats\n                (the net stack's counters)
 *
 * with zero or more `\x02kv <text>\n` lines and then one `\x02ok[ <text>]\n`
 * or `\x02err <reason>\n`. Input outside a frame is ignored (the console has
 * no other input). Every reply line starts with \x02 so the host can pick
 * it out of program output, which may be mid-line. The Wi-Fi password is
 * never sent back: get and list show "(set)". Setting or removing a
 * wifi.* key makes the station reconnect with the new settings. */
#include "sdkconfig.h"
#include "platform.h"
#include "platform_esp.h"
#if CONFIG_GOGO_TLS
#include "tls.h"
#endif
#if CONFIG_GOGO_NET
#include "net.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"

#define CFG_NS        "gogo"
#define CFG_KEY_MAX   15u      /* NVS key limit (16 with the NUL) */
#define CFG_VALUE_MAX 1000u    /* as cmd/gogo/config.go */
#define CFG_SECRET    "wifi.password"
#define CFG_LINE_MAX  (12u + 5u + CFG_KEY_MAX + CFG_VALUE_MAX + 4u)

static nvs_handle_t cfg_nvs;
static bool cfg_ok;

int config_esp_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* an unreadable layout (never written by this firmware): start over */
        if (nvs_flash_erase() == ESP_OK) e = nvs_flash_init();
    }
    if (e == ESP_OK) e = nvs_open(CFG_NS, NVS_READWRITE, &cfg_nvs);
    cfg_ok = e == ESP_OK;
    if (!cfg_ok) platform_esp_say("gogo: config store unavailable (%s)", esp_err_to_name(e));
    return cfg_ok ? 0 : -1;
}

static bool valid_key(const char *k, size_t n) {
    if (n == 0 || n > CFG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

/* The value of key (NUL-terminated) into buf, at most cap bytes; its full
 * length, or -1 if unset. Thread-safe (NVS locks). */
int32_t config_esp_get(const char *key, uint8_t *buf, uint32_t cap) {
    if (!cfg_ok) return -1;
    size_t len = 0;
    if (nvs_get_blob(cfg_nvs, key, NULL, &len) != ESP_OK) return -1;
    if (len > CFG_VALUE_MAX) return -1;
    if (!buf || cap == 0 || len == 0) return (int32_t)len;
    if (cap >= len) {
        size_t n = len;
        if (nvs_get_blob(cfg_nvs, key, buf, &n) != ESP_OK || n != len) return -1;
        return (int32_t)len;
    }
    uint8_t *tmp = malloc(len);
    if (!tmp) return -1;
    size_t n = len;
    int32_t r = -1;
    if (nvs_get_blob(cfg_nvs, key, tmp, &n) == ESP_OK && n == len) {
        memcpy(buf, tmp, cap);
        r = (int32_t)len;
    }
    memset(tmp, 0, len);   /* it may be the Wi-Fi password */
    free(tmp);
    return r;
}

int config_esp_erase(const char *key) {
    if (!cfg_ok) return -1;
    esp_err_t e = nvs_erase_key(cfg_nvs, key);
    if (e == ESP_OK) e = nvs_commit(cfg_nvs);
    return e == ESP_OK ? 0 : -1;
}

int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap) {
    char k[CFG_KEY_MAX + 1u];
    if (!valid_key((const char *)key, klen)) return -1;
    memcpy(k, key, klen);
    k[klen] = 0;
    if (strncmp(k, "wifi.", 5) == 0) return -1;   /* platform-only (the VM checks too) */
    return config_esp_get(k, buf, cap);
}

/* ---- serial command handler ---- */

/* Reply scratch for get, list and scan (only the handler task uses it). */
static char scratch[CFG_KEY_MAX + 2u + CFG_VALUE_MAX];

/* One reply line: \x02, the text, \n; whole, after anything the VM has
 * buffered on stdout. */
static void reply(const char *tag, const char *text, size_t n) {
    flockfile(stdout);
    fputc('\x02', stdout);
    fputs(tag, stdout);
    if (text) {
        fputc(' ', stdout);
        fwrite(text, 1, n, stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}
static void reply_s(const char *tag, const char *text) { reply(tag, text, text ? strlen(text) : 0); }

static void cmd_get(const char *key) {
    uint8_t *v = (uint8_t *)scratch;
    bool secret = strcmp(key, CFG_SECRET) == 0;   /* only its presence is read */
    int32_t n = config_esp_get(key, secret ? NULL : v, secret ? 0u : CFG_VALUE_MAX);
    if (n < 0) { reply_s("err", "not set"); return; }
    if (secret) { reply_s("ok", "(set)"); return; }
    reply("ok", (const char *)v, (size_t)n);
}

static void cmd_list(void) {
    char *line = scratch;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find_in_handle(cfg_nvs, NVS_TYPE_BLOB, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t kl = strlen(info.key);
        memcpy(line, info.key, kl);
        line[kl] = '=';
        size_t n = kl + 1u;
        if (strcmp(info.key, CFG_SECRET) == 0) {
            memcpy(line + n, "(set)", 5);
            n += 5u;
        } else {
            int32_t vl = config_esp_get(info.key, (uint8_t *)line + n, CFG_VALUE_MAX);
            if (vl > 0) n += (size_t)vl;
        }
        reply("kv", line, n);
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    reply_s("ok", NULL);
}

static void cmd_set(const char *key, const char *value, size_t vn) {
    if (vn > CFG_VALUE_MAX || memchr(value, '\x02', vn) || memchr(value, '\r', vn)) {
        reply_s("err", "bad value");
        return;
    }
    esp_err_t e = nvs_set_blob(cfg_nvs, key, value, vn);
    if (e == ESP_OK) e = nvs_commit(cfg_nvs);
    if (e != ESP_OK) { reply_s("err", esp_err_to_name(e)); return; }
    reply_s("ok", NULL);
#if CONFIG_GOGO_NET
    if (strncmp(key, "wifi.", 5) == 0) wifi_esp_reconfigure();
#endif
}

static void cmd_rm(const char *key) {
    esp_err_t e = nvs_erase_key(cfg_nvs, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) { reply_s("err", "not set"); return; }
    if (e == ESP_OK) e = nvs_commit(cfg_nvs);
    if (e != ESP_OK) { reply_s("err", esp_err_to_name(e)); return; }
    reply_s("ok", NULL);
#if CONFIG_GOGO_NET
    if (strncmp(key, "wifi.", 5) == 0) wifi_esp_reconfigure();
#endif
}

/* `gogo config stats`: the net stack's counters and the memory figures
 * (read from this task while the VM updates them: a snapshot that may be
 * a frame behind). */
#if CONFIG_GOGO_NET
static void stats_net(void) {
    static const char *const names[] = {
        "rx_frames", "rx_busy", "rx_oversize", "eth_bad", "eth_filtered", "eth_unknown", "arp_bad",
        "arp_ignored", "ip_bad", "ip_frag", "ip_filtered", "ip_proto", "icmp_bad", "icmp_echo", "udp_bad",
        "udp_noport", "udp_quota", "udp_rx", "tx_frames", "tx_fail", "tx_arp_miss", "tx_noroute", "tx_bad",
        "tcp_bad", "tcp_noconn", "tcp_drop", "tcp_quota", "tcp_syn_drop", "tcp_retx", "tcp_rx",
    };
    _Static_assert(sizeof names / sizeof names[0] == sizeof(NetStats) / sizeof(uint32_t), "NetStats fields");
    const volatile uint32_t *c = (const volatile uint32_t *)net_stats();
    char line[48];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        int n = snprintf(line, sizeof line, "%s=%lu", names[i], (unsigned long)c[i]);
        reply("kv", line, (size_t)n);
    }
}
#endif

#if CONFIG_GOGO_TLS
/* The tls port's counters (runtime/net/tls/tls.h TlsStats): slices and their longest
 * time, the last handshake's time. */
static void stats_tls(void) {
    const volatile TlsStats *t = tls_stats();
    char line[48];
    const struct { const char *name; uint32_t v; } kv[] = {
        {"tls_opens", t->opens}, {"tls_handshakes", t->handshakes}, {"tls_steps", t->steps},
        {"tls_restarts", t->restarts}, {"tls_max_step_us", t->max_step_us},
        {"tls_max_step_state", (uint32_t)t->max_step_state}, {"tls_max_io_us", t->max_io_us},
        {"tls_last_hs_us", t->last_hs_us}, {"tls_live", t->live},
    };
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) {
        int n = snprintf(line, sizeof line, "%s=%lu", kv[i].name, (unsigned long)kv[i].v);
        reply("kv", line, (size_t)n);
    }
    int n = snprintf(line, sizeof line, "tls_last_error=-0x%04x", (unsigned)-t->last_error);
    reply("kv", line, (size_t)n);
}
#endif

static void cmd_stats(void) {
#if CONFIG_GOGO_NET
    stats_net();
#endif
    char line[48];
    /* Internal RAM: free now, lowest since boot; the VM arena's use. */
    int n = snprintf(line, sizeof line, "heap_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    reply("kv", line, (size_t)n);
    n = snprintf(line, sizeof line, "heap_min=%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    reply("kv", line, (size_t)n);
    n = snprintf(line, sizeof line, "arena_used=%u", (unsigned)platform_esp_arena_used());
    reply("kv", line, (size_t)n);
#if CONFIG_SPIRAM
    /* PSRAM (StickS3): free now, lowest since boot. */
    n = snprintf(line, sizeof line, "psram_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    reply("kv", line, (size_t)n);
    n = snprintf(line, sizeof line, "psram_min=%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    reply("kv", line, (size_t)n);
#endif
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    {
        size_t now, peak;
        platform_esp_tls_mem(&now, &peak);
        n = snprintf(line, sizeof line, "mbedtls_mem=%u", (unsigned)now);
        reply("kv", line, (size_t)n);
        n = snprintf(line, sizeof line, "mbedtls_mem_peak=%u", (unsigned)peak);
        reply("kv", line, (size_t)n);
    }
#endif
#if CONFIG_GOGO_TLS
    stats_tls();
#endif
    reply_s("ok", NULL);
}

static void cmd_scan(void) {
#if CONFIG_GOGO_NET
    int n = wifi_esp_scan(scratch, 80, reply);
    if (n < 0) reply_s("err", "scan failed (wifi busy connecting; try again)");
    else reply_s("ok", NULL);
#else
    reply_s("err", "no network in this firmware");
#endif
}

/* line: the frame after \x02, without the \n. */
static void handle(char *line, size_t n) {
    static const char magic[] = "gogo-config ";
    if (n < sizeof magic - 1u || memcmp(line, magic, sizeof magic - 1u) != 0) return;  /* not ours */
    char *p = line + sizeof magic - 1u, *end = line + n;
    if (!cfg_ok) { reply_s("err", "config store unavailable"); return; }
    char *op = p;
    while (p < end && *p != ' ') p++;
    size_t opn = (size_t)(p - op);
    if (p < end) p++;
    if (opn == 4 && memcmp(op, "list", 4) == 0 && p >= end) { cmd_list(); return; }
    if (opn == 4 && memcmp(op, "scan", 4) == 0 && p >= end) { cmd_scan(); return; }
    if (opn == 5 && memcmp(op, "stats", 5) == 0 && p >= end) { cmd_stats(); return; }
    char *key = p;
    while (p < end && *p != ' ') p++;
    size_t kn = (size_t)(p - key);
    bool more = p < end;
    if (more) *p++ = 0;
    else *end = 0;
    if (!valid_key(key, kn)) { reply_s("err", "bad key"); return; }
    key[kn] = 0;
    if (opn == 3 && memcmp(op, "set", 3) == 0 && more) cmd_set(key, p, (size_t)(end - p));
    else if (opn == 3 && memcmp(op, "get", 3) == 0 && !more) cmd_get(key);
    else if (opn == 2 && memcmp(op, "rm", 2) == 0 && !more) cmd_rm(key);
    else reply_s("err", "usage: set <key> <value> | get <key> | rm <key> | list | scan");
}

static void config_task(void *arg) {
    (void)arg;
    static char line[CFG_LINE_MAX + 1u];
    size_t n = 0;
    bool in = false, over = false;
    uint8_t buf[64];
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof buf, portMAX_DELAY);
        for (int i = 0; i < got; i++) {
            char c = (char)buf[i];
            if (c == '\x02') { in = true; n = 0; over = false; memset(line, 0, sizeof line); continue; }
            if (!in) continue;
            if (c == '\n') {
                in = false;
                if (n > 0 && line[n - 1] == '\r') n--;
                if (over) reply_s("err", "line too long");
                else handle(line, n);
                memset(line, 0, sizeof line);   /* a set may have carried the password */
                continue;
            }
            if (n < CFG_LINE_MAX) line[n++] = c;
            else over = true;
        }
        if (got > 0) memset(buf, 0, (size_t)got);
    }
}

void config_esp_start_serial(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        platform_esp_say("gogo: serial input unavailable (gogo config disabled)");
        return;
    }
    fflush(stdout);
    fflush(stderr);
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(config_task, "gogo_cfg", 4096, NULL, 2, NULL, 0);
}
