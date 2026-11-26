/* The `\x02gogo-config` protocol (configsvc.h). */
#include "configsvc.h"
#include "vfs_int.h"

#include <string.h>

#define PUSH_TMP ".gogo-push"

void configsvc_init(ConfigSvc *s, const KvHal *kv, const ConfigBoard *board, const VfsBackend *flash,
                    void *flash_ctx) {
    memset(s, 0, sizeof *s);
    s->kv = kv;
    s->board = board;
    s->flash = flash;
    s->flash_ctx = flash_ctx;
}

uint32_t configsvc_crc32(uint32_t crc, const uint8_t *buf, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- replies ---- */

static void line_out(ConfigSvc *s, const char *tag, const char *text, size_t n) {
    const char *parts[5] = { "\x02", tag, " ", text, "\n" };
    size_t lens[5] = { 1, strlen(tag), 1, n, 1 };
    if (!text) {
        parts[2] = "\n";
        s->board->write(s->board->ctx, parts, lens, 3);
        return;
    }
    s->board->write(s->board->ctx, parts, lens, 5);
}

void configsvc_kv(ConfigSvc *s, const char *text, size_t n) { line_out(s, "kv", text, n); }
void configsvc_reply(ConfigSvc *s, const char *tag, const char *text, size_t n) { line_out(s, tag, text, n); }
static void reply_s(ConfigSvc *s, const char *tag, const char *text) {
    line_out(s, tag, text, text ? strlen(text) : 0);
}

/* ---- the store ---- */

static bool valid_key(const char *k, size_t n) {
    if (n == 0 || n > CONFIG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

int32_t configsvc_vm_get(const KvHal *kv, const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap) {
    char k[CONFIG_KEY_MAX + 1u];
    if (!kv || !valid_key((const char *)key, klen)) return -1;
    memcpy(k, key, klen);
    k[klen] = 0;
    if (strncmp(k, "wifi.", 5) == 0) return -1;   /* the store's alone (the VM checks too) */
    int32_t n = kv->get(kv->ctx, k, buf, cap);
    return n > (int32_t)CONFIG_VALUE_MAX ? -1 : n;
}

static void cmd_get(ConfigSvc *s, const char *key) {
    const KvHal *kv = s->kv;
    uint8_t *v = (uint8_t *)s->scratch;
    bool secret = strcmp(key, CONFIG_SECRET) == 0;   /* only its presence is read */
    int32_t n = kv->get(kv->ctx, key, secret ? NULL : v, secret ? 0u : CONFIG_VALUE_MAX);
    if (n < 0 || n > (int32_t)CONFIG_VALUE_MAX) { reply_s(s, "err", "not set"); return; }
    if (secret) { reply_s(s, "ok", "(set)"); return; }
    configsvc_reply(s, "ok", (const char *)v, (size_t)n);
}

static void list_one(void *ctx, const char *key) {
    ConfigSvc *s = ctx;
    const KvHal *kv = s->kv;
    char *line = s->scratch;
    size_t kl = strlen(key);
    if (kl > CONFIG_KEY_MAX) return;
    memcpy(line, key, kl);
    line[kl] = '=';
    size_t n = kl + 1u;
    if (strcmp(key, CONFIG_SECRET) == 0) {
        memcpy(line + n, "(set)", 5);
        n += 5u;
    } else {
        int32_t vl = kv->get(kv->ctx, key, (uint8_t *)line + n, CONFIG_VALUE_MAX);
        if (vl > 0) n += (size_t)(vl > (int32_t)CONFIG_VALUE_MAX ? (int32_t)CONFIG_VALUE_MAX : vl);
    }
    configsvc_kv(s, line, n);
}

static void cmd_list(ConfigSvc *s) {
    const char *e = s->kv->each(s->kv->ctx, list_one, s);
    if (e) reply_s(s, "err", e);
    else reply_s(s, "ok", NULL);
}

static void cmd_set(ConfigSvc *s, const char *key, const char *value, size_t vn) {
    if (vn > CONFIG_VALUE_MAX || memchr(value, '\x02', vn) || memchr(value, '\r', vn)) {
        reply_s(s, "err", "bad value");
        return;
    }
    const char *e = s->kv->set(s->kv->ctx, key, (const uint8_t *)value, (uint32_t)vn);
    if (e) { reply_s(s, "err", e); return; }
    reply_s(s, "ok", NULL);
    if (s->board->changed) s->board->changed(s->board->ctx, key);
}

static void cmd_rm(ConfigSvc *s, const char *key) {
    const char *e = s->kv->rm(s->kv->ctx, key);
    if (e) { reply_s(s, "err", e); return; }
    reply_s(s, "ok", NULL);
    if (s->board->changed) s->board->changed(s->board->ctx, key);
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
        int d = *p >= '0' && *p <= '9' ? *p - '0'
              : base == 16 && (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1;
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

static void push_drop(ConfigSvc *s) {
    if (!s->push.open) return;
    s->push.open = false;
    s->flash->close(s->flash_ctx, &s->push.f);
    s->flash->remove(s->flash_ctx, s->push.tmp);
}

/* rel's directory + PUSH_TMP into tmp. */
static void push_tmp_name(ConfigSvc *s, const char *rel) {
    const char *slash = strrchr(rel, '/');
    size_t d = slash ? (size_t)(slash - rel) + 1u : 0u;
    memcpy(s->push.tmp, rel, d);
    memcpy(s->push.tmp + d, PUSH_TMP, sizeof PUSH_TMP);
}

/* The backend works on paths inside the mount: "/" + rel. */
static int push_begin(ConfigSvc *s, const char *rel) {
    if (!s->flash) return GOGO_E_NOTMOUNTED;
    push_drop(s);
    push_tmp_name(s, rel);
    int e = s->flash->open(s->flash_ctx, &s->push.f, s->push.tmp, VFS_WRITE);
    s->push.open = e == GOGO_E_OK;
    s->push.len = s->push.crc = 0;
    return e;
}

static void cmd_push(ConfigSvc *s, char *p, char *end) {
    char *path = p;
    while (p < end && *p != ' ') p++;
    size_t pn = (size_t)(p - path);
    char *ls = p < end ? p + 1 : end, *le = ls;
    while (le < end && *le != ' ') le++;
    char *cs = le < end ? le + 1 : end;
    uint32_t first;
    static const char pre[] = "/flash";
    size_t pl = sizeof pre - 1u;
    if (pn <= pl + 1u || memcmp(path, pre, pl) != 0 || path[pl] != '/' ||
        vfs_path_check(path, (uint32_t)pn, &first) != GOGO_E_OK ||
        !parse_u32(ls, le, 10, &s->push.want_len) || !parse_u32(cs, end, 16, &s->push.want_crc) ||
        (pn >= 11u && memcmp(path + pn - 11u, "/" PUSH_TMP, 11) == 0)) {   /* the temporary file itself */
        if (s->flash) push_drop(s);
        reply_s(s, "err", "usage: push </flash/path> <len> <crc32 hex>");
        return;
    }
    /* "/dir/name.ggb": the path inside the mount */
    memcpy(s->push.rel, path + pl, pn - pl);
    s->push.rel[pn - pl] = 0;
    int e = push_begin(s, s->push.rel);
    if (e != GOGO_E_OK) { reply_s(s, "err", err_text(e)); return; }
    reply_s(s, "ok", "0");
}

static void cmd_data(ConfigSvc *s, char *p, char *end) {
    char *os = p;
    while (p < end && *p != ' ') p++;
    uint32_t off;
    uint8_t buf[CONFIG_PUSH_CHUNK];
    int n = p < end ? b64_decode(p + 1, end, buf, sizeof buf) : -1;
    if (!parse_u32(os, p, 10, &off) || n <= 0) {
        if (s->flash) push_drop(s);
        reply_s(s, "err", "bad data frame");
        return;
    }
    if (!s->push.open) { reply_s(s, "err", err_text(GOGO_E_BADARG)); return; }
    int e = s->flash->write(s->flash_ctx, &s->push.f, buf, (uint32_t)n);
    if (e != GOGO_E_OK) {
        push_drop(s);
        reply_s(s, "err", err_text(e));
        return;
    }
    s->push.crc = configsvc_crc32(s->push.crc, buf, (size_t)n);
    s->push.len += (uint32_t)n;
    /* A chunk at the wrong offset (a lost or repeated frame) ends the push. */
    if (s->push.len != off + (uint32_t)n) {
        push_drop(s);
        reply_s(s, "err", "data out of order");
        return;
    }
    char t[10];
    size_t tn = sizeof t;
    uint32_t v = s->push.len;
    do { t[--tn] = (char)('0' + v % 10u); v /= 10u; } while (v);
    configsvc_reply(s, "ok", t + tn, sizeof t - tn);
}

/* Closes the pushed file, reads it back, and renames it over the target
 * if it is the length and CRC-32 announced; otherwise removes it. */
static int push_end(ConfigSvc *s) {
    const VfsBackend *be = s->flash;
    void *c = s->flash_ctx;
    if (!s->push.open) return GOGO_E_BADARG;
    s->push.open = false;
    int e = be->close(c, &s->push.f);
    if (e == GOGO_E_OK && (s->push.len != s->push.want_len || s->push.crc != s->push.want_crc)) e = GOGO_E_IO;
    if (e == GOGO_E_OK) {
        e = be->open(c, &s->push.f, s->push.tmp, VFS_READ);
        if (e == GOGO_E_OK) {
            uint8_t b[64];
            uint32_t got = 0, crc = 0, k = 0;
            while ((e = be->read(c, &s->push.f, b, sizeof b, &k)) == GOGO_E_OK && k > 0) {
                crc = configsvc_crc32(crc, b, k);
                got += k;
            }
            be->close(c, &s->push.f);
            if (e == GOGO_E_OK && (got != s->push.want_len || crc != s->push.want_crc)) e = GOGO_E_IO;
        }
    }
    if (e == GOGO_E_OK) e = be->rename(c, s->push.tmp, s->push.rel, true);
    if (e != GOGO_E_OK) be->remove(c, s->push.tmp);
    return e;
}

static void cmd_end(ConfigSvc *s) {
    int e = s->flash ? push_end(s) : GOGO_E_BADARG;
    if (e != GOGO_E_OK) { reply_s(s, "err", err_text(e)); return; }
    reply_s(s, "ok", NULL);
}

/* ---- requests ---- */

static bool is(const char *op, size_t opn, const char *name) { return opn == strlen(name) && memcmp(op, name, opn) == 0; }

static void board_cmd(ConfigSvc *s, void (*hook)(ConfigSvc *), const char *none) {
    if (hook) hook(s);
    else reply_s(s, "err", none);
}

/* line: the frame after \x02, without the \n (it may be written). */
static void handle(ConfigSvc *s, char *line, size_t n) {
    static const char magic[] = "gogo-config ";
    if (n < sizeof magic - 1u || memcmp(line, magic, sizeof magic - 1u) != 0) return;  /* not ours */
    char *p = line + sizeof magic - 1u, *end = line + n;
    char *op = p;
    while (p < end && *p != ' ') p++;
    size_t opn = (size_t)(p - op);
    if (p < end) p++;
    bool bare = p >= end;
    /* Not config operations: they work without the store. */
    if (is(op, opn, "data")) { cmd_data(s, p, end); return; }
    if (is(op, opn, "push")) { cmd_push(s, p, end); return; }
    if (is(op, opn, "end") && bare) { cmd_end(s); return; }
    if (is(op, opn, "abort") && bare) {
        if (s->flash) push_drop(s);
        reply_s(s, "ok", NULL);
        return;
    }
    if (is(op, opn, "format-flash") && bare) { board_cmd(s, s->board->format_flash, "no /flash on this board"); return; }
    if (!s->kv) { reply_s(s, "err", "config store unavailable"); return; }
    if (is(op, opn, "list") && bare) { cmd_list(s); return; }
    if (is(op, opn, "scan") && bare) { board_cmd(s, s->board->scan, "no network in this firmware"); return; }
    if (is(op, opn, "stats") && bare) { board_cmd(s, s->board->stats, "no stats on this board"); return; }
    char *key = p;
    while (p < end && *p != ' ') p++;
    size_t kn = (size_t)(p - key);
    bool more = p < end;
    if (more) *p++ = 0;
    else *end = 0;
    if (!valid_key(key, kn)) { reply_s(s, "err", "bad key"); return; }
    key[kn] = 0;
    if (is(op, opn, "set") && more) cmd_set(s, key, p, (size_t)(end - p));
    else if (is(op, opn, "get") && !more) cmd_get(s, key);
    else if (is(op, opn, "rm") && !more) cmd_rm(s, key);
    else reply_s(s, "err", "usage: set <key> <value> | get <key> | rm <key> | list | scan");
}

void configsvc_feed(ConfigSvc *s, const uint8_t *buf, size_t got) {
    for (size_t i = 0; i < got; i++) {
        char c = (char)buf[i];
        if (c == '\x02') { s->in = true; s->n = 0; s->over = false; memset(s->line, 0, sizeof s->line); continue; }
        if (!s->in) continue;
        if (c == '\n') {
            s->in = false;
            size_t n = s->n;
            if (n > 0 && s->line[n - 1] == '\r') n--;
            if (s->over) reply_s(s, "err", "line too long");
            else handle(s, s->line, n);
            memset(s->line, 0, sizeof s->line);   /* a set may have carried the password */
            memset(s->scratch, 0, sizeof s->scratch);
            continue;
        }
        if (s->n < CONFIG_LINE_MAX) s->line[s->n++] = c;
        else s->over = true;
    }
}
