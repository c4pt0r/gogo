#define _POSIX_C_SOURCE 200112L   /* nanosleep, termios */
/* unix gpio simulation (spec M3 5.4).
 *
 * Output pins: a Write prints "gpio: pin N = V" on stderr.
 *
 * Input pins get events from stdin.
 *  - Script mode (stdin is not a terminal): lines "<pin> <0|1>" and
 *    "sleep <ms>". gpio_unix_wait, called from platform_wait, consumes ONE
 *    line per idle wait while an input port is open, so the output depends
 *    only on message causality. Debounce is forced off. Unknown lines are
 *    reported on stderr and skipped. After EOF no input port can produce
 *    events (can_event false), so the VM may exit.
 *  - tty mode: --key <c>=<pin> maps a key to a pin; a press yields level 0
 *    then 1. The terminal is put in non-canonical no-echo mode while an input
 *    port is open and polled in platform_wait. ISIG stays on: SIGINT/SIGTERM
 *    restore the terminal and then die by the default action. Ctrl-D (0x04)
 *    ends the input, so the VM can exit.
 */
#include "gpio_unix.h"
#include "platform.h"
#include "evq.h"
#include "opcodes.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <signal.h>

#define NPINS 64
#define MAX_KEYS 16

enum { M_INPUT = 0, M_PULLUP = 1, M_PULLDOWN = 2, M_OUTPUT = 3 };
enum { E_NONE = 0, E_RISING = 1, E_FALLING = 2, E_BOTH = 3 };

/* Port state: state[0] pin, [1] mode, [2] edge, [3] level | index << 8. */
static Port *pins[NPINS];
static int inputs_open;
static int tty_mode = -1;        /* -1: not decided yet (first open) */
static int stdin_eof;

static struct { char c; int pin; } keys[MAX_KEYS];
static int nkeys;

static struct termios saved_tio;
static volatile sig_atomic_t tio_saved;

int gpio_unix_add_key(char c, int pin) {
    if (nkeys >= MAX_KEYS || pin < 0 || pin >= NPINS) return 0;
    keys[nkeys].c = c;
    keys[nkeys].pin = pin;
    nkeys++;
    return 1;
}

static void restore_tty(void) {
    if (tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    tio_saved = 0;
}

/* Restores the terminal, then re-raises sig with the default action. Only
 * async-signal-safe calls (tcsetattr, signal, raise). */
static void on_fatal_signal(int sig) {
    if (tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void decide_mode(void) {
    if (tty_mode >= 0) return;
    tty_mode = isatty(STDIN_FILENO) ? 1 : 0;
}

static void enter_raw(void) {
    struct termios t;
    if (!tty_mode || tio_saved || tcgetattr(STDIN_FILENO, &saved_tio) != 0) return;
    tio_saved = 1;
    static int hooked;             /* atexit and the handlers: once per process */
    if (!hooked) {
        hooked = 1;
        atexit(restore_tty);
        signal(SIGINT, on_fatal_signal);
        signal(SIGTERM, on_fatal_signal);
    }
    t = saved_tio;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void err_line(const char *prefix, const char *s) {
    char b[160];
    int n = snprintf(b, sizeof b, "%s%s\n", prefix, s);
    if (n > 0) platform_write_err(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

/* ---- driver ---- */

static int g_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    if (n != 4) return -1;
    int32_t pin = cfg[0], mode = cfg[1], edge = cfg[2];
    if (pin < 0 || pin >= NPINS || mode < M_INPUT || mode > M_OUTPUT || edge < E_NONE || edge > E_BOTH) return -1;
    if (pins[pin]) return -1;
    decide_mode();
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode;
    p->state[2] = (uint32_t)edge;
    p->state[3] = (mode == M_PULLUP ? 1u : 0u) | ((uint32_t)index << 8);
    p->debounce_ms = tty_mode ? (uint16_t)(cfg[3] > 0 ? (cfg[3] > 65535 ? 65535 : cfg[3]) : 20) : 0;
    pins[pin] = p;
    if (mode != M_OUTPUT) {
        inputs_open++;
        enter_raw();
    }
    return 0;
}

static void g_command(Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    char b[48];
    if (type != TYPE_WRITE || n != 1 || p->state[1] != M_OUTPUT) return;
    int len = snprintf(b, sizeof b, "gpio: pin %u = %d\n", (unsigned)p->state[0], f[0] != 0);
    if (len > 0) platform_write_err(b, (size_t)len);
}

static void g_close(Port *p) {
    unsigned pin = (unsigned)p->state[0];
    if (pin < NPINS && pins[pin] == p) pins[pin] = NULL;
    if (p->state[1] != M_OUTPUT && inputs_open > 0) inputs_open--;
    if (!inputs_open) restore_tty();
}

static bool g_can_event(const Port *p) { return p->state[1] != M_OUTPUT && p->state[2] != E_NONE && !stdin_eof; }

const PortDriver gpio_unix_driver = {
    "gpio", TYPE_GPIO, TYPE_EDGE, g_open, g_command, g_close, g_can_event
};

/* ---- input ---- */

/* Pin `pin` is now at `level`: an event if it changed and the edge matches. */
static void set_level(int pin, int level) {
    Port *p = pins[pin];
    if (!p || p->state[1] == M_OUTPUT) return;
    int old = (int)(p->state[3] & 1u);
    if (old == level) return;
    p->state[3] = (p->state[3] & ~1u) | (uint32_t)level;
    uint32_t edge = p->state[2];
    if (!(edge == (level ? E_RISING : E_FALLING) || edge == E_BOTH)) return;
    PortEvent e;
    e.port = (uint16_t)(p->state[3] >> 8);
    e.gen = p->gen;
    e.a = pin;
    e.b = level;
    e.time_ms = (uint32_t)platform_now_ms();
    evq_push(&e);
    platform_notify();
}

static void sleep_ms(unsigned long ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* One script line; true at EOF. */
static bool script_line(void) {
    char buf[128];
    if (!fgets(buf, sizeof buf, stdin)) return true;
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[--n] = 0;
    else if (n == sizeof buf - 1) {                 /* overlong: drop the rest */
        int c;
        while ((c = getchar()) != EOF && c != '\n') {}
        err_line("gpio: input line too long, skipped", "");
        return false;
    }
    if (n && buf[n - 1] == '\r') buf[--n] = 0;
    unsigned long ms;
    int pin, level;
    char tail;
    if (n == 0) return false;
    if (sscanf(buf, "sleep %lu %c", &ms, &tail) == 1) { sleep_ms(ms); return false; }
    if (sscanf(buf, "%d %d %c", &pin, &level, &tail) == 2 && pin >= 0 && pin < NPINS && (level == 0 || level == 1)) {
        set_level(pin, level);
        return false;
    }
    err_line("gpio: bad input line, skipped: ", buf);
    return false;
}

int gpio_unix_wait(uint32_t ms) {
    (void)ms;
    if (tty_mode != 0 || inputs_open == 0 || stdin_eof) return 0;
    if (script_line()) stdin_eof = 1;
    return 1;
}

int gpio_unix_poll_fd(void) {
    return tty_mode == 1 && inputs_open > 0 && !stdin_eof ? STDIN_FILENO : -1;
}

void gpio_unix_read_tty(void) {
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n == 0) { stdin_eof = 1; return; }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 4) { stdin_eof = 1; restore_tty(); return; }   /* Ctrl-D */
        for (int k = 0; k < nkeys; k++) {
            if (keys[k].c != buf[i]) continue;
            set_level(keys[k].pin, 0);
            set_level(keys[k].pin, 1);
        }
    }
}
