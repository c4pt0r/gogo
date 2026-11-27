/* The device configuration store: NVS namespace "gogo", one blob per key,
 * read by sys.Config (esp_host.config_get; the VM never asks for "wifi."
 * keys) and by the Wi-Fi bring-up (wifi.ssid, wifi.password); and the
 * board's side of `gogo config`, `gogo device` and `gogo push`.
 *
 * The `\x02gogo-config` protocol itself is runtime/configsvc (configsvc.h
 * lists the requests). This file gives it the NVS store as a KvHal, the
 * console as its output (whole lines, after anything the VM has buffered
 * on stdout), /flash's backend for push (lfs_be.c takes the volume's lock,
 * so the VM and this task can both use it), and the board's requests:
 * scan (nearby Wi-Fi networks), stats (net counters, memory, fixed tables
 * in use, this task's stack), info (the firmware version, the board, the
 * image format and the requirement names esp_board provides), format-flash (answers ok, then restarts the
 * board, whose next mount formats /flash: fs_lfs_request_format; NVS is
 * not touched). Setting or removing a wifi.* key makes the station
 * reconnect with the new settings. A task reads the USB-Serial/JTAG
 * console input and feeds it to the service. */
#include "sdkconfig.h"
#include "host.h"
#include "config.h"
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
#include "esp_partition.h"
#include "esp_system.h"
#include "vfs_int.h"
#if CONFIG_GOGO_BOARD_PORTS
#include "board_ports.h"
#endif
#include "configsvc.h"
#include "image.h"
#include "port.h"
#include "port_names.h"
#include "board.h"

#ifndef GOGO_FIRMWARE_VERSION
#define GOGO_FIRMWARE_VERSION "dev"
#endif

#define CFG_NS        "gogo"
#define CFG_STACK     4096u    /* bytes; gogo device stats shows the least free */

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

/* The value of key (NUL-terminated) into buf, at most cap bytes; its full
 * length, or -1 if unset. Thread-safe (NVS locks). */
int32_t config_esp_get(const char *key, uint8_t *buf, uint32_t cap) {
    if (!cfg_ok) return -1;
    size_t len = 0;
    if (nvs_get_blob(cfg_nvs, key, NULL, &len) != ESP_OK) return -1;
    if (len > CONFIG_VALUE_MAX) return -1;
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

/* ---- the store as a KvHal ---- */

static int32_t kv_get(void *ctx, const char *key, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    return config_esp_get(key, buf, cap);
}

static const char *kv_set(void *ctx, const char *key, const uint8_t *v, uint32_t n) {
    (void)ctx;
    esp_err_t e = nvs_set_blob(cfg_nvs, key, v, n);
    if (e == ESP_OK) e = nvs_commit(cfg_nvs);
    return e == ESP_OK ? NULL : esp_err_to_name(e);
}

static const char *kv_rm(void *ctx, const char *key) {
    (void)ctx;
    esp_err_t e = nvs_erase_key(cfg_nvs, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) return "not set";
    if (e == ESP_OK) e = nvs_commit(cfg_nvs);
    return e == ESP_OK ? NULL : esp_err_to_name(e);
}

static const char *kv_each(void *ctx, void (*cb)(void *cb_ctx, const char *key), void *cb_ctx) {
    (void)ctx;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find_in_handle(cfg_nvs, NVS_TYPE_BLOB, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        cb(cb_ctx, info.key);
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return NULL;
}

static const KvHal nvs_kv = { .get = kv_get, .set = kv_set, .rm = kv_rm, .each = kv_each };

int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap) {
    return configsvc_vm_get(cfg_ok ? &nvs_kv : NULL, key, klen, buf, cap);
}

/* ---- the board's side of the protocol ---- */

static ConfigSvc svc;   /* only the handler task uses it */

/* One reply line, whole, after anything the VM has buffered on stdout. */
static void out_line(void *ctx, const char *const *parts, const size_t *lens, int n) {
    (void)ctx;
    flockfile(stdout);
    for (int i = 0; i < n; i++) fwrite(parts[i], 1, lens[i], stdout);
    fflush(stdout);
    funlockfile(stdout);
}

/* The reply function of the board reports (tag "kv", "ok" or "err"). */
static void reply(const char *tag, const char *text, size_t n) { configsvc_reply(&svc, tag, text, n); }

static void changed(void *ctx, const char *key) {
    (void)ctx;
#if CONFIG_GOGO_NET
    if (strncmp(key, "wifi.", 5) == 0) wifi_esp_reconfigure();
#else
    (void)key;
#endif
}

/* `gogo device stats`: the net stack's counters and the memory figures
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
    const volatile uint32_t *c = (const volatile uint32_t *)net_stats(&esp_net_state);
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
    const volatile TlsStats *t = tls_stats(&esp_net_state);
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

static void cmd_stats(ConfigSvc *s) {
    (void)s;
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
    /* The fixed tables a program can run out of: used/capacity. */
    uint32_t bytes_used, bytes_max, ports_used, ports_max;
    main_esp_usage(&bytes_used, &bytes_max, &ports_used, &ports_max);
    n = snprintf(line, sizeof line, "bytes=%u/%u", (unsigned)bytes_used, (unsigned)bytes_max);
    reply("kv", line, (size_t)n);
    n = snprintf(line, sizeof line, "ports=%u/%u", (unsigned)ports_used, (unsigned)ports_max);
    reply("kv", line, (size_t)n);
#if CONFIG_GOGO_NET
    n = snprintf(line, sizeof line, "subs=%u/%u", (unsigned)net_subs_used(&esp_net_state), (unsigned)GOGO_NET_MAX_SUBS);
    reply("kv", line, (size_t)n);
#endif
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
    /* This task's stack: the least ever free (it runs LittleFS for gogo push). */
    n = snprintf(line, sizeof line, "console_stack_free=%u/%u", (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)CFG_STACK);
    reply("kv", line, (size_t)n);
    reply("ok", NULL, 0);
}

#if CONFIG_GOGO_NET
static void cmd_scan(ConfigSvc *s) {
    static const char busy[] = "scan failed (wifi busy connecting; try again)";
    int n = wifi_esp_scan(s->scratch, 80, reply);
    if (n < 0) reply("err", busy, sizeof busy - 1u);
    else reply("ok", NULL, 0);
}
#endif

/* ---- format-flash ---- */

static void cmd_format_flash(ConfigSvc *s) {
    (void)s;
    static const char none[] = "no gogofs partition", ok[] = "restarting to format /flash";
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "gogofs")) {
        reply("err", none, sizeof none - 1u);
        return;
    }
    reply("ok", ok, sizeof ok - 1u);
    vTaskDelay(pdMS_TO_TICKS(200));   /* let the reply leave */
    fs_lfs_request_format();
    esp_restart();
}

/* ---- info ---- */

/* The requirement names of esp_board (an image's requirements are checked
 * against the same lists, runtime/vm/module.c): its drivers, its mounts,
 * "float" in a float build, and the board ports (GOGO_BOARD_PORT drivers
 * of other components) the boot registers. Filled on the first info. */
#define INFO_NAMES 32
static const char *info_names[INFO_NAMES + 1];

static void cmd_info(void *ctx, ConfigInfo *out) {
    (void)ctx;
    if (!info_names[0]) {
        size_t n = 0;
        for (const struct PortDriver *const *d = esp_board.ports; *d && n < INFO_NAMES; d++) info_names[n++] = (*d)->name;
        for (const VmMount *const *m = esp_board.mounts; *m && n < INFO_NAMES; m++) info_names[n++] = (*m)->prefix;
#if GOGO_FLOAT
        if (n < INFO_NAMES) info_names[n++] = PORT_REQ_FLOAT;
#endif
#if CONFIG_GOGO_BOARD_PORTS
        uint32_t k = board_ports_names(&esp_board, &info_names[n], (uint32_t)(INFO_NAMES - n));
        n += k < INFO_NAMES - n ? k : INFO_NAMES - n;
#endif
        info_names[n] = NULL;
    }
    out->fw = GOGO_FIRMWARE_VERSION;
    out->board = BOARD_NAME;
    out->image_major = IMAGE_MAJOR;
    out->image_minor = IMAGE_MINOR;
    out->provides = info_names;
    main_esp_params(out->params, sizeof out->params);
}

static const ConfigBoard esp_cfg_board = {
    .write = out_line,
    .changed = changed,
#if CONFIG_GOGO_NET
    .scan = cmd_scan,
#endif
    .stats = cmd_stats,
    .format_flash = cmd_format_flash,
    .info = cmd_info,
};

static void config_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof buf, portMAX_DELAY);
        if (got > 0) {
            configsvc_feed(&svc, buf, (size_t)got);
            memset(buf, 0, (size_t)got);   /* a set may have carried the password */
        }
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
    configsvc_init(&svc, cfg_ok ? &nvs_kv : NULL, &esp_cfg_board, esp_flash_mount.be, esp_flash_mount.ctx);
    xTaskCreatePinnedToCore(config_task, "gogo_cfg", CFG_STACK, NULL, 2, NULL, 0);
}
