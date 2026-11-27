/* The device configuration service: the `\x02gogo-config` line protocol
 * that `gogo config`, `gogo device` and `gogo push` speak over a board's
 * console, implemented once over the board's key-value store (KvHal) and,
 * for push, its /flash backend. Pure C99, no malloc; the state is the
 * board's (a ConfigSvc in static storage). One thread (the board's console
 * reader) feeds it bytes.
 *
 * Requests, one per line:
 *
 *     \x02gogo-config set <key> <value>\n    (value: the rest of the line)
 *     \x02gogo-config get <key>\n
 *     \x02gogo-config rm <key>\n
 *     \x02gogo-config list\n
 *     \x02gogo-config scan\n                 (nearby Wi-Fi networks)
 *     \x02gogo-config stats\n                (board counters: memory, tables, net)
 *     \x02gogo-config info\n                 (the firmware: version, board, image format)
 *     \x02gogo-config format-flash\n         (format /flash: the board restarts)
 *     \x02gogo-config push <path> <len> <crc32>\n   (gogo push: write a file
 *     \x02gogo-config data <offset> <base64>\n       to /flash, in chunks; push
 *     \x02gogo-config end\n                          and data answer ok <bytes
 *     \x02gogo-config abort\n                        so far>)
 *
 * Replies: zero or more `\x02kv <text>\n` lines and then one
 * `\x02ok[ <text>]\n` or `\x02err <reason>\n`. Input outside a frame is
 * ignored. Every reply line starts with \x02 so the host can pick it out
 * of program output, which may be mid-line. The Wi-Fi password is never
 * sent back: get and list show "(set)". Keys are 1..15 bytes of
 * `a-z A-Z 0-9 . _ -`, values at most CONFIG_VALUE_MAX bytes without \x02
 * or \r.
 *
 * info answers one line, `\x02ok fw=<version> board=<name> image=<major>.<minor>
 * actor_memory=<size> memory=<size> max_actors=<n> max_ports=<n>
 * max_links=<n> code_mem=<size> bytes_handles=<n>/<n> provides=<name>,<name>...`:
 * the firmware's version (`git describe`, "dev" when unknown), the board
 * it was built for, the image format it runs (major.0 to major.minor),
 * the VM's parameters as it runs with them (ConfigInfo.params;
 * sizes in bytes or with a K, M or G suffix, memory "unlimited" without a
 * limit) and the requirement names it provides (port drivers, mount
 * prefixes, "float"). A field the board does not know is left out;
 * values never hold spaces. It needs no store. A firmware older
 * than info answers it `\x02err bad key` (as any unknown bare word).
 *
 * push writes a file to /flash: <path> is an absolute /flash path, <len>
 * its size and <crc32> the CRC-32 (IEEE) of its bytes in hex. The bytes
 * follow in data frames (base64, CONFIG_PUSH_CHUNK bytes at most, <offset>
 * the count sent before it), each answered before the next is sent, so a
 * console's small input buffer never overflows; the file is written under
 * a temporary name (.gogo-push) beside the target, and end reads it back,
 * checks it against <len> and <crc32> and only then renames it over
 * <path>. A failure, abort or a new push drops the temporary file. The
 * backend must take its own lock when the VM uses it too; each backend
 * call is atomic then, but the sequence (write, close, read back, rename)
 * is not: the VM may use the volume between two calls. */
#ifndef GOGO_CONFIGSVC_H
#define GOGO_CONFIGSVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"

#define CONFIG_KEY_MAX    15u      /* NVS's key limit (16 with the NUL) */
#define CONFIG_VALUE_MAX  1000u    /* as cmd/gogo/config.go */
#define CONFIG_PUSH_CHUNK 128u     /* as cmd/gogo/push.go */
#define CONFIG_LINE_MAX   (12u + 5u + CONFIG_KEY_MAX + CONFIG_VALUE_MAX + 4u)
#define CONFIG_SECRET     "wifi.password"

/* A board's key-value store. Keys are NUL-terminated and valid (above).
 *  - get: the value of key into buf (at most cap bytes; buf NULL or cap 0:
 *    only the length); its full length, or -1 if unset. Any thread.
 *  - set / rm: NULL on success, else a short reason for the console; rm
 *    of a key that is not set returns "not set".
 *  - each: calls cb with every key, in any order; NULL or a reason. */
typedef struct KvHal {
    void *ctx;
    int32_t (*get)(void *ctx, const char *key, uint8_t *buf, uint32_t cap);
    const char *(*set)(void *ctx, const char *key, const uint8_t *value, uint32_t n);
    const char *(*rm)(void *ctx, const char *key);
    const char *(*each)(void *ctx, void (*cb)(void *cb_ctx, const char *key), void *cb_ctx);
} KvHal;

typedef struct ConfigSvc ConfigSvc;

/* The board's identity, for info. NULL / 0 fields are left out. */
typedef struct ConfigInfo {
    const char *fw;                 /* firmware version */
    const char *board;              /* board name (sticks3, cardputer) */
    uint32_t image_major, image_minor;   /* image formats it runs (major 0: unknown) */
    const char *const *provides;    /* requirement names, NULL-terminated */
    /* The VM's parameters as name=value fields separated by single spaces
     * (vm_params_format, runtime/board/params.h), NUL-terminated;
     * empty: none. Here, not a pointer, so the board keeps no static copy. */
    char params[160];
} ConfigInfo;

/* The board's side, besides the store. Every hook is optional (NULL: the
 * request answers that the board cannot). Hooks get the service, so they
 * can answer with configsvc_kv / configsvc_reply; each must end with one
 * configsvc_reply (ok or err).
 *  - write: puts the parts, in order, on the console as one line that
 *    nothing else interleaves (required).
 *  - changed: key was set or removed (the ESP reconnects Wi-Fi on wifi.*).
 *  - scan, stats: the board's reports, as kv lines.
 *  - info: fills in the board's identity (the service writes the reply).
 *  - format_flash: replies, then formats /flash (the ESP restarts). */
typedef struct ConfigBoard {
    void *ctx;
    void (*write)(void *ctx, const char *const *parts, const size_t *lens, int nparts);
    void (*changed)(void *ctx, const char *key);
    void (*scan)(ConfigSvc *s);
    void (*stats)(ConfigSvc *s);
    void (*format_flash)(ConfigSvc *s);
    void (*info)(void *ctx, ConfigInfo *out);
} ConfigBoard;

/* The service's state: zeroed static storage, set up by configsvc_init. */
struct ConfigSvc {
    const KvHal *kv;            /* NULL: no store (every config request is an error) */
    const ConfigBoard *board;
    const VfsBackend *flash;    /* /flash's backend and volume, for push (NULL: none) */
    void *flash_ctx;
    char line[CONFIG_LINE_MAX + 1u];
    size_t n;
    bool in, over;
    char scratch[CONFIG_KEY_MAX + 2u + CONFIG_VALUE_MAX];
    struct {
        bool open;
        union { void *p; uint32_t u; uint8_t b[GOGO_FS_SLOT_BYTES]; } f;   /* a vfs slot's storage */
        char rel[VFS_PATH_MAX];          /* the target, inside /flash ("dir/name.ggb") */
        char tmp[VFS_PATH_MAX + 16u];    /* its temporary name */
        uint32_t want_len, want_crc, len, crc;
    } push;
};

void configsvc_init(ConfigSvc *s, const KvHal *kv, const ConfigBoard *board, const VfsBackend *flash,
                    void *flash_ctx);
/* Bytes from the console; whole request lines are handled as they end. */
void configsvc_feed(ConfigSvc *s, const uint8_t *buf, size_t n);
/* For the board's hooks: one `\x02kv <text>` line, and the final reply
 * (`\x02<tag>[ <text>]`, tag "ok" or "err"; text NULL: none). */
void configsvc_kv(ConfigSvc *s, const char *text, size_t n);
void configsvc_reply(ConfigSvc *s, const char *tag, const char *text, size_t n);

/* sys.Config for a VM (gogo_host.config_get): key (klen bytes) from kv,
 * -1 if unset, invalid, or under "wifi." (the store's alone). */
int32_t configsvc_vm_get(const KvHal *kv, const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap);

/* CRC-32 (IEEE, as zlib and Go's hash/crc32): crc of the bytes so far
 * (0 to start), updated with buf. */
uint32_t configsvc_crc32(uint32_t crc, const uint8_t *buf, size_t n);

#endif
