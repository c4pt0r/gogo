/* The "ble" port on gogovm (ble_unix.h): the radio is a Unix-domain
 * socket. Opening the port listens on it ("advertising"); a client that
 * connects plays the phone, one at a time, over a line protocol:
 *
 *   gogovm -> phone, on accept:  adv NAME
 *                                svc UUID              (per service)
 *                                chr K UUID FLAGS      (per characteristic, K from 0)
 *                                end
 *   phone -> gogovm:  connect PEER MTU   the connection (Connected); first
 *                     mtu N              the ATT MTU changed (MTUChanged)
 *                     read K             answered: value K HEX | error K CODE
 *                     write K HEX        answered: ok K | error K CODE (Written)
 *                     writenr K HEX      no answer (Written)
 *                     sub K 0|1          answered: ok K | error K CODE (Subscribed)
 *                     drop               the link is lost (reason 0x08), closed
 *                     (end of file)      the phone disconnected (reason 0x13)
 *   gogovm -> phone:  notify K HEX       a SendNotify
 *                     busy               to a second phone (then closed)
 *
 * UUIDs as gogo/ble writes them (4 hex digits or the 36-character form),
 * data as lower-case hex (- for none), CODE an ATT error: 2 read not
 * permitted, 3 write not permitted, 6 not supported (no Notify), 13 too
 * long, 17 insufficient resources (the event queue is full). Port close
 * closes the phone's socket and the listener (and removes the file).
 * tests/ble is the phone of the tests.
 *
 * The socket's thread is the radio side: it queues events with ble_post
 * under `mu` and wakes the VM, and answers reads from the values itself,
 * as NimBLE's task does on the board. */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "ble_unix.h"
#include "platform_unix.h"
#include "gogo_errors.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static BleBoard board;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;    /* board's queue, the values, conn */
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;   /* writes to the phone's socket */

static struct {
    BleConfig cfg;                   /* the thread's copy (start) */
    uint8_t *val[BLE_MAX_CHARS];     /* the values (malloc), NULL: empty */
    uint16_t vlen[BLE_MAX_CHARS];
    int lfd, cfd;                    /* listener, phone (-1: none) */
    int stop[2];                     /* the thread's stop pipe */
    bool connected;                  /* the phone sent connect (mu) */
    bool running;
    pthread_t th;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} u = { .lfd = -1, .cfd = -1, .stop = { -1, -1 } };

static void hal_lock(void *ctx) { (void)ctx; pthread_mutex_lock(&mu); }
static void hal_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&mu); }

/* ---- the phone's socket ---- */

/* Writes all of s (n bytes) to fd, under wmu; false if the phone is gone. */
static bool write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        s += w;
        n -= (size_t)w;
    }
    return n == 0;
}

static bool send_line(int fd, const char *s, size_t n) {
    pthread_mutex_lock(&wmu);
    bool ok = write_all(fd, s, n);
    pthread_mutex_unlock(&wmu);
    return ok;
}

static size_t put_hex(char *o, const uint8_t *d, uint32_t len) {
    static const char hex[] = "0123456789abcdef";
    if (!len) {
        o[0] = '-';
        return 1;
    }
    for (uint32_t i = 0; i < len; i++) {
        o[2 * i] = hex[d[i] >> 4];
        o[2 * i + 1] = hex[d[i] & 15];
    }
    return 2u * len;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* s as bytes into out (cap); the count, or -1 (odd, not hex, too long). */
static int32_t get_hex(const char *s, uint8_t *out, uint32_t cap) {
    if (!strcmp(s, "-")) return 0;
    size_t n = strlen(s);
    if (n % 2u || n / 2u > cap) return -1;
    for (size_t i = 0; i < n; i += 2u) {
        int a = hex_digit(s[i]), b = hex_digit(s[i + 1u]);
        if (a < 0 || b < 0) return -1;
        out[i / 2u] = (uint8_t)(a << 4 | b);
    }
    return (int32_t)(n / 2u);
}

/* A UUID as gogo/ble writes it. */
static void uuid_text(const BleUuid *x, char *o) {
    if (x->len == 2u) {
        snprintf(o, 8, "%02x%02x", x->b[1], x->b[0]);
        return;
    }
    size_t k = 0;
    for (int i = 15; i >= 0; i--) {
        if (i == 11 || i == 9 || i == 7 || i == 5) o[k++] = '-';
        k += (size_t)snprintf(o + k, 3, "%02x", x->b[i]);
    }
    o[k] = 0;
}

static void say_gatt(int fd) {
    char line[96];
    int n = snprintf(line, sizeof line, "adv %s\n", u.cfg.name);
    send_line(fd, line, (size_t)n);
    for (uint32_t s = 0; s < u.cfg.nsvc; s++) {
        char t[40];
        uuid_text(&u.cfg.svc[s], t);
        n = snprintf(line, sizeof line, "svc %s\n", t);
        send_line(fd, line, (size_t)n);
        for (uint32_t k = u.cfg.first[s]; k < u.cfg.first[s + 1u]; k++) {
            uuid_text(&u.cfg.chr[k], t);
            n = snprintf(line, sizeof line, "chr %u %s %u\n", (unsigned)k, t, (unsigned)u.cfg.flags[k]);
            send_line(fd, line, (size_t)n);
        }
    }
    send_line(fd, "end\n", 4);
}

/* Queues an event and wakes the VM; false if it did not fit. */
static bool post(uint32_t ev, uint32_t arg, const uint8_t *d, uint32_t len) {
    pthread_mutex_lock(&mu);
    bool ok = ble_post(&board, ev, arg, d, len);
    pthread_mutex_unlock(&mu);
    if (ok) platform_notify();
    return ok;
}

static void reply(int fd, const char *what, unsigned k, int code) {
    char line[48];
    int n = code ? snprintf(line, sizeof line, "error %u %d\n", k, code) : snprintf(line, sizeof line, "%s %u\n", what, k);
    send_line(fd, line, (size_t)n);
}

/* One line from the phone; false: drop the connection (reason in *why). */
static bool handle(int fd, char *line, uint32_t *why) {
    static uint8_t buf[BLE_VALUE_MAX];
    char *argv[4] = { NULL, NULL, NULL, NULL };
    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " ", &save); t && argc < 4; t = strtok_r(NULL, " ", &save)) argv[argc++] = t;
    if (!argc) return true;
    pthread_mutex_lock(&mu);
    bool conn = u.connected;
    pthread_mutex_unlock(&mu);
    if (!strcmp(argv[0], "connect") && argc == 3 && !conn) {
        unsigned mtu = (unsigned)strtoul(argv[2], NULL, 10);
        pthread_mutex_lock(&mu);
        u.connected = true;
        pthread_mutex_unlock(&mu);
        post(BLE_EV_CONNECTED, mtu, (const uint8_t *)argv[1], (uint32_t)strnlen(argv[1], BLE_PEER_MAX));
        return true;
    }
    if (!conn) return true;                        /* nothing but connect before it */
    if (!strcmp(argv[0], "drop")) {
        *why = 0x08;
        return false;
    }
    if (!strcmp(argv[0], "mtu") && argc == 2) {
        post(BLE_EV_MTU, (uint32_t)strtoul(argv[1], NULL, 10), NULL, 0);
        return true;
    }
    if (argc < 2) return true;
    unsigned k = (unsigned)strtoul(argv[1], NULL, 10);
    uint8_t f = k < u.cfg.nchr ? u.cfg.flags[k] : 0;
    if (!strcmp(argv[0], "read")) {
        if (!(f & BLE_F_READ)) {
            reply(fd, "", k, 2);
            return true;
        }
        static char out[32 + 2u * BLE_VALUE_MAX];
        pthread_mutex_lock(&mu);
        size_t n = (size_t)snprintf(out, 32, "value %u ", k);
        n += put_hex(out + n, u.val[k], u.vlen[k]);
        pthread_mutex_unlock(&mu);
        out[n++] = '\n';
        send_line(fd, out, n);
        return true;
    }
    if ((!strcmp(argv[0], "write") || !strcmp(argv[0], "writenr")) && argc == 3) {
        bool resp = argv[0][5] == 0;
        int32_t n = get_hex(argv[2], buf, sizeof buf);
        int code = !(f & (resp ? BLE_F_WRITE : BLE_F_WRITE_NR)) ? 3 : n < 0 ? 13 : 0;
        /* The response goes out before anything the program does about
         * the write (a notify takes wmu too), so a phone sees them in
         * that order, as on air. */
        pthread_mutex_lock(&wmu);
        if (!code && !post(BLE_EV_WRITTEN, k, buf, (uint32_t)n)) code = 17;
        if (resp) {
            char line[48];
            int m = code ? snprintf(line, sizeof line, "error %u %d\n", k, code) : snprintf(line, sizeof line, "ok %u\n", k);
            (void)write_all(fd, line, (size_t)m);
        }
        pthread_mutex_unlock(&wmu);
        return true;
    }
    if (!strcmp(argv[0], "sub") && argc == 3) {
        if (!(f & BLE_F_NOTIFY)) {
            reply(fd, "", k, 6);
            return true;
        }
        char ok[24];
        int m = snprintf(ok, sizeof ok, "ok %u\n", k);
        pthread_mutex_lock(&wmu);
        post(BLE_EV_SUBSCRIBED, k | (argv[2][0] == '1' ? 1u << 8 : 0u), NULL, 0);
        (void)write_all(fd, ok, (size_t)m);
        pthread_mutex_unlock(&wmu);
        return true;
    }
    return true;
}

/* Ends the phone's connection (reason why, if it had connected). */
static void hang_up(uint32_t why) {
    pthread_mutex_lock(&mu);
    bool conn = u.connected;
    u.connected = false;
    int fd = u.cfd;
    u.cfd = -1;
    pthread_mutex_unlock(&mu);
    if (fd >= 0) {
        pthread_mutex_lock(&wmu);   /* no notify is writing to it */
        close(fd);
        pthread_mutex_unlock(&wmu);
    }
    if (conn) post(BLE_EV_DISCONNECTED, why, NULL, 0);
}

static void *thread_main(void *arg) {
    (void)arg;
    static char line[64 + 2u * BLE_VALUE_MAX];
    size_t fill = 0;
    for (;;) {
        struct pollfd p[3] = { { u.stop[0], POLLIN, 0 }, { u.lfd, POLLIN, 0 }, { -1, POLLIN, 0 } };
        pthread_mutex_lock(&mu);
        p[2].fd = u.cfd;
        pthread_mutex_unlock(&mu);
        if (poll(p, 3, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents) break;
        if (p[1].revents & POLLIN) {
            int fd = accept(u.lfd, NULL, NULL);
            if (fd >= 0) {
                pthread_mutex_lock(&mu);
                bool taken = u.cfd >= 0;
                if (!taken) u.cfd = fd;
                pthread_mutex_unlock(&mu);
                if (taken) {
                    send_line(fd, "busy\n", 5);
                    close(fd);
                } else {
                    fill = 0;
                    say_gatt(fd);
                }
            }
        }
        if (p[2].fd >= 0 && p[2].revents) {
            ssize_t r = read(p[2].fd, line + fill, sizeof line - 1u - fill);
            if (r <= 0) {
                if (r < 0 && errno == EINTR) continue;
                hang_up(0x13);
                continue;
            }
            fill += (size_t)r;
            char *nl;
            uint32_t why = 0;
            bool keep = true;
            while (keep && (nl = memchr(line, '\n', fill)) != NULL) {
                *nl = 0;
                keep = handle(p[2].fd, line, &why);
                size_t used = (size_t)(nl - line) + 1u;
                memmove(line, nl + 1, fill - used);
                fill -= used;
            }
            if (!keep || fill == sizeof line - 1u) hang_up(keep ? 0x13 : why);
        }
    }
    return NULL;
}

/* ---- the HAL ---- */

static void drop_values(void) {
    for (uint32_t k = 0; k < BLE_MAX_CHARS; k++) {
        free(u.val[k]);
        u.val[k] = NULL;
        u.vlen[k] = 0;
    }
}

static int hal_start(void *ctx, const BleConfig *cfg) {
    (void)ctx;
    const char *path = getenv("GOGO_BLE");
    char def[sizeof u.path];
    if (!path || !*path) {
        const char *tmp = getenv("TMPDIR");
        snprintf(def, sizeof def, "%s/gogo-ble.sock", tmp && *tmp ? tmp : "/tmp");
        path = def;
    }
    struct sockaddr_un a;
    if (strlen(path) >= sizeof a.sun_path) return GOGO_E_BADPATH;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path, strlen(path) + 1u);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return GOGO_E_IO;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 2) != 0 || pipe(u.stop) != 0) {
        close(fd);
        return GOGO_E_IO;
    }
    pthread_mutex_lock(&mu);
    u.cfg = *cfg;
    drop_values();
    u.lfd = fd;
    u.cfd = -1;
    u.connected = false;
    pthread_mutex_unlock(&mu);
    memcpy(u.path, path, strlen(path) + 1u);
    if (pthread_create(&u.th, NULL, thread_main, NULL) != 0) {
        close(fd);
        close(u.stop[0]);
        close(u.stop[1]);
        u.lfd = u.stop[0] = u.stop[1] = -1;
        unlink(u.path);
        return GOGO_E_IO;
    }
    u.running = true;
    return 0;
}

static void hal_stop(void *ctx) {
    (void)ctx;
    if (!u.running) return;
    ssize_t w = write(u.stop[1], "x", 1);
    (void)w;
    pthread_join(u.th, NULL);
    u.running = false;
    pthread_mutex_lock(&mu);
    if (u.cfd >= 0) close(u.cfd);
    u.cfd = -1;
    u.connected = false;
    drop_values();
    pthread_mutex_unlock(&mu);
    close(u.lfd);
    close(u.stop[0]);
    close(u.stop[1]);
    u.lfd = u.stop[0] = u.stop[1] = -1;
    unlink(u.path);
}

static int hal_set_value(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len) {
    (void)ctx;
    uint8_t *v = NULL;
    if (len) {
        v = malloc(len);
        if (!v) return GOGO_E_NOMEMORY;
        memcpy(v, d, len);
    }
    pthread_mutex_lock(&mu);
    free(u.val[chr]);
    u.val[chr] = v;
    u.vlen[chr] = (uint16_t)len;
    pthread_mutex_unlock(&mu);
    return 0;
}

static int hal_notify(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len) {
    (void)ctx;
    static char out[32 + 2u * BLE_VALUE_MAX];   /* VM thread only */
    pthread_mutex_lock(&mu);
    int fd = u.connected ? u.cfd : -1;
    pthread_mutex_unlock(&mu);
    if (fd < 0) return GOGO_E_CLOSED;
    size_t n = (size_t)snprintf(out, 32, "notify %u ", (unsigned)chr);
    n += put_hex(out + n, d, len);
    out[n++] = '\n';
    /* hang_up takes fd from u.cfd, then closes it under wmu: while this
     * holds wmu and u.cfd is still fd, fd stays open */
    pthread_mutex_lock(&wmu);
    pthread_mutex_lock(&mu);
    bool same = u.cfd == fd;
    pthread_mutex_unlock(&mu);
    /* A phone that does not read fills the socket buffer: the first
     * write does not wait (BLE_AGAIN, tried again by the driver, as a
     * board's stack without a buffer); once part of the line is out, the
     * rest is written in full, so the line protocol stays whole. */
    int r = GOGO_E_CLOSED;
    if (same) {
        ssize_t w;
        do w = send(fd, out, n, MSG_DONTWAIT); while (w < 0 && errno == EINTR);
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) r = BLE_AGAIN;
        else if (w > 0 && write_all(fd, out + w, n - (size_t)w)) r = 0;
    }
    pthread_mutex_unlock(&wmu);
    return r;
}

const BleHal ble_unix_hal = {
    .ctx = NULL, .board = &board,
    .start = hal_start, .stop = hal_stop, .set_value = hal_set_value, .notify = hal_notify,
    .lock = hal_lock, .unlock = hal_unlock,
};
