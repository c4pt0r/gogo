#define _POSIX_C_SOURCE 200112L   /* clock_gettime, nanosleep, poll, pipe */
#include "platform_unix.h"
#include "config.h"
#include "gpio_unix.h"
#include "input_unix.h"
#include "netif.h"
#include "net_unix.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <sys/random.h>      /* getentropy: declared here on macOS and glibc (unistd.h hides it under _POSIX_C_SOURCE) */
#include <fcntl.h>
#include <string.h>

void platform_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
/* Flush program output first so diagnostics interleave in order. */
void (*platform_unix_err_hook)(const char *, size_t);  /* test-only capture */
void platform_write_err(const char *s, size_t n) {
    if (platform_unix_err_hook) { platform_unix_err_hook(s, n); return; }
    fflush(stdout);
    fwrite(s, 1, n, stderr);
}

/* Bytes handed out by chunk_alloc and not yet freed; a nonzero cap makes
 * the allocator fail past it, like a small MCU arena. */
static size_t arena_used;

static void *chunk_alloc(void *ctx, size_t bytes) {
    (void)ctx;
    if (GOGO_ARENA_BYTES && bytes > (size_t)GOGO_ARENA_BYTES - arena_used) return NULL;
    void *p = malloc(bytes);
    if (p) arena_used += bytes;
    return p;
}
static void chunk_free(void *ctx, void *p, size_t bytes) {
    (void)ctx;
    arena_used = bytes > arena_used ? 0 : arena_used - bytes;
    free(p);
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t time_start;
static int time_started;

/* Simulated clock (--sim-clock): time moves only in
 * platform_wait, toward the target the peer simulator set with
 * advance_clock messages on the net link. */
static bool sim_clock;
static uint64_t sim_now, sim_target;

uint64_t platform_now_ms(void) {
    if (sim_clock) return sim_now;
    uint64_t now = mono_ms();
    if (!time_started) {
        time_start = now;
        time_started = 1;
    }
    return now - time_start;
}

/* The wall clock (host.h) starts from $GOGO_UNIX_TIME_MS if set (0:
 * unknown), else from the host clock, but unknown under --sim-clock (the
 * network goldens set it with SNTP). gogovm hands it to the Vm, which keeps
 * it as an offset (runtime/vm/host.c). */
int64_t platform_unix_wall_start(void) {
    const char *e = getenv("GOGO_UNIX_TIME_MS");
    if (e) return (int64_t)strtoll(e, NULL, 10);
    if (sim_clock) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t platform_random(void) {
    uint32_t r;
    if (getentropy(&r, sizeof r) != 0) {      /* never, in practice: mix the clocks */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        r = (uint32_t)ts.tv_nsec * 2654435761u ^ (uint32_t)ts.tv_sec ^ (uint32_t)mono_ms();
    }
    return r;
}


/* ---- net link (--net-fd) ----
 *
 * One stream fd (a socketpair end in tests) carries messages
 * [u16 len, big-endian][u8 type][len bytes]: NET_MSG_FRAME (a raw Ethernet
 * frame, both ways), NET_MSG_ADVANCE (simulator -> VM: u32 ms, big-endian,
 * move the simulated clock forward) and NET_MSG_ACK (VM -> simulator, no
 * payload). With --sim-clock the clock moves only toward the target the
 * advances set, in steps: platform_wait(t) with the target at least t
 * ahead moves time by t and returns at once, so timers fire at their own
 * times however large the advance. When the VM has reached the target and
 * goes idle it sends one ACK for the advances received, then blocks on the
 * link (and the wake pipe and tty) without a timeout. So "advance n, wait
 * for the ACK" runs everything due by now + n and returns once the VM is
 * idle, after all the frames it sent meanwhile; advance 0 is a sync.
 * Without --sim-clock, advances are acknowledged at once. Frames are read
 * one per platform_wait, so the stack's single RX slot is drained between
 * them. At EOF the link is down (net_link_event(ns, false)); with --sim-clock
 * time can no longer move, so the process exits 0 (the simulator is done). */
static int net_fd = -1;
static bool link_down;        /* set by NET_MSG_LINK: sends fail while down */
static bool ack_owed;
static uint32_t net_rx_count;
static NetState *net_ns;      /* the stack the link feeds (NULL: frames dropped) */

/* Frames go to net_rx_offer unless a test diverts them. */
static void (*rx_sink)(const uint8_t *, uint32_t);

void platform_unix_net_open(int fd, bool sim, NetState *ns) {
    net_fd = fd;
    net_ns = ns;
    link_down = false;
    sim_clock = sim;
    sim_now = sim_target = 0;
    ack_owed = false;
}

void platform_unix_net_close(void) {      /* test-only */
    net_fd = -1;
    net_ns = NULL;
    sim_clock = false;
    ack_owed = false;
}

void platform_unix_net_set_sink(void (*fn)(const uint8_t *, uint32_t)) { rx_sink = fn; }
uint32_t platform_unix_net_rx_count(void) { return net_rx_count; }

static bool write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool net_put(uint8_t type, const uint8_t *payload, uint32_t len) {
    if (net_fd < 0 || len > 0xFFFFu) return false;
    uint8_t h[3] = { (uint8_t)(len >> 8), (uint8_t)len, type };
    return write_all(net_fd, h, 3) && write_all(net_fd, payload, len);
}

/* Test-only: frames the stack sends go to fn instead of the link (NULL =
 * the link); with fn set the platform has a MAC even without a link. */
static int (*tx_hook)(const uint8_t *, uint32_t);
void platform_unix_net_set_tx(int (*fn)(const uint8_t *, uint32_t)) { tx_hook = fn; }

static bool net_mac(void *ctx, uint8_t mac[6]) {
    static const uint8_t m[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };   /* locally administered */
    (void)ctx;
    if (net_fd < 0 && !tx_hook) return false;
    memcpy(mac, m, 6);
    return true;
}

static int net_send(void *ctx, const uint8_t *frame, uint32_t len) {
    (void)ctx;
    if (link_down) return -1;
    if (tx_hook) return tx_hook(frame, len);
    return net_put(NET_MSG_FRAME, frame, len) ? 0 : -1;
}

static bool net_link_up(void *ctx) { (void)ctx; return !link_down && (net_fd >= 0 || tx_hook); }

/* No status line on unix: programs print what they need (golden output). */
static void net_report(void *ctx, bool up, uint32_t ip, uint32_t gw, uint32_t dns) {
    (void)ctx; (void)up; (void)ip; (void)gw; (void)dns;
}

const NetIf unix_netif = { net_mac, net_send, net_link_up, net_report, NULL };

static void net_eof(void) {
    net_fd = -1;
    if (net_ns) net_link_event(net_ns, false);
    if (sim_clock) {
        fflush(stdout);
        exit(0);
    }
}

/* Reads and handles one link message (the fd is readable). The rest of
 * the message is read with blocking reads after poll: acceptable for this
 * host test link, whose peer always writes whole messages at once. */
static void net_read(void) {
    static uint8_t buf[2048];
    uint8_t h[3];
    if (!read_all(net_fd, h, 3)) { net_eof(); return; }
    uint32_t len = ((uint32_t)h[0] << 8) | h[1];
    uint32_t keep = len < sizeof buf ? len : (uint32_t)sizeof buf;
    if (!read_all(net_fd, buf, keep)) { net_eof(); return; }
    for (uint32_t rest = len - keep; rest; ) {          /* oversized: skip the tail */
        uint8_t junk[256];
        uint32_t n = rest < sizeof junk ? rest : (uint32_t)sizeof junk;
        if (!read_all(net_fd, junk, n)) { net_eof(); return; }
        rest -= n;
    }
    switch (h[2]) {
    case NET_MSG_FRAME:
        if (len > keep) break;                          /* too long for any frame */
        net_rx_count++;
        if (rx_sink) rx_sink(buf, len);
        else if (net_ns) net_rx_offer(net_ns, buf, len);
        break;
    case NET_MSG_ADVANCE:
        if (len != 4) break;
        if (sim_clock) {
            sim_target += ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                          ((uint32_t)buf[2] << 8) | buf[3];
            ack_owed = true;
        } else {
            net_put(NET_MSG_ACK, NULL, 0);
        }
        break;
    case NET_MSG_LINK:
        if (len != 1) break;
        link_down = buf[0] == 0;
        if (net_ns) net_link_event(net_ns, buf[0] != 0);
        break;
    default:                                            /* unknown: ignored */
        break;
    }
}

/* Wakeup latch: a self-pipe. platform_notify writes a byte (async-signal-
 * safe, so usable from a signal handler or another thread); platform_wait
 * polls the read end, so a notify before OR during a wait ends it at once,
 * and the byte persists until a wait consumes it. The pipe is created
 * before main (constructor), so platform_notify never has to allocate. */
static int wake_fd[2] = {-1, -1};

__attribute__((constructor)) static void wake_init(void) {
    if (pipe(wake_fd) != 0) {
        fputs("gogo: cannot create wakeup pipe\n", stderr);
        exit(1);
    }
    for (int i = 0; i < 2; i++) {
        fcntl(wake_fd[i], F_SETFL, fcntl(wake_fd[i], F_GETFL) | O_NONBLOCK);
        fcntl(wake_fd[i], F_SETFD, FD_CLOEXEC);
    }
}

void platform_notify(void) {
    char c = 1;
    int saved = errno;
    ssize_t r = write(wake_fd[1], &c, 1);   /* full pipe = already latched */
    (void)r;
    errno = saved;
}

static void net_wait(uint32_t ms);

/* Returns after the timeout or once notified; consumes the latch. */
void platform_wait(uint32_t ms) {
    if (input_unix_wait(ms)) {             /* an input line was consumed: rescan */
        char buf[64];
        while (read(wake_fd[0], buf, sizeof buf) > 0) {}
        return;
    }
    if (net_fd >= 0 || sim_clock) {
        net_wait(ms);
        return;
    }
    struct pollfd p[2];
    char buf[64];
    int n = 1;
    p[0].fd = wake_fd[0]; p[0].events = POLLIN; p[0].revents = 0;
    int sfd = input_unix_poll_fd();        /* tty stdin, or -1 */
    if (sfd >= 0) { p[1].fd = sfd; p[1].events = POLLIN; p[1].revents = 0; n = 2; }
    int rc = poll(p, (nfds_t)n, ms == UINT32_MAX ? -1 : (ms > 2000000000u ? 2000000000 : (int)ms));
    if (rc > 0 && n == 2 && p[1].revents) input_unix_read_tty();
    while (read(wake_fd[0], buf, sizeof buf) > 0) {}   /* clear the latch */
}

/* Polls the wake pipe, the link and the tty for up to timeout ms (-1 =
 * forever); handles one link message and any tty input; clears the latch.
 * True if anything was ready. */
static bool net_poll(int timeout) {
    struct pollfd p[3];
    int n = 0, li = -1, ti = -1;
    p[n].fd = wake_fd[0]; p[n].events = POLLIN; p[n].revents = 0; n++;
    if (net_fd >= 0) { li = n; p[n].fd = net_fd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    int sfd = input_unix_poll_fd();
    if (sfd >= 0) { ti = n; p[n].fd = sfd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    int rc = poll(p, (nfds_t)n, timeout);
    if (rc > 0 && ti >= 0 && p[ti].revents) input_unix_read_tty();
    if (rc > 0 && li >= 0 && p[li].revents) net_read();
    char buf[64];
    while (read(wake_fd[0], buf, sizeof buf) > 0) {}
    return rc > 0;
}

static void net_wait(uint32_t ms) {
    if (!sim_clock) {
        net_poll(ms == UINT32_MAX ? -1 : (ms > 2000000000u ? 2000000000 : (int)ms));
        return;
    }
    if (net_poll(0)) return;                            /* input first */
    if (ms != UINT32_MAX && sim_target - sim_now >= ms) {
        sim_now += ms;                                  /* sleep in simulated time */
        return;
    }
    sim_now = sim_target;                               /* idle at the target */
    if (ack_owed) {
        ack_owed = false;
        net_put(NET_MSG_ACK, NULL, 0);
    }
    net_poll(-1);
}

/* The kbd, power, imu, ir, mic and speaker drivers (runtime/input/) are linked
 * into gogovm only, not into the C test builds that share this file:
 * main.c's board lists them after these. */
const PortDriver *const unix_base_ports[] = {
    &gpio_unix_driver, &net_netif_driver, &net_udp_driver, &net_tcp_driver, &net_tcp_listen_driver, NULL
};

/* sys.Config: the file $GOGO_CONFIG, else ~/.gogo/config,
 * of key=value lines, which `gogo config set|get|list|rm` edits. Read on
 * every call; the first line for the key wins; lines that are too long
 * (UNIX_CONFIG_LINE) are skipped. */
#define UNIX_CONFIG_LINE 1024
int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap) {
    if (klen == 0 || memchr(key, '=', klen) || memchr(key, '\n', klen)) return -1;
    char path[UNIX_CONFIG_LINE];
    const char *p = getenv("GOGO_CONFIG");
    if (!p || !*p) {
        const char *home = getenv("HOME");
        if (!home) return -1;
        int n = snprintf(path, sizeof path, "%s/.gogo/config", home);
        if (n < 0 || (size_t)n >= sizeof path) return -1;
        p = path;
    }
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    static char line[UNIX_CONFIG_LINE];
    int32_t found = -1;
    bool skip = false;   /* the rest of an over-long line */
    while (found < 0 && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        bool whole = n > 0 && line[n - 1] == '\n';
        bool was_skip = skip;
        skip = !whole && !feof(f);
        if (was_skip || skip) continue;
        if (whole) line[--n] = 0;
        if (n > 0 && line[n - 1] == '\r') line[--n] = 0;
        if (n > klen && line[klen] == '=' && memcmp(line, key, klen) == 0) {
            uint32_t vl = (uint32_t)(n - klen - 1u);
            if (buf) memcpy(buf, line + klen + 1, vl < cap ? vl : cap);
            found = (int32_t)vl;
        }
    }
    fclose(f);
    return found;
}

/* ---- the host table ---- */

static void host_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    if (stream == 2) platform_write_err(s, n);
    else platform_write(s, n);
}
static uint64_t host_now(void *ctx) { (void)ctx; return platform_now_ms(); }
static void host_wake(void *ctx) { (void)ctx; platform_notify(); }
static void host_wait(void *ctx, uint32_t ms) { (void)ctx; platform_wait(ms); }
static uint32_t host_random(void *ctx) { (void)ctx; return platform_random(); }
static int32_t host_config_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    return platform_config_get(k, kl, buf, cap);
}

/* No RTC hooks: the Vm keeps the wall clock (main.c seeds it); no special
 * code memory. */
const gogo_host unix_host = {
    .size = sizeof(gogo_host),
    .write = host_write,
    .alloc = chunk_alloc,
    .free = chunk_free,
    .now_ms = host_now,
    .wake = host_wake,
    .wait = host_wait,
    .random = host_random,
    .config_get = host_config_get,
};
