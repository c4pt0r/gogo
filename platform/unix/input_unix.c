#define _POSIX_C_SOURCE 200112L   /* nanosleep, termios */
/* unix input simulation (spec M3 5.4, input §5.6): the only reader of stdin.
 *
 * gpio input ports, the kbd port and the power port are fed from it.
 *
 *  - Script mode (stdin is not a terminal): one line per idle wait while a
 *    port that can produce events is open, so the output depends only on
 *    message causality. Lines:
 *      <pin> <0|1>              gpio input level
 *      sleep <ms>               pause (applies to every line kind)
 *      key <name>               key press, then release
 *      keydown <name> / keyup <name>
 *      battery <mV> <charging> <external>
 *    Names: a-z 0-9 enter esc backspace tab space delete up down left right
 *    f1-f12 ctrl shift alt opt fn, and the punctuation characters
 *    - = [ ] \ ; ' ` , . /   Unknown lines are reported on stderr and
 *    skipped. After EOF no port can produce events (input_unix_live false),
 *    so the VM may exit. Key lines are dropped while kbd is closed.
 *    The simulated battery fails to read until the first battery line; a
 *    battery line is flagged as a new value until it is read.
 *  - tty mode: --key <c>=<pin> maps a character to a gpio pin (it goes only
 *    to gpio); other printable characters, while kbd is open, become a key
 *    press and release (Shift around uppercase letters and shifted symbols);
 *    Enter, Backspace, Tab, Esc and the arrow escape sequences map to their
 *    keys. The terminal is in non-canonical no-echo mode while a port is
 *    open and polled in platform_wait. ISIG stays on: SIGINT/SIGTERM restore
 *    the terminal and then die by the default action. Ctrl-D (0x04) ends the
 *    input, so the VM can exit.
 */
#include "input_unix.h"
#include "gpio_unix.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <signal.h>

#define MAX_KEYS 16
#define QLEN 64u

static int users;                /* open ports that can produce events */
static int tty_mode = -1;        /* -1: not decided yet (first use) */
static int stdin_eof;

static struct { char c; int pin; } keys[MAX_KEYS];
static int nkeys;

static struct termios saved_tio;
static volatile sig_atomic_t tio_saved;

int input_unix_add_key(char c, int pin) {
    if (nkeys >= MAX_KEYS || pin < 0 || pin >= GPIO_UNIX_NPINS) return 0;
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

bool input_unix_tty(void) {
    if (tty_mode < 0) tty_mode = isatty(STDIN_FILENO) ? 1 : 0;
    return tty_mode != 0;
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

void input_unix_attach(void) {
    input_unix_tty();
    users++;
    enter_raw();
}

void input_unix_detach(void) {
    if (users > 0) users--;
    if (!users) restore_tty();
}

bool input_unix_live(void) { return !stdin_eof; }

static void err_line(const char *prefix, const char *s) {
    char b[160];
    int n = snprintf(b, sizeof b, "%s%s\n", prefix, s);
    if (n > 0) platform_write_err(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

/* ---- the keyboard ---- */

enum { H_A = 0x04, H_1 = 0x1E, H_0 = 0x27, H_ENTER = 0x28, H_ESC = 0x29, H_BS = 0x2A, H_TAB = 0x2B,
       H_SPACE = 0x2C, H_MINUS = 0x2D, H_SLASH = 0x38, H_F1 = 0x3A, H_F12 = 0x45, H_DEL = 0x4C,
       H_RIGHT = 0x4F, H_LEFT = 0x50, H_DOWN = 0x51, H_UP = 0x52 };

/* raw index = HID code; fn_code is the Cardputer Fn layer. */
static KbdKey keytab[256];
static void keytab_init(void) {
    static bool done;
    if (done) return;
    done = true;
    for (unsigned c = H_A; c <= H_UP; c++) {
        if ((c >= 0x39 && c < H_F1) || (c > H_F12 && c < H_DEL) || (c > H_DEL && c < H_RIGHT)) continue;
        if (c == 0x32) continue;                                  /* non-US '#': not a key here */
        keytab[c].code = (uint8_t)c;
    }
    keytab[KEY_CTRL].code = KEY_CTRL; keytab[KEY_SHIFT].code = KEY_SHIFT;
    keytab[KEY_ALT].code = KEY_ALT; keytab[KEY_OPT].code = KEY_OPT; keytab[KEY_FN].code = KEY_FN;
    for (unsigned i = 0; i < 10; i++) keytab[H_1 + i].fn_code = (uint8_t)(H_F1 + i);   /* 1..0 -> F1..F10 */
    keytab[0x2D].fn_code = 0x44;                                  /* - -> F11 */
    keytab[0x2E].fn_code = 0x45;                                  /* = -> F12 */
    keytab[0x35].fn_code = H_ESC;                                 /* ` -> Esc */
    keytab[H_BS].fn_code = H_DEL;
    keytab[0x33].fn_code = H_UP;                                  /* ; */
    keytab[0x36].fn_code = H_LEFT;                                /* , */
    keytab[0x37].fn_code = H_DOWN;                                /* . */
    keytab[H_SLASH].fn_code = H_RIGHT;
}

static bool kbd_on, power_on;
static struct { uint8_t code; bool down; } q[QLEN];
static unsigned qh, qt;

static void key_push(unsigned code, bool down) {
    if (!kbd_on || qt - qh >= QLEN) return;       /* closed, or full: dropped like a chip overflow */
    q[qt % QLEN].code = (uint8_t)code;
    q[qt % QLEN].down = down;
    qt++;
    platform_notify();
}

static int k_open(void) {
    keytab_init();
    qh = qt = 0;
    kbd_on = true;
    input_unix_attach();
    return 0;
}
static void k_close(void) {
    if (!kbd_on) return;
    kbd_on = false;
    input_unix_detach();
}
static bool k_pending(void) { return qh != qt; }
static int k_read(uint16_t *raw, bool *down) {
    if (qh == qt) return 0;
    *raw = q[qh % QLEN].code;
    *down = q[qh % QLEN].down;
    qh++;
    return 1;
}

static const char punct[] = "-=[]\\ ;'`,./";            /* HID 0x2D..0x38 (0x32 unused) */

/* HID code of a key name, 0 if unknown. */
static unsigned key_code(const char *s) {
    static const struct { const char *n; uint8_t c; } named[] = {
        { "enter", H_ENTER }, { "esc", H_ESC }, { "backspace", H_BS }, { "tab", H_TAB }, { "space", H_SPACE },
        { "delete", H_DEL }, { "up", H_UP }, { "down", H_DOWN }, { "left", H_LEFT }, { "right", H_RIGHT },
        { "ctrl", KEY_CTRL }, { "shift", KEY_SHIFT }, { "alt", KEY_ALT }, { "opt", KEY_OPT }, { "fn", KEY_FN },
    };
    if (!s[0]) return 0;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return H_A + (unsigned)(c - 'a');
        if (c >= '1' && c <= '9') return H_1 + (unsigned)(c - '1');
        if (c == '0') return H_0;
        const char *p = c == ' ' ? NULL : strchr(punct, c);
        if (p) return H_MINUS + (unsigned)(p - punct);
        return 0;
    }
    if (s[0] == 'f') {
        char *end;
        long n = strtol(s + 1, &end, 10);
        if (*end == 0 && s[1] != '+' && s[1] != '-' && n >= 1 && n <= 12) return H_F1 + (unsigned)n - 1u;
    }
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (strcmp(named[i].n, s) == 0) return named[i].c;
    }
    return 0;
}

/* A typed character: its HID code, and whether Shift is needed; 0 if none. */
static unsigned char_code(unsigned char c, bool *shift) {
    static const char shifted[] = "_+{}| :\"~<>?";          /* US layout, same order as punct */
    *shift = false;
    if (!c) return 0;
    if (c >= 'a' && c <= 'z') return H_A + (unsigned)(c - 'a');
    if (c >= 'A' && c <= 'Z') { *shift = true; return H_A + (unsigned)(c - 'A'); }
    if (c >= '1' && c <= '9') return H_1 + (unsigned)(c - '1');
    if (c == '0') return H_0;
    if (c == ' ') return H_SPACE;
    const char *p = strchr(punct, c);
    if (p && c != ' ') return H_MINUS + (unsigned)(p - punct);
    p = strchr("!@#$%^&*()", c);
    if (p) { *shift = true; return H_1 + (unsigned)(p - "!@#$%^&*()"); }
    p = strchr(shifted, c);
    if (p && c != ' ') { *shift = true; return H_MINUS + (unsigned)(p - shifted); }
    return 0;
}

/* ---- the battery ---- */

static struct { int32_t mv; int8_t charging, external; bool have, pend; } bat;

static void p_enable(bool on) {
    if (on == power_on) return;
    power_on = on;
    if (on) input_unix_attach(); else input_unix_detach();
}
static int p_read(int32_t *mv, int8_t *ch, int8_t *ext) {
    if (!bat.have) return -1;
    *mv = bat.mv; *ch = bat.charging; *ext = bat.external;
    bat.pend = false;
    return 0;
}
static bool p_pending(void) { return bat.pend; }

const InputHw input_unix_hw = {
    keytab, 256, k_open, k_close, k_pending, k_read, p_read, p_pending, input_unix_live, p_enable, true
};

/* ---- script ---- */

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
        err_line("input: line too long, skipped", "");
        return false;
    }
    if (n && buf[n - 1] == '\r') buf[--n] = 0;
    unsigned long ms;
    int pin, level, ch, ext;
    long mv;
    char tail, name[16], verb[16];
    if (n == 0) return false;
    if (sscanf(buf, "sleep %lu %c", &ms, &tail) == 1) { sleep_ms(ms); return false; }
    if (sscanf(buf, "%d %d %c", &pin, &level, &tail) == 2 && pin >= 0 && pin < GPIO_UNIX_NPINS && (level == 0 || level == 1)) {
        gpio_unix_set_level(pin, level);
        return false;
    }
    if (sscanf(buf, "%15s %15s %c", verb, name, &tail) == 2 && strncmp(verb, "key", 3) == 0) {
        unsigned code = key_code(name);
        if (code && strcmp(verb, "key") == 0) { key_push(code, true); key_push(code, false); return false; }
        if (code && strcmp(verb, "keydown") == 0) { key_push(code, true); return false; }
        if (code && strcmp(verb, "keyup") == 0) { key_push(code, false); return false; }
    }
    if (sscanf(buf, "battery %ld %d %d %c", &mv, &ch, &ext, &tail) == 3 && mv >= 0 && mv <= 65535 &&
        (ch == 0 || ch == 1) && (ext == 0 || ext == 1)) {
        bat.mv = (int32_t)mv; bat.charging = (int8_t)ch; bat.external = (int8_t)ext;
        bat.have = bat.pend = true;
        platform_notify();
        return false;
    }
    err_line("input: bad line, skipped: ", buf);
    return false;
}

int input_unix_wait(uint32_t ms) {
    (void)ms;
    if (tty_mode != 0 || users == 0 || stdin_eof) return 0;
    if (script_line()) stdin_eof = 1;
    return 1;
}

int input_unix_poll_fd(void) {
    return tty_mode == 1 && users > 0 && !stdin_eof ? STDIN_FILENO : -1;
}

static void tap(unsigned code, bool shift) {
    if (shift) key_push(KEY_SHIFT, true);
    key_push(code, true);
    key_push(code, false);
    if (shift) key_push(KEY_SHIFT, false);
}

void input_unix_read_tty(void) {
    unsigned char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n == 0) { stdin_eof = 1; return; }
    if (n < 0) return;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 4) { stdin_eof = 1; restore_tty(); return; }   /* Ctrl-D */
        bool mapped = false;
        for (int k = 0; k < nkeys; k++) {
            if ((unsigned char)keys[k].c != c) continue;
            gpio_unix_set_level(keys[k].pin, 0);
            gpio_unix_set_level(keys[k].pin, 1);
            mapped = true;
        }
        if (mapped || !kbd_on) continue;                       /* --key characters go only to gpio */
        if (c == 0x1b && i + 2 < n && buf[i + 1] == '[' && buf[i + 2] >= 'A' && buf[i + 2] <= 'D') {
            static const uint8_t arrows[4] = { H_UP, H_DOWN, H_RIGHT, H_LEFT };
            tap(arrows[buf[i + 2] - 'A'], false);
            i += 2;
            continue;
        }
        bool shift = false;
        unsigned code = c == '\r' || c == '\n' ? H_ENTER : c == 0x1b ? H_ESC : c == 0x7f || c == 8 ? H_BS :
                        c == '\t' ? H_TAB : char_code(c, &shift);
        if (!code) continue;
        tap(code, shift);
    }
}
