/* ble_win.c: platform/unix/ble_unix.c on Winsock.
 *
 * The "ble" port's radio is a Unix-domain socket, and Windows 10 has those:
 * AF_UNIX stream sockets (afunix.h) work with bind/listen/accept and with
 * WSAPoll, and Go's net.Dial("unix", path) reaches them, so the phone of
 * tests/ble connects to this build exactly as it does to the unix one. The
 * line protocol, the one-phone-at-a-time rule and the VM wake are the unix
 * logic, call for call.
 *
 * What differs is the socket API and the stop signal:
 *   int fd / read / write / close   -> SOCKET / recv / send / closesocket
 *   unlink(path)                    -> DeleteFileA(path)
 *   poll(...)                       -> WSAPoll(POLLRDNORM)
 *   the thread's stop pipe          -> a UDP socket looped back to itself
 *   send(MSG_DONTWAIT)              -> "is the socket writable now?" first
 * The thread is a pthread, as on unix (MinGW's winpthreads), so the locks
 * and the join are unchanged.
 */
#include "ble_unix.h"
#include "platform_unix.h"     /* platform_notify */
#include "gogo_errors.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BleBoard board;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;    /* board's queue, the values, conn */
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;   /* writes to the phone's socket */

static struct {
    BleConfig cfg;                   /* the thread's copy (start) */
    uint8_t *val[BLE_MAX_CHARS];     /* the values (malloc), NULL: empty */
    uint16_t vlen[BLE_MAX_CHARS];
    SOCKET lfd, cfd;                 /* listener, phone (INVALID_SOCKET: none) */
    SOCKET wake;                     /* the thread's stop latch (a looped UDP socket) */
    bool connected;                  /* the phone sent connect (mu) */
    bool running;
    pthread_t th;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} u = { .lfd = INVALID_SOCKET, .cfd = INVALID_SOCKET, .wake = INVALID_SOCKET };

static void hal_lock(void *ctx) { (void)ctx; pthread_mutex_lock(&mu); }
static void hal_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&mu); }

/* ---- the phone's socket ---- */

/* Writes all of s (n bytes) to the phone, under wmu; false if it is gone. */
static bool write_all(SOCKET fd, const char *s, size_t n) {
    while (n) {
        int chunk = n > (1u << 20) ? (1 << 20) : (int)n;
        int w = send(fd, s, chunk, 0);
        if (w <= 0) break;
        s += w;
        n -= (size_t)w;
    }
    return n == 0;
}

static bool send_line(SOCKET fd, const char *s, size_t n) {
    pthread_mutex_lock(&wmu);
    bool ok = write_all(fd, s, n);
    pthread_mutex_unlock(&wmu);
    return ok;
}

/* True if the socket has room now (what send with MSG_DONTWAIT asks on unix). */
static bool writable(SOCKET fd) {
    WSAPOLLFD p;
    p.fd = fd;
    p.events = POLLWRNORM;
    p.revents = 0;
    return WSAPoll(&p, 1, 0) > 0 && (p.revents & POLLWRNORM) != 0;
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

static void say_gatt(SOCKET fd) {
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

static void reply(SOCKET fd, const char *what, unsigned k, int code) {
    char line[48];
    int n = code ? snprintf(line, sizeof line, "error %u %d\n", k, code) : snprintf(line, sizeof line, "%s %u\n", what, k);
    send_line(fd, line, (size_t)n);
}

/* One line from the phone; false: drop the connection (reason in *why). */
static bool handle(SOCKET fd, char *line, uint32_t *why) {
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
    SOCKET fd = u.cfd;
    u.cfd = INVALID_SOCKET;
    pthread_mutex_unlock(&mu);
    if (fd != INVALID_SOCKET) {
        pthread_mutex_lock(&wmu);   /* no notify is writing to it */
        closesocket(fd);
        pthread_mutex_unlock(&wmu);
    }
    if (conn) post(BLE_EV_DISCONNECTED, why, NULL, 0);
}

static void *thread_main(void *arg) {
    (void)arg;
    static char line[64 + 2u * BLE_VALUE_MAX];
    size_t fill = 0;
    for (;;) {
        WSAPOLLFD p[3];
        int n = 0, wi, li, ci = -1;
        SOCKET cf;
        wi = n;
        p[n].fd = u.wake;
        p[n].events = POLLRDNORM;
        p[n].revents = 0;
        n++;
        li = n;
        p[n].fd = u.lfd;
        p[n].events = POLLRDNORM;
        p[n].revents = 0;
        n++;
        pthread_mutex_lock(&mu);
        cf = u.cfd;
        pthread_mutex_unlock(&mu);
        if (cf != INVALID_SOCKET) {
            ci = n;
            p[n].fd = cf;
            p[n].events = POLLRDNORM;
            p[n].revents = 0;
            n++;
        }
        if (WSAPoll(p, (ULONG)n, -1) <= 0) continue;
        if (p[wi].revents) {                     /* stop */
            char b[8];
            (void)recv(u.wake, b, sizeof b, 0);
            break;
        }
        if (p[li].revents) {
            SOCKET fd = accept(u.lfd, NULL, NULL);
            if (fd != INVALID_SOCKET) {
                pthread_mutex_lock(&mu);
                bool taken = u.cfd != INVALID_SOCKET;
                if (!taken) u.cfd = fd;
                pthread_mutex_unlock(&mu);
                if (taken) {
                    send_line(fd, "busy\n", 5);
                    closesocket(fd);
                } else {
                    fill = 0;
                    say_gatt(fd);
                }
            }
        }
        if (ci >= 0 && p[ci].revents) {
            int r = recv(cf, line + fill, (int)(sizeof line - 1u - fill), 0);
            if (r <= 0) {
                hang_up(0x13);
                continue;
            }
            fill += (size_t)r;
            char *nl;
            uint32_t why = 0;
            bool keep = true;
            while (keep && (nl = memchr(line, '\n', fill)) != NULL) {
                *nl = 0;
                keep = handle(cf, line, &why);
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

/* A UDP socket looped back to itself: one byte sent to it ends the thread,
 * which is what the unix stop pipe does. INVALID_SOCKET on failure. */
static SOCKET wake_new(void) {
    struct sockaddr_in a;
    int alen = sizeof a;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    u_long nb = 1;
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

static int hal_start(void *ctx, const BleConfig *cfg) {
    (void)ctx;
    const char *path = getenv("GOGO_BLE");
    char def[sizeof u.path];
    if (!path || !*path) {
        const char *tmp = getenv("TEMP");
        if (!tmp || !*tmp) tmp = getenv("TMP");
        snprintf(def, sizeof def, "%s/gogo-ble.sock", tmp && *tmp ? tmp : ".");
        path = def;
    }
    struct sockaddr_un a;
    if (strlen(path) >= sizeof a.sun_path) return GOGO_E_BADPATH;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path, strlen(path) + 1u);
    SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return GOGO_E_IO;
    DeleteFileA(path);                       /* a leftover from a crash */
    SOCKET wake = wake_new();
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 2) != 0 ||
        wake == INVALID_SOCKET) {
        closesocket(fd);
        if (wake != INVALID_SOCKET) closesocket(wake);
        return GOGO_E_IO;
    }
    pthread_mutex_lock(&mu);
    u.cfg = *cfg;
    drop_values();
    u.lfd = fd;
    u.cfd = INVALID_SOCKET;
    u.wake = wake;
    u.connected = false;
    pthread_mutex_unlock(&mu);
    memcpy(u.path, path, strlen(path) + 1u);
    if (pthread_create(&u.th, NULL, thread_main, NULL) != 0) {
        closesocket(fd);
        closesocket(wake);
        u.lfd = u.wake = INVALID_SOCKET;
        DeleteFileA(u.path);
        return GOGO_E_IO;
    }
    u.running = true;
    return 0;
}

static void hal_stop(void *ctx) {
    (void)ctx;
    if (!u.running) return;
    if (u.wake != INVALID_SOCKET) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        /* the wake socket's own address, from the bound port */
        {
            int alen = sizeof a;
            if (getsockname(u.wake, (struct sockaddr *)&a, &alen) != 0) {
                a.sin_port = 0;
            }
        }
        (void)sendto(u.wake, "x", 1, 0, (struct sockaddr *)&a, sizeof a);
    }
    pthread_join(u.th, NULL);
    u.running = false;
    pthread_mutex_lock(&mu);
    if (u.cfd != INVALID_SOCKET) closesocket(u.cfd);
    u.cfd = INVALID_SOCKET;
    u.connected = false;
    drop_values();
    pthread_mutex_unlock(&mu);
    if (u.lfd != INVALID_SOCKET) closesocket(u.lfd);
    if (u.wake != INVALID_SOCKET) closesocket(u.wake);
    u.lfd = u.wake = INVALID_SOCKET;
    DeleteFileA(u.path);
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
    SOCKET fd = u.connected ? u.cfd : INVALID_SOCKET;
    pthread_mutex_unlock(&mu);
    if (fd == INVALID_SOCKET) return GOGO_E_CLOSED;
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
        if (!writable(fd)) {
            r = BLE_AGAIN;
        } else {
            int w = send(fd, out, (int)n, 0);
            if (w > 0 && write_all(fd, out + w, n - (size_t)w)) r = 0;
        }
    }
    pthread_mutex_unlock(&wmu);
    return r;
}

const BleHal ble_unix_hal = {
    .ctx = NULL, .board = &board,
    .start = hal_start, .stop = hal_stop, .set_value = hal_set_value, .notify = hal_notify,
    .lock = hal_lock, .unlock = hal_unlock,
};
