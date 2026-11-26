/* The device configuration store: NVS namespace "gogo", one
 * blob per key, read by sys.Config (esp_host.config_get; the VM never asks
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
 *     \x02gogo-config stats\n                (net counters, memory, fixed tables in use)
 *     \x02gogo-config push <path> <len> <crc32>\n   (gogo push: write a file
 *     \x02gogo-config data <offset> <base64>\n       to /flash, in chunks; push
 *     \x02gogo-config end\n                          and data answer ok <bytes
 *     \x02gogo-config abort\n                        so far>)
 *
 * with zero or more `\x02kv <text>\n` lines and then one `\x02ok[ <text>]\n`
 * or `\x02err <reason>\n`. Input outside a frame is ignored (the console has
 * no other input). Every reply line starts with \x02 so the host can pick
 * it out of program output, which may be mid-line. The Wi-Fi password is
 * never sent back: get and list show "(set)". Setting or removing a
 * wifi.* key makes the station reconnect with the new settings.
 *
 * push writes a file to /flash (fs_lfs.c): <path> is an absolute /flash
 * path, <len> its size and <crc32> the CRC-32 (IEEE) of its bytes in hex.
 * The bytes follow in data frames (base64, CFG_PUSH_CHUNK bytes at most,
 * <offset> the count sent before it), each answered before the next is
 * sent, so the console's small input buffer never overflows; end checks
 * the stored file against <len> and <crc32> and only then renames it over
 * <path>. A failure, abort or a new push drops the temporary file. */
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
#include "vfs_int.h"

#define CFG_NS        "gogo"
#define CFG_KEY_MAX   15u      /* NVS key limit (16 with the NUL) */
#define CFG_VALUE_MAX 1000u    /* as cmd/gogo/config.go */
#define CFG_SECRET    "wifi.password"
#define CFG_LINE_MAX  (12u + 5u + CFG_KEY_MAX + CFG_VALUE_MAX + 4u)
#define CFG_PUSH_CHUNK 128u    /* as cmd/gogo/push.go */

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
    /* The fixed tables a program can run out of: used/capacity. */
    uint32_t bytes_used, ports_used;
    main_esp_usage(&bytes_used, &ports_used);
    n = snprintf(line, sizeof line, "bytes=%u/%u", (unsigned)bytes_used, (unsigned)GOGO_BYTES_HANDLES);
    reply("kv", line, (size_t)n);
    n = snprintf(line, sizeof line, "ports=%u/%u", (unsigned)ports_used, (unsigned)GOGO_MAX_PORTS);
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

/* ---- gogo push ---- */

static const char *err_text(int e) {
    switch (e) {
    case GOGO_E_NOTMOUNTED: return "/flash is not mounted";
    case GOGO_E_NOSPACE:    return "no space on /flash";
    case GOGO_E_BADPATH:    return "bad path";
    case GOGO_E_ISDIR:      return "the path is a directory";
    case GOGO_E_NOTDIR:     return "a directory on the path is a file";
    case GOGO_E_NOTFOUND:   return "no such directory";
    case GOGO_E_BADARG:     return "no push in progress";
    case GOGO_E_IO:         return "the stored file does not match (length or CRC)";
    default:                return "I/O error";
    }
}

/* Parses a decimal (base 10) or hex (base 16) number filling [p, end). */
static bool parse_u32(const char *p, const char *end, int base, uint32_t *out) {
    uint64_t v = 0;
    if (p >= end || end - p > 10) return false;
    for (; p < end; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : base == 16 && (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (v > 0xFFFFFFFFu) return false;
    *out = (uint32_t)v;
    return true;
}

/* Decodes base64 [p, end) into out (cap bytes); its length, or -1. */
static int b64_decode(const char *p, const char *end, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; p < end && *p != '='; p++) {
        char c = *p;
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

/* The /flash path of the push in progress, relative ("dir/name.ggb"). */
static char push_rel[VFS_PATH_MAX];
static uint32_t push_len, push_crc;

static void cmd_push(char *p, char *end) {
    char *path = p;
    while (p < end && *p != ' ') p++;
    size_t pn = (size_t)(p - path);
    char *ls = p < end ? p + 1 : end, *le = ls;
    while (le < end && *le != ' ') le++;
    char *cs = le < end ? le + 1 : end;
    uint32_t first;
    static const char pre[] = "/flash/";
    if (pn <= sizeof pre - 1u || memcmp(path, pre, sizeof pre - 1u) != 0 || vfs_path_check(path, (uint32_t)pn, &first) != GOGO_E_OK ||
        !parse_u32(ls, le, 10, &push_len) || !parse_u32(cs, end, 16, &push_crc)) {
        fs_lfs_push_abort();
        reply_s("err", "usage: push </flash/path> <len> <crc32 hex>");
        return;
    }
    memcpy(push_rel, path + sizeof pre - 1u, pn - (sizeof pre - 1u));
    push_rel[pn - (sizeof pre - 1u)] = 0;
    int e = fs_lfs_push_begin(push_rel);
    if (e != GOGO_E_OK) { reply_s("err", err_text(e)); return; }
    reply_s("ok", "0");
}

static void cmd_data(char *p, char *end) {
    char *os = p;
    while (p < end && *p != ' ') p++;
    uint32_t off, total;
    uint8_t buf[CFG_PUSH_CHUNK];
    int n = p < end ? b64_decode(p + 1, end, buf, sizeof buf) : -1;
    if (!parse_u32(os, p, 10, &off) || n <= 0) {
        fs_lfs_push_abort();
        reply_s("err", "bad data frame");
        return;
    }
    /* A chunk at the wrong offset (a lost or repeated frame) ends the push. */
    int e = fs_lfs_push_write(buf, (uint32_t)n, &total);
    if (e == GOGO_E_OK && total != off + (uint32_t)n) {
        fs_lfs_push_abort();
        reply_s("err", "data out of order");
        return;
    }
    if (e != GOGO_E_OK) { reply_s("err", err_text(e)); return; }
    char t[12];
    int tn = snprintf(t, sizeof t, "%lu", (unsigned long)total);
    reply("ok", t, (size_t)tn);
}

static void cmd_end(void) {
    int e = fs_lfs_push_end(push_rel, push_len, push_crc);
    if (e != GOGO_E_OK) { reply_s("err", err_text(e)); return; }
    reply_s("ok", NULL);
}

/* line: the frame after \x02, without the \n. */
static void handle(char *line, size_t n) {
    static const char magic[] = "gogo-config ";
    if (n < sizeof magic - 1u || memcmp(line, magic, sizeof magic - 1u) != 0) return;  /* not ours */
    char *p = line + sizeof magic - 1u, *end = line + n;
    char *op = p;
    while (p < end && *p != ' ') p++;
    size_t opn = (size_t)(p - op);
    if (p < end) p++;
    /* Not config operations: they work without the store. */
    if (opn == 4 && memcmp(op, "data", 4) == 0) { cmd_data(p, end); return; }
    if (opn == 4 && memcmp(op, "push", 4) == 0) { cmd_push(p, end); return; }
    if (opn == 3 && memcmp(op, "end", 3) == 0 && p >= end) { cmd_end(); return; }
    if (opn == 5 && memcmp(op, "abort", 5) == 0 && p >= end) { fs_lfs_push_abort(); reply_s("ok", NULL); return; }
    if (!cfg_ok) { reply_s("err", "config store unavailable"); return; }
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
