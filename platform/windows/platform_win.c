/* platform_win.c: the Windows board (gogo_host table, netif, base port
 * drivers and the platform_* functions platform/unix/platform_unix.h
 * declares).
 *
 * It is the Windows twin of platform/unix/platform_unix.c and keeps every
 * exported name, so platform/unix/main.c, platform/unix/gpio_unix.c,
 * platform/unix/input_unix.c and platform/unix/screen_unix.c compile and
 * link unchanged (build_vm_win.ps1 does exactly that):
 *
 *   unix_host, unix_ports_host   the gogo_host tables (stdout/stderr,
 *                                malloc under GOGO_ARENA_BYTES, the
 *                                QueryPerformanceCounter clock, the wake
 *                                event, BCryptGenRandom, the config file)
 *   unix_netif, unix_base_ports  no --net-fd link: mac reports none (as
 *                                unix does without a link), gpio and the
 *                                simulated net stack's drivers
 *   platform_write/_err/_now_ms/_random/_wait/_notify,
 *   platform_set_args/_links, platform_config_get, platform_unix_wall_start
 *   platform_unix_extra_fds      always NULL (no extra fds)
 *   platform_unix_net_open/...   --net-fd is POSIX-only: a clean refusal
 *
 * Clock: QueryPerformanceCounter (monotonic), relative to first use, as the
 * unix clock_gettime(CLOCK_MONOTONIC) is. Wake: a Win32 auto-reset event
 * with the latch semantics of the unix self-pipe (a notify latches, a wait
 * consumes). Wait: WaitForSingleObject on the event; in tty mode
 * (platform/unix/input_unix.c's raw keyboard) a 10 ms slice loop that polls
 * _kbhit() between slices, because a console input handle is not a waitable
 * byte stream the way a unix tty fd is.
 */
#include "platform_unix.h"
#include "config.h"
#include "gpio_unix.h"
#include "input_unix.h"
#include "net.h"
#include "net_unix.h"
#include "netif.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <io.h>

/* platform/windows/console_win.c */
int platform_win_kbd_ready(void);

/* ---- output ---- */

void platform_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }

/* Flush program output first so diagnostics interleave in order. */
void (*platform_unix_err_hook)(const char *, size_t);   /* test-only capture */
void platform_write_err(const char *s, size_t n) {
    if (platform_unix_err_hook) { platform_unix_err_hook(s, n); return; }
    fflush(stdout);
    fwrite(s, 1, n, stderr);
}

/* ---- the allocator (chunk_alloc / chunk_free, as unix) ---- */

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

/* ---- the clock ---- */

static uint64_t mono_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) return (uint64_t)GetTickCount64();
    return (uint64_t)((now.QuadPart * 1000) / freq.QuadPart);
}

static uint64_t time_start;
static int time_started;

/* The net link's clock state (--net-fd, platform_unix_net_open below): with
 * --sim-clock time moves only in platform_wait, toward the target the peer
 * simulator set with advance messages. Declared here because platform_now_ms
 * reads it. */
static SOCKET net_sock = INVALID_SOCKET;
static bool sim_clock;
static uint64_t sim_now, sim_target;
static void net_wait(uint32_t ms);   /* the --net-fd link's wait (below) */

uint64_t platform_now_ms(void) {
    uint64_t now;
    if (sim_clock) return sim_now;
    now = mono_ms();
    if (!time_started) {
        time_start = now;
        time_started = 1;
    }
    return now - time_start;
}

/* ---- the wall clock ---- */

/* Unix time in ms, 0 if unknown. The Unix epoch is 11644473600 s after the
 * FILETIME epoch (1601-01-01), in 100 ns units. */
static int64_t host_unix_ms(void) {
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000u) - 11644473600000LL;
}

int64_t platform_unix_wall_start(void) {
    const char *e = getenv("GOGO_UNIX_TIME_MS");
    if (e) return (int64_t)strtoll(e, NULL, 10);
    /* As unix: under --sim-clock the host clock means nothing (the peer's
     * simulated time is the one the program sees), so the wall clock starts
     * unknown and only SNTP (or the program) sets it. TLS refuses to check
     * certificates until it is known. */
    if (sim_clock) return 0;
    return host_unix_ms();
}

/* ---- randomness ---- */

uint32_t platform_random(void) {
    uint32_t r = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return r;
    {   /* never, in practice: mix the clocks */
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        r = (uint32_t)c.LowPart * 2654435761u ^ (uint32_t)time(NULL) ^ (uint32_t)GetTickCount64();
    }
    return r;
}

/* ---- the wake latch and the idle wait ----
 *
 * Two latches, because gogovm's wait has two shapes:
 *  - a Win32 auto-reset event, for the waits that have no sockets to poll
 *    (a sleep until the next timer, the console's key poll);
 *  - a UDP socket looped back to itself, for the waits that DO poll host
 *    sockets (--host-net): WSAPoll can wait on sockets but not on an event
 *    handle, so platform_notify also posts one datagram to this socket and
 *    platform_wait polls it beside hostnet_win.c's sockets.
 * A notify sets both; whichever wait runs consumes its own. */

static HANDLE wake_ev;
static SOCKET wake_sock = INVALID_SOCKET;
static struct sockaddr_in wake_addr;

static void wake_init(void) {
    if (wake_ev) return;
    wake_ev = CreateEventA(NULL, FALSE /* auto-reset: a wait consumes the latch */, FALSE, NULL);
    if (!wake_ev) {
        fputs("gogo: cannot create wakeup event\n", stderr);
        exit(1);
    }
}

static void net_init(void) {
    WSADATA w;
    SOCKET s;
    struct sockaddr_in a;
    int alen = sizeof a;
    u_long nb = 1;
    if (wake_sock != INVALID_SOCKET) return;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        closesocket(s);
        return;
    }
    ioctlsocket(s, FIONBIO, &nb);
    wake_addr = a;
    wake_sock = s;
}

__attribute__((constructor)) static void platform_ctor(void) {
    wake_init();
    net_init();
}

/* The datagrams the wake socket queued (1 byte each). */
static void wake_drain(void) {
    char b[64];
    while (wake_sock != INVALID_SOCKET && recv(wake_sock, b, sizeof b, 0) > 0) {}
}

/* Any thread (posts), and the CRT's signal thread. */
void platform_notify(void) {
    if (wake_ev) SetEvent(wake_ev);
    if (wake_sock != INVALID_SOCKET) {
        (void)sendto(wake_sock, "x", 1, 0, (struct sockaddr *)&wake_addr, sizeof wake_addr);
    }
}

/* tty mode's poll slice: the VM thread wakes this often to look at the
 * console. 10 ms keeps typed keys responsive without measurable CPU. */
#define WIN_WAIT_SLICE 10u
/* Sockets one wait may poll (hostnet's connections and listeners). */
#define WIN_WAIT_FDS 128

/* The wait while host sockets exist (--host-net): WSAPoll over this file's
 * wake socket and every socket hostnet_win.c has something to wait for.
 * Readiness is not recorded here: hostnet's own event source polls again
 * (s_pending) when the VM steps, which is where revents are kept. The
 * array grows to the socket count (a server may hold hundreds). */
static WSAPOLLFD *wpf;
static int wpf_cap;

static WSAPOLLFD *wpf_room(int want) {
    if (want > wpf_cap) {
        WSAPOLLFD *p = realloc(wpf, (size_t)want * sizeof *p);
        if (!p) return NULL;
        wpf = p;
        wpf_cap = want;
    }
    return wpf;
}

static void win_wait_sockets(uint32_t ms, int tty) {
    uint64_t deadline = 0;
    if (ms != UINT32_MAX) deadline = platform_now_ms() + ms;
    for (;;) {
        int extra = platform_unix_extra_fds(NULL, 0);
        int n = 1;
        int slice;
        WSAPOLLFD *pf;
        if (extra < 0) extra = 0;
        pf = wpf_room(1 + extra);
        if (!pf) {                       /* out of memory: no wait (the VM polls) */
            WaitForSingleObject(wake_ev, tty ? WIN_WAIT_SLICE : (ms == UINT32_MAX ? 1 : ms));
            return;
        }
        pf[0].fd = wake_sock;
        pf[0].events = POLLRDNORM;
        pf[0].revents = 0;
        if (extra > 0) n += platform_unix_extra_fds(pf + 1, extra);
        if (ms == UINT32_MAX) {
            slice = -1;
        } else {
            uint64_t now = platform_now_ms();
            uint64_t left;
            if (now >= deadline) return;
            left = deadline - now;
            slice = left > 2000000000u ? 2000000000 : (int)left;
        }
        if (tty && (slice < 0 || slice > (int)WIN_WAIT_SLICE)) slice = (int)WIN_WAIT_SLICE;
        {
            int rc = WSAPoll(pf, (ULONG)n, slice);
            if (rc > 0) {
                if (pf[0].revents) {              /* notified: consume both latches */
                    wake_drain();
                    (void)WaitForSingleObject(wake_ev, 0);
                    return;
                }
                for (int k = 1; k < n; k++) {
                    if (pf[k].revents) return;    /* a host socket is ready */
                }
            }
        }
        if (tty && platform_win_kbd_ready()) {
            input_unix_read_tty();
            return;
        }
        if (ms != UINT32_MAX && platform_now_ms() >= deadline) return;
    }
}

/* Returns after the timeout or once notified; consumes the latch. */
void platform_wait(uint32_t ms) {
    /* msvcrt's _IOLBF (set by main.c) only line-buffers a CONSOLE, not a
     * pipe or a file: a long-running program's output would sit in the
     * buffer (a server's "listening on port N" never reaching its log).
     * Flushing before every idle wait gives the unix behaviour: what was
     * printed is visible as soon as the VM blocks. */
    fflush(stdout);
    if (input_unix_wait(ms)) return;              /* an input line was consumed: rescan */
    int tty = input_unix_poll_fd() >= 0;
    if (net_sock != INVALID_SOCKET || sim_clock) {   /* --net-fd: the stack's link */
        net_wait(ms);
        return;
    }
    if (platform_unix_extra_fds && wake_sock != INVALID_SOCKET) {
        win_wait_sockets(ms, tty);
        return;
    }
    uint64_t deadline = 0;
    DWORD slice = ms;
    if (ms != UINT32_MAX) deadline = platform_now_ms() + ms;
    if (tty && (ms == UINT32_MAX || ms > WIN_WAIT_SLICE)) slice = WIN_WAIT_SLICE;
    for (;;) {
        DWORD rc = WaitForSingleObject(wake_ev, slice);
        if (rc == WAIT_OBJECT_0) return;           /* notified (the latch is consumed) */
        if (tty && platform_win_kbd_ready()) {
            input_unix_read_tty();                 /* raw keys: the reader queues them */
            return;
        }
        if (ms == UINT32_MAX) continue;            /* no timeout: wait another slice */
        uint64_t now = platform_now_ms();
        if (now >= deadline) return;
        {
            uint64_t left = deadline - now;
            slice = left > WIN_WAIT_SLICE ? WIN_WAIT_SLICE : (DWORD)left;
        }
    }
}

/* ---- the net link (--net-fd) ----
 *
 * The unix net link is a socketpair end polled with poll(); Windows has
 * neither socketpairs nor a poll() over CRT fds, and the link only exists
 * for the POSIX golden tests. Refusing it by name is better than hanging:
 * a program that does not use it is untouched (the simulated net stack,
 * runtime/net, needs no host fds). */
/* platform_unix.c's link, over Winsock. A Windows child cannot inherit a
 * socketpair end the way POSIX passes fd 3, so here the number after
 * --net-fd is a TCP PORT on 127.0.0.1 to CONNECT to: the peer (the tests'
 * simulator, tests/net/sim, which on Windows listens on loopback and then
 * accepts) is the other end. Everything after the socket is the unix logic:
 * messages [u16 len, big-endian][u8 type][len bytes] with FRAME, ADVANCE,
 * ACK and LINK, the simulated clock that only advances move, the ACK sent
 * when the VM is idle at the target, and exit 0 at EOF when time is
 * simulated (the simulator is done). */
static bool link_down;        /* set by NET_MSG_LINK: sends fail while down */
static bool ack_owed;
static uint32_t net_rx_count;
static NetState *net_ns;      /* the stack the link feeds (NULL: frames dropped) */
static void (*rx_sink)(const uint8_t *, uint32_t);
static int (*tx_hook)(const uint8_t *, uint32_t);   /* test-only: frames go to fn */

static bool write_all_sock(SOCKET s, const uint8_t *p, size_t n) {
    while (n) {
        int chunk = n > (1u << 20) ? (1 << 20) : (int)n;
        int w = send(s, (const char *)p, chunk, 0);
        if (w == SOCKET_ERROR || w == 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all_sock(SOCKET s, uint8_t *p, size_t n) {
    while (n) {
        int chunk = n > (1u << 20) ? (1 << 20) : (int)n;
        int r = recv(s, (char *)p, chunk, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool net_put(uint8_t type, const uint8_t *payload, uint32_t len) {
    uint8_t h[3];
    if (net_sock == INVALID_SOCKET || len > 0xFFFFu) return false;
    h[0] = (uint8_t)(len >> 8);
    h[1] = (uint8_t)len;
    h[2] = type;
    return write_all_sock(net_sock, h, 3) && write_all_sock(net_sock, payload, len);
}

/* The link is gone: the stack's link goes down; with the simulated clock
 * time can no longer move, so the process exits 0 (the simulator is done). */
static void net_eof(void) {
    if (net_sock != INVALID_SOCKET) closesocket(net_sock);
    net_sock = INVALID_SOCKET;
    if (net_ns) net_link_event(net_ns, false);
    if (sim_clock) {
        fflush(stdout);
        exit(0);
    }
}

/* Reads and handles one link message (the socket is readable). */
static void net_read(void) {
    static uint8_t buf[2048];
    uint8_t h[3];
    if (!read_all_sock(net_sock, h, 3)) { net_eof(); return; }
    uint32_t len = ((uint32_t)h[0] << 8) | h[1];
    uint32_t keep = len < sizeof buf ? len : (uint32_t)sizeof buf;
    if (!read_all_sock(net_sock, buf, keep)) { net_eof(); return; }
    for (uint32_t rest = len - keep; rest;) {        /* oversized: skip the tail */
        uint8_t junk[256];
        uint32_t n = rest < sizeof junk ? rest : (uint32_t)sizeof junk;
        if (!read_all_sock(net_sock, junk, n)) { net_eof(); return; }
        rest -= n;
    }
    switch (h[2]) {
    case NET_MSG_FRAME:
        if (len > keep) break;                       /* too long for any frame */
        net_rx_count++;
        if (rx_sink) rx_sink(buf, len);
        else if (net_ns) net_rx_offer(net_ns, buf, len);
        break;
    case NET_MSG_ADVANCE:
        if (len != 4) break;
        if (sim_clock) {
            sim_target += ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
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
    default:                                         /* unknown: ignored */
        break;
    }
}

/* Polls the wake socket and the link for up to timeout ms (-1 = forever);
 * handles one link message; true if anything was ready. */
static bool net_poll(int timeout) {
    WSAPOLLFD p[2];
    int n = 0, wi = -1, li = -1;
    if (wake_sock != INVALID_SOCKET) {
        wi = n;
        p[n].fd = wake_sock;
        p[n].events = POLLRDNORM;
        p[n].revents = 0;
        n++;
    }
    if (net_sock != INVALID_SOCKET) {
        li = n;
        p[n].fd = net_sock;
        p[n].events = POLLRDNORM;
        p[n].revents = 0;
        n++;
    }
    int slice = timeout;
    if (input_unix_poll_fd() >= 0 && (timeout < 0 || timeout > (int)WIN_WAIT_SLICE)) {
        slice = (int)WIN_WAIT_SLICE;                 /* a console cannot be polled: slice */
    }
    int rc = n ? WSAPoll(p, (ULONG)n, slice) : 0;
    if (rc > 0) {
        if (wi >= 0 && p[wi].revents) wake_drain();
        if (li >= 0 && p[li].revents) net_read();
    }
    if (input_unix_poll_fd() >= 0 && platform_win_kbd_ready()) input_unix_read_tty();
    return rc > 0;
}

static void net_wait(uint32_t ms) {
    if (!sim_clock) {
        net_poll(ms == UINT32_MAX ? -1 : (ms > 2000000000u ? 2000000000 : (int)ms));
        return;
    }
    if (net_poll(0)) return;                          /* input first */
    if (ms != UINT32_MAX && sim_target - sim_now >= ms) {
        sim_now += ms;                                /* sleep in simulated time */
        return;
    }
    sim_now = sim_target;                             /* idle at the target */
    if (ack_owed) {
        ack_owed = false;
        net_put(NET_MSG_ACK, NULL, 0);
    }
    net_poll(-1);
}

/* fd is a TCP port on 127.0.0.1 here (see above). */
void platform_unix_net_open(int fd, bool sim, NetState *ns) {
    struct sockaddr_in a;
    SOCKET s;
    BOOL one = 1;
    if (fd <= 0 || fd > 65535) {
        fflush(stdout);
        fprintf(stderr, "gogovm: --net-fd %d: on Windows the number is a TCP port on 127.0.0.1\n", fd);
        exit(1);                                      /* EXIT_USAGE of main.c */
    }
    net_ns = ns;
    link_down = false;
    sim_clock = sim;
    sim_now = sim_target = 0;
    ack_owed = false;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)fd);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    if (s == INVALID_SOCKET) {
        fflush(stdout);
        fprintf(stderr, "gogovm: --net-fd %d: cannot connect the simulator on 127.0.0.1\n", fd);
        exit(1);
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    net_sock = s;
}
void platform_unix_net_close(void) {                   /* test-only */
    if (net_sock != INVALID_SOCKET) closesocket(net_sock);
    net_sock = INVALID_SOCKET;
    net_ns = NULL;
    sim_clock = false;
    ack_owed = false;
}
void platform_unix_net_set_sink(void (*fn)(const uint8_t *, uint32_t)) { rx_sink = fn; }
uint32_t platform_unix_net_rx_count(void) { return net_rx_count; }
void platform_unix_net_set_tx(int (*fn)(const uint8_t *, uint32_t)) { tx_hook = fn; }

/* No extra fds: --host-net is stubbed (hostnet_win.c). */
int (*platform_unix_extra_fds)(struct pollfd *p, int max);

/* ---- the netif: the --net-fd link, or none (as unix) ---- */

static const uint8_t net_mac_addr[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };   /* locally administered */

static bool net_mac(void *ctx, uint8_t mac[6]) {
    (void)ctx;
    if (net_sock == INVALID_SOCKET && !tx_hook) return false;
    memcpy(mac, net_mac_addr, 6);
    return true;
}
static int net_send(void *ctx, const uint8_t *frame, uint32_t len) {
    (void)ctx;
    if (link_down) return -1;
    if (tx_hook) return tx_hook(frame, len);
    return net_put(NET_MSG_FRAME, frame, len) ? 0 : -1;
}
static bool net_link_up(void *ctx) {
    (void)ctx;
    return !link_down && (net_sock != INVALID_SOCKET || tx_hook);
}
/* No status line on the host: programs print what they need. */
static void net_report(void *ctx, bool up, uint32_t ip, uint32_t gw, uint32_t dns) {
    (void)ctx; (void)up; (void)ip; (void)gw; (void)dns;
}

const NetIf unix_netif = { net_mac, net_send, net_link_up, net_report, NULL };

/* ---- the base port drivers (main.c adds the input ones) ---- */

const PortDriver *const unix_base_ports[] = {
    &gpio_unix_driver, &net_netif_driver, &net_udp_driver, &net_tcp_driver, &net_tcp_listen_driver, NULL
};

/* ---- sys.Config: $GOGO_CONFIG, else %USERPROFILE%\.gogo\config ----
 *
 * Same file format as unix: key=value lines, first line for the key wins,
 * over-long lines skipped. */

#define WIN_CONFIG_LINE 1024

int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap) {
    if (klen == 0 || memchr(key, '=', klen) || memchr(key, '\n', klen)) return -1;
    char path[WIN_CONFIG_LINE];
    const char *p = getenv("GOGO_CONFIG");
    if (!p || !*p) {
        const char *home = getenv("USERPROFILE");
        if (!home || !*home) home = getenv("HOME");
        if (!home) return -1;
        int n = snprintf(path, sizeof path, "%s\\.gogo\\config", home);
        if (n < 0 || (size_t)n >= sizeof path) return -1;
        p = path;
    }
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    static char line[WIN_CONFIG_LINE];
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

/* The program's arguments (platform_set_args): the strings of the command
 * line after the image, which stay valid while the VM runs. */
static int prog_argc;
static char *const *prog_argv;

void platform_set_args(int argc, char *const *argv) {
    prog_argc = argc;
    prog_argv = argv;
}

/* The value of the key "\0args" (sys.Args): every argument followed by a
 * NUL, so an empty argument is kept and no arguments is an unset key. */
static int32_t args_value(uint8_t *buf, uint32_t cap) {
    uint32_t n = 0;
    for (int i = 0; i < prog_argc; i++) {
        size_t l = strlen(prog_argv[i]) + 1u;
        if (cap > n) memcpy(buf + n, prog_argv[i], l < cap - n ? l : cap - n);
        n += (uint32_t)l;
    }
    return n ? (int32_t)n : -1;
}

static uint32_t vm_links;

void platform_set_links(uint32_t n) { vm_links = n; }

/* The value of the key "\0links": the link cap in decimal (unset if 0). */
static int32_t links_value(uint8_t *buf, uint32_t cap) {
    char d[11];
    int n = vm_links ? snprintf(d, sizeof d, "%u", (unsigned)vm_links) : 0;
    if (n <= 0) return -1;
    if (cap) memcpy(buf, d, (uint32_t)n < cap ? (uint32_t)n : cap);
    return n;
}

static int32_t host_config_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    if (kl == 5u && memcmp(k, "\0args", 5) == 0) return args_value(buf, cap);
    if (kl == 6u && memcmp(k, "\0links", 6) == 0) return links_value(buf, cap);
    return platform_config_get(k, kl, buf, cap);
}

/* Board ports' posts and posters (BoardRun.ports_host): malloc and free,
 * which any thread may call, unlike chunk_alloc's arena count; the wake is
 * the event, which any thread may set. */
static void *ports_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void ports_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
const gogo_host unix_ports_host = {
    .size = sizeof(gogo_host),
    .write = host_write,
    .alloc = ports_alloc,
    .free = ports_free,
    .now_ms = host_now,
    .wake = host_wake,
    .wait = host_wait,
};

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
