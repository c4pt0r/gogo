/* The gogo-config protocol against an in-memory store and the memfs
 * backend as /flash: framing, the store commands and the secret, the
 * board hooks, and gogo push (CRC, order, abort, the temporary name). */
#include "test.h"
#include "configsvc.h"
#include "memfs.h"
#include <stdio.h>
#include <string.h>

void test_host_arena_forget(void);
void test_host_arena_forget(void) {}   /* test_main.c calls it; no VM here */

/* ---- an in-memory store ---- */

#define NKEYS 8
static struct { char k[CONFIG_KEY_MAX + 1]; uint8_t v[CONFIG_VALUE_MAX]; uint32_t n; bool used; } store[NKEYS];

static int find(const char *k) {
    for (int i = 0; i < NKEYS; i++) if (store[i].used && !strcmp(store[i].k, k)) return i;
    return -1;
}
static int32_t m_get(void *ctx, const char *k, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    int i = find(k);
    if (i < 0) return -1;
    if (buf && cap) memcpy(buf, store[i].v, store[i].n < cap ? store[i].n : cap);
    return (int32_t)store[i].n;
}
static const char *m_set(void *ctx, const char *k, const uint8_t *v, uint32_t n) {
    (void)ctx;
    int i = find(k);
    for (int j = 0; i < 0 && j < NKEYS; j++) if (!store[j].used) i = j;
    if (i < 0) return "store full";
    store[i].used = true;
    strcpy(store[i].k, k);
    memcpy(store[i].v, v, n);
    store[i].n = n;
    return NULL;
}
static const char *m_rm(void *ctx, const char *k) {
    (void)ctx;
    int i = find(k);
    if (i < 0) return "not set";
    store[i].used = false;
    return NULL;
}
static const char *m_each(void *ctx, void (*cb)(void *, const char *), void *c) {
    (void)ctx;
    for (int i = 0; i < NKEYS; i++) if (store[i].used) cb(c, store[i].k);
    return NULL;
}
static const KvHal kv = { .get = m_get, .set = m_set, .rm = m_rm, .each = m_each };

/* ---- the board: output into a buffer ---- */

static char out[8192];
static size_t outn;
static char changed[32];
static void b_write(void *ctx, const char *const *parts, const size_t *lens, int n) {
    (void)ctx;
    for (int i = 0; i < n; i++) {
        CHECK(outn + lens[i] < sizeof out);
        memcpy(out + outn, parts[i], lens[i]);
        outn += lens[i];
    }
    out[outn] = 0;
}
static void b_changed(void *ctx, const char *key) { (void)ctx; snprintf(changed, sizeof changed, "%s", key); }
static void b_stats(ConfigSvc *s) {
    configsvc_kv(s, "heap_free=1", 11);
    configsvc_reply(s, "ok", NULL, 0);
}
static const ConfigBoard board = { .write = b_write, .changed = b_changed, .stats = b_stats };

static ConfigSvc svc;

static void setup(void) {
    memset(store, 0, sizeof store);
    outn = 0;
    out[0] = 0;
    changed[0] = 0;
    memfs_init(0, false, true, -1);
    configsvc_init(&svc, &kv, &board, &memfs_be[0], memfs_ctx(0));
}

/* Sends one request line; returns the replies written for it. */
static const char *req(const char *line) {
    char b[CONFIG_LINE_MAX + 16];
    int n = snprintf(b, sizeof b, "\x02gogo-config %s\n", line);
    outn = 0;
    out[0] = 0;
    configsvc_feed(&svc, (const uint8_t *)b, (size_t)n);
    return out;
}

TEST(configsvc_set_get_rm_list) {
    setup();
    CHECK(!strcmp(req("get net.ntp"), "\x02" "err not set\n"));
    CHECK(!strcmp(req("set net.ntp pool.ntp.org"), "\x02ok\n"));
    CHECK(!strcmp(changed, "net.ntp"));
    CHECK(!strcmp(req("get net.ntp"), "\x02ok pool.ntp.org\n"));
    CHECK(!strcmp(req("set wifi.password hunter2"), "\x02ok\n"));
    CHECK(!strcmp(req("get wifi.password"), "\x02ok (set)\n"));   /* never sent back */
    CHECK(!strcmp(req("list"), "\x02kv net.ntp=pool.ntp.org\n\x02kv wifi.password=(set)\n\x02ok\n"));
    CHECK(strstr(out, "hunter2") == NULL);
    CHECK(!strcmp(req("rm net.ntp"), "\x02ok\n"));
    CHECK(!strcmp(req("rm net.ntp"), "\x02" "err not set\n"));
    CHECK(!strcmp(req("get bad/key"), "\x02" "err bad key\n"));
    CHECK(!strcmp(req("stats"), "\x02kv heap_free=1\n\x02ok\n"));
    CHECK(!strcmp(req("scan"), "\x02" "err no network in this firmware\n"));
    CHECK(!strcmp(req("format-flash"), "\x02" "err no /flash on this board\n"));
}

TEST(configsvc_framing) {
    setup();
    /* noise outside a frame, a frame split across feeds, a \r\n ending */
    const char *a = "garbage\n\x02gogo-con", *b = "fig set a.b 1\r\n";
    outn = 0;
    configsvc_feed(&svc, (const uint8_t *)a, strlen(a));
    CHECK_EQ(outn, 0);
    configsvc_feed(&svc, (const uint8_t *)b, strlen(b));
    CHECK(!strcmp(out, "\x02ok\n"));
    CHECK(!strcmp(req("get a.b"), "\x02ok 1\n"));
    /* another protocol's frame is ignored */
    outn = 0;
    configsvc_feed(&svc, (const uint8_t *)"\x02other thing\n", 13);
    CHECK_EQ(outn, 0);
    /* a store that is unavailable: config requests fail, push still works */
    configsvc_init(&svc, NULL, &board, &memfs_be[0], memfs_ctx(0));
    CHECK(!strcmp(req("get a.b"), "\x02" "err config store unavailable\n"));
}

/* info: the board's identity, one ok line; works without a store. */
static const char *const provides[] = { "gpio", "udp", "/flash", "float", NULL };
static ConfigInfo info_of;
static void b_info(void *ctx, ConfigInfo *out) { (void)ctx; *out = info_of; }

TEST(configsvc_info) {
    static const ConfigBoard ib = { .write = b_write, .info = b_info };
    setup();
    /* a board without the hook */
    CHECK(!strcmp(req("info"), "\x02" "err no info on this board\n"));
    configsvc_init(&svc, &kv, &ib, NULL, NULL);
    info_of = (ConfigInfo){ .fw = "v0.2.0-3-gabc1234", .board = "sticks3", .image_major = 15, .image_minor = 6,
                            .provides = provides };
    CHECK(!strcmp(req("info"), "\x02ok fw=v0.2.0-3-gabc1234 board=sticks3 image=15.6 provides=gpio,udp,/flash,float\n"));
    /* fields the board does not know are left out; spaces never reach the line */
    /* the VM's parameters, field by field, before provides */
    strcpy(info_of.params, "actor_memory=16K memory=160K max_actors=1024 max_ports=16 max_links=8 code_mem=64K bytes_handles=64/16");
    CHECK(!strcmp(req("info"), "\x02ok fw=v0.2.0-3-gabc1234 board=sticks3 image=15.6 actor_memory=16K memory=160K "
                               "max_actors=1024 max_ports=16 max_links=8 code_mem=64K bytes_handles=64/16 "
                               "provides=gpio,udp,/flash,float\n"));
    info_of = (ConfigInfo){ .fw = "my build", .image_major = 1 };
    CHECK(!strcmp(req("info"), "\x02ok fw=my_build image=1.0\n"));
    info_of = (ConfigInfo){ 0 };
    CHECK(!strcmp(req("info"), "\x02ok\n"));
    /* no store needed; info with an argument is not info */
    configsvc_init(&svc, NULL, &ib, NULL, NULL);
    info_of = (ConfigInfo){ .fw = "dev", .board = "cardputer" };
    CHECK(!strcmp(req("info"), "\x02ok fw=dev board=cardputer\n"));
    CHECK(!strcmp(req("info x"), "\x02" "err config store unavailable\n"));
    configsvc_init(&svc, &kv, &ib, NULL, NULL);
    CHECK(!strcmp(req("info x"), "\x02" "err usage: set <key> <value> | get <key> | rm <key> | list | scan | stats | info\n"));
}

/* How a firmware without info (v0.1.0's handler, and this one's for any
 * unknown bare word) answers: `gogo setup` and `gogo device info` take
 * this as "unknown". */
TEST(configsvc_unknown_word) {
    setup();
    CHECK(!strcmp(req("nosuchop"), "\x02" "err bad key\n"));
}

TEST(configsvc_vm_get) {
    setup();
    uint8_t b[16];
    req("set net.ntp x");
    req("set wifi.ssid home");
    CHECK_EQ(configsvc_vm_get(&kv, (const uint8_t *)"net.ntp", 7, b, sizeof b), 1);
    CHECK_EQ(configsvc_vm_get(&kv, (const uint8_t *)"wifi.ssid", 9, b, sizeof b), -1);
    CHECK_EQ(configsvc_vm_get(&kv, (const uint8_t *)"no key", 6, b, sizeof b), -1);
}

TEST(configsvc_crc32) {
    CHECK_EQ(configsvc_crc32(0, (const uint8_t *)"123456789", 9), 0xCBF43926u);
    uint32_t c = configsvc_crc32(0, (const uint8_t *)"1234", 4);
    CHECK_EQ(configsvc_crc32(c, (const uint8_t *)"56789", 5), 0xCBF43926u);
}

static const char *pushdata(const char *b64, unsigned off) {
    char l[256];
    snprintf(l, sizeof l, "data %u %s", off, b64);
    return req(l);
}

TEST(configsvc_push) {
    setup();
    char l[128];
    uint32_t crc = configsvc_crc32(0, (const uint8_t *)"hello world", 11);
    snprintf(l, sizeof l, "push /flash/app.ggb 11 %x", (unsigned)crc);
    CHECK(!strcmp(req(l), "\x02ok 0\n"));
    CHECK(!strcmp(pushdata("aGVsbG8g", 0), "\x02ok 6\n"));       /* "hello " */
    CHECK(!strcmp(pushdata("d29ybGQ=", 6), "\x02ok 11\n"));      /* "world" */
    CHECK(!strcmp(req("end"), "\x02ok\n"));
    int32_t size;
    bool dir;
    CHECK_EQ(memfs_be[0].stat(memfs_ctx(0), "/app.ggb", &size, &dir), GOGO_E_OK);
    CHECK_EQ(size, 11);
    CHECK(memfs_be[0].stat(memfs_ctx(0), "/.gogo-push", &size, &dir) == GOGO_E_NOTFOUND);

    /* a wrong CRC: nothing replaced, the temporary file gone */
    snprintf(l, sizeof l, "push /flash/app.ggb 11 %x", (unsigned)(crc ^ 1u));
    CHECK(!strcmp(req(l), "\x02ok 0\n"));
    pushdata("aGVsbG8g", 0);
    pushdata("d29ybGQ=", 6);
    CHECK(!strcmp(req("end"), "\x02" "err the stored file does not match (length or CRC)\n"));
    CHECK(memfs_be[0].stat(memfs_ctx(0), "/.gogo-push", &size, &dir) == GOGO_E_NOTFOUND);
    CHECK_EQ(memfs_be[0].stat(memfs_ctx(0), "/app.ggb", &size, &dir), GOGO_E_OK);

    /* a chunk out of order ends the push */
    snprintf(l, sizeof l, "push /flash/b.ggb 11 %x", (unsigned)crc);
    req(l);
    CHECK(!strcmp(pushdata("aGVsbG8g", 3), "\x02" "err data out of order\n"));
    CHECK(!strcmp(req("end"), "\x02" "err no push in progress\n"));
    CHECK(memfs_be[0].stat(memfs_ctx(0), "/.gogo-push", &size, &dir) == GOGO_E_NOTFOUND);

    /* abort, bad paths, the temporary name itself */
    req(l);
    CHECK(!strcmp(req("abort"), "\x02ok\n"));
    CHECK(memfs_be[0].stat(memfs_ctx(0), "/.gogo-push", &size, &dir) == GOGO_E_NOTFOUND);
    CHECK(!strcmp(req("push /sd/x 1 0"), "\x02" "err usage: push </flash/path> <len> <crc32 hex>\n"));
    CHECK(!strcmp(req("push /flash/.gogo-push 1 0"), "\x02" "err usage: push </flash/path> <len> <crc32 hex>\n"));
    CHECK(!strcmp(req("push /flash/nodir/x.ggb 1 0"), "\x02" "err no such directory\n"));
    CHECK(!strcmp(req("data 0 !!"), "\x02" "err bad data frame\n"));
}
