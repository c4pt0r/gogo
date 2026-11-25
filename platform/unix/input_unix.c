#define _POSIX_C_SOURCE 200112L   /* nanosleep, termios */
/* unix input simulation (spec M3 5.4, input §5.6): the only reader of stdin.
 *
 * gpio input ports, the kbd, power, imu, ir_rx and mic ports are fed from
 * it; ir_tx prints `ir tx: <n> values` on stderr for each frame and is busy
 * for as long as the frame would take (the sum of its timings); spk prints
 * `spk: <n> bytes` for each write (at most 4096 bytes each) and is busy for
 * as long as they take to play.
 *
 *  - Script mode (stdin is not a terminal): one line per idle wait while a
 *    port that can produce events is open, so the output depends only on
 *    message causality. Lines:
 *      <pin> <0|1>              gpio input level
 *      sleep <ms>               pause (applies to every line kind)
 *      key <name>               key press, then release
 *      keydown <name> / keyup <name>
 *      battery <mV> <charging> <external>
 *      imu sample <gx> <gy> <gz> <ax> <ay> <az>   int16 each; queued, handed to
 *                                                 the driver at its next poll
 *      imu gesture <kind> [value]   tap doubletap tripletap orientation flat
 *                                   anymotion nomotion step (value: the
 *                                   orientation number or step count)
 *      ir rx <us> <us> ...      one received frame (1 to 512 values, each
 *                               1..32767; a line is a whole frame, GapUs
 *                               plays no part), queued (4 frames) while
 *                               ir_rx is open, handed over at its next poll
 *      mic tone <hz> <ms>       a sine (hz 1..8000 and under Rate/2, ms
 *                               1..600000) at amplitude
 *                               1000 << Gain/6; queued (16 lines) while mic
 *                               is open, handed over at its next poll; a line
 *                               that finds the queue full counts in Dropped
 *      mic silence <ms>         zeros, likewise
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
    if (n <= 0) return;
    if (n >= (int)sizeof b) {                    /* cut (a long `ir rx` line): keep the newline */
        n = (int)sizeof b - 1;
        memcpy(b + n - 4, "...\n", 4);
    }
    platform_write_err(b, (size_t)n);
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

/* ---- the motion sensor ---- */

/* `imu sample` lines queue samples (the FIFO); imu_read_fifo releases what
 * is queued, so a sample reaches the driver at the next poll after its
 * line. Samples are kept only while the port is open at a RateHz above 0;
 * `imu gesture` lines accumulate until imu_read_gestures. */
#define IMU_QLEN 256u
enum { IMU_SAMPLE = 12 };
static struct {
    bool on;
    uint16_t rate;
    uint8_t mask;
    uint8_t q[IMU_QLEN][IMU_SAMPLE];
    unsigned qh, qt;
    ImuGestures g;
} imu;

static int i_open(uint16_t rate, uint8_t mask) {
    memset(&imu, 0, sizeof imu);
    imu.on = true;
    imu.rate = rate;
    imu.mask = mask;
    input_unix_attach();
    return 0;
}
static void i_close(void) {
    if (!imu.on) return;
    imu.on = false;
    input_unix_detach();
}
static int i_read_fifo(uint8_t *buf, uint32_t cap) {
    uint32_t n = 0;
    while (imu.qh != imu.qt && n + IMU_SAMPLE <= cap) {
        memcpy(buf + n, imu.q[imu.qh % IMU_QLEN], IMU_SAMPLE);
        imu.qh++;
        n += IMU_SAMPLE;
    }
    return (int)n;
}
static int i_read_gestures(ImuGestures *g) {
    *g = imu.g;
    imu.g.bits = 0;
    return 0;
}

static void imu_push_sample(const int *v) {
    if (!imu.on || !imu.rate || imu.qt - imu.qh >= IMU_QLEN) return;   /* full: dropped like a FIFO overflow */
    uint8_t *d = imu.q[imu.qt % IMU_QLEN];
    for (int i = 0; i < 6; i++) {
        d[2 * i] = (uint8_t)(v[i] & 0xFF);
        d[2 * i + 1] = (uint8_t)((unsigned)v[i] >> 8 & 0xFF);
    }
    imu.qt++;
}

static void imu_push_gesture(unsigned bit, int value) {
    if (!imu.on || !(imu.mask & bit)) return;
    imu.g.bits = (uint8_t)(imu.g.bits | bit);
    if (bit == IMU_ORIENTATION) imu.g.orient = (uint8_t)value;
    if (bit == IMU_STEP) imu.g.steps = (uint32_t)value;
}

/* imu.Tap .. imu.Step bit of a gesture name, 0 if unknown. */
static unsigned gesture_bit(const char *s) {
    static const char *const names[8] = { "tap", "doubletap", "tripletap", "orientation", "flat", "anymotion",
                                          "nomotion", "step" };
    for (unsigned i = 0; i < 8; i++) {
        if (strcmp(names[i], s) == 0) return 1u << i;
    }
    return 0;
}

/* ---- infrared ---- */

#define IR_SIM_FRAMES 4u
#define IR_SIM_MAX 512u
static struct {
    bool rx_on;
    uint16_t f[IR_SIM_FRAMES][IR_SIM_MAX];
    uint16_t n[IR_SIM_FRAMES];
    uint32_t at[IR_SIM_FRAMES];   /* ms: when its line was read (the frame's start) */
    unsigned h, t;
    uint64_t tx_end;           /* ms: the frame being sent ends */
} irs;

static int ir_start(const uint8_t *t, uint32_t n, uint32_t carrier, uint8_t duty) {
    (void)carrier; (void)duty;
    uint64_t us = 0;
    for (uint32_t i = 0; i < n; i++) us += (uint32_t)t[2 * i] | (uint32_t)t[2 * i + 1] << 8;
    irs.tx_end = platform_now_ms() + (us + 999u) / 1000u;
    char b[48];
    int k = snprintf(b, sizeof b, "ir tx: %lu values\n", (unsigned long)n);
    if (k > 0) platform_write_err(b, (size_t)k);
    return 0;
}
static bool ir_busy(void) { return platform_now_ms() < irs.tx_end; }
static int ir_rx_open(uint32_t gap_us) {
    (void)gap_us;
    irs.h = irs.t = 0;
    irs.rx_on = true;
    input_unix_attach();
    return 0;
}
static void ir_rx_close(void) {
    if (!irs.rx_on) return;
    irs.rx_on = false;
    input_unix_detach();
}
static int ir_rx_read(uint16_t *buf, uint32_t cap, uint32_t *start_ms) {
    if (irs.h == irs.t) return 0;
    unsigned k = irs.h++ % IR_SIM_FRAMES;
    uint32_t n = irs.n[k];
    *start_ms = irs.at[k];
    memcpy(buf, irs.f[k], (n < cap ? n : cap) * sizeof *buf);
    return (int)n;
}

/* An `ir rx` line's values (after "ir rx"); false if malformed. */
static bool ir_push_frame(const char *s) {
    static uint16_t v[IR_SIM_MAX];
    uint32_t n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char *end;
        long x = strtol(s, &end, 10);
        if (end == s || (*end && *end != ' ' && *end != '\t') || x < 1 || x > 32767 || n == IR_SIM_MAX) return false;
        v[n++] = (uint16_t)x;
        s = end;
    }
    if (!n) return false;
    if (!irs.rx_on || irs.t - irs.h >= IR_SIM_FRAMES) return true;   /* closed, or full: lost like on air */
    unsigned k = irs.t++ % IR_SIM_FRAMES;
    memcpy(irs.f[k], v, n * sizeof v[0]);
    irs.n[k] = (uint16_t)n;
    irs.at[k] = (uint32_t)platform_now_ms();
    return true;
}

/* ---- audio ---- */

/* `mic tone` / `mic silence` lines queue segments (MIC_SEGS) while mic is
 * open; mic_read renders them as PCM on demand, so a segment of any length
 * costs a few bytes and reaches the driver at its next poll. A line that
 * finds the queue full is lost and reported as an overflow (Dropped).
 * Tones are integer sines from a quarter-wave table (no floating point):
 * amplitude 1000 at Gain 0, doubled every 6 dB (16000 at 24 dB), the phase
 * running on across segments. */
#define MIC_SEGS 16u
static const int16_t quarter_sine[65] = {   /* round(32767 * sin(i * pi / 128)), i = 0..64 */
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
    22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510,
    28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285,
    32412, 32521, 32609, 32678, 32728, 32757, 32767,
};
static struct {
    bool on;
    uint32_t rate, amp, phase;     /* phase: 2^32 per cycle */
    uint32_t lost;                 /* bytes of lines that found the queue full */
    struct { uint16_t hz; uint32_t left; } q[MIC_SEGS];   /* hz 0: silence; left in samples */
    unsigned h, t;
} mics;

/* sin(phase) * 32767, phase 2^32 per cycle, from the top 8 bits. */
static int32_t sine(uint32_t phase) {
    uint32_t i = phase >> 24;
    int32_t v = quarter_sine[(i & 64u) ? 64u - (i & 63u) : (i & 63u)];
    return (i & 128u) ? -v : v;
}

static int m_open(uint32_t rate, uint8_t gain_db) {
    memset(&mics, 0, sizeof mics);
    mics.on = true;
    mics.rate = rate;
    mics.amp = 1000u << (gain_db / 6u);
    input_unix_attach();
    return 0;
}
static void m_close(void) {
    if (!mics.on) return;
    mics.on = false;
    input_unix_detach();
}
static int m_read(uint8_t *buf, uint32_t cap, uint32_t *lost) {
    *lost = mics.lost;
    mics.lost = 0;
    uint32_t n = 0;
    while (mics.h != mics.t && n + 2u <= cap) {
        unsigned k = mics.h % MIC_SEGS;
        uint32_t hz = mics.q[k].hz, step = (uint32_t)(((uint64_t)hz << 32) / mics.rate);
        for (; mics.q[k].left && n + 2u <= cap; mics.q[k].left--, n += 2u) {
            int32_t v = hz ? sine(mics.phase) * (int32_t)mics.amp / 32767 : 0;
            mics.phase += step;
            buf[n] = (uint8_t)((uint32_t)v & 0xFFu);
            buf[n + 1u] = (uint8_t)((uint32_t)v >> 8 & 0xFFu);
        }
        if (!mics.q[k].left) mics.h++;
    }
    return (int)n;
}

/* A `mic tone <hz> <ms>` (hz 1..8000) or `mic silence <ms>` (hz 0) line;
 * ms 1..600000. Dropped while mic is closed; false (a bad line) for a tone
 * at or above half the open Rate, which would alias. */
static bool mic_push(uint32_t hz, uint32_t ms) {
    if (!mics.on) return true;
    if (hz * 2u >= mics.rate) return false;
    uint32_t n = ms * (mics.rate / 1000u);
    if (mics.t - mics.h >= MIC_SEGS) {         /* full: lost like a buffer overflow */
        mics.lost += n * 2u;
        return true;
    }
    unsigned k = mics.t++ % MIC_SEGS;
    mics.q[k].hz = (uint16_t)hz;
    mics.q[k].left = n;
    platform_notify();
    return true;
}

/* The speaker takes at most SPK_SIM_WRITE bytes per write (so an Out is
 * written in parts, the same way every run) into an unbounded buffer,
 * prints `spk: <n> bytes` on stderr for each, and plays in real time at
 * Rate from the write that found it idle. */
#define SPK_SIM_WRITE 4096u
static struct {
    uint32_t rate;
    uint64_t written, base;        /* samples: all written; those written before `start` */
    uint64_t start;                /* ms: playing (re)started */
} spks;

static uint64_t s_played(void) {
    uint64_t can = spks.base + (platform_now_ms() - spks.start) * spks.rate / 1000u;
    return can < spks.written ? can : spks.written;
}
static int s_open(uint32_t rate, uint8_t volume) {
    (void)volume;
    memset(&spks, 0, sizeof spks);
    spks.rate = rate;
    return 0;
}
static int s_write(const uint8_t *d, uint32_t len) {
    (void)d;
    uint32_t n = len < SPK_SIM_WRITE ? len : SPK_SIM_WRITE;
    if (s_played() == spks.written) {          /* idle: starts playing now */
        spks.base = spks.written;
        spks.start = platform_now_ms();
    }
    spks.written += n / 2u;
    char b[32];
    int k = snprintf(b, sizeof b, "spk: %lu bytes\n", (unsigned long)n);
    if (k > 0) platform_write_err(b, (size_t)k);
    return (int)n;
}
static uint32_t s_queued(void) { return (uint32_t)((spks.written - s_played()) * 2u); }
static void s_close(void) { memset(&spks, 0, sizeof spks); }

const InputHw input_unix_hw = {
    keytab, 256, k_open, k_close, k_pending, k_read, p_read, p_pending, input_unix_live, p_enable, true,
    i_open, i_close, i_read_fifo, i_read_gestures,
    ir_start, ir_busy, ir_rx_open, ir_rx_read, ir_rx_close, NULL, NULL,   /* no ir_tx_open / ir_tx_close */
    m_open, m_read, m_close, s_open, s_write, s_queued, s_close,
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
    static char buf[4096];      /* an `ir rx` line of 512 values fits */
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
    {
        int v[6], val = 0, got;
        got = sscanf(buf, "imu sample %d %d %d %d %d %d %c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail);
        if (got == 6) {
            int ok = 1;
            for (int i = 0; i < 6; i++) ok &= v[i] >= -32768 && v[i] <= 32767;
            if (ok) { imu_push_sample(v); return false; }
        }
        got = sscanf(buf, "imu gesture %15s %d %c", name, &val, &tail);
        if (got >= 1 && got <= 2 && gesture_bit(name) && (got == 1 || val >= 0)) {
            imu_push_gesture(gesture_bit(name), val);
            return false;
        }
    }
    if (strncmp(buf, "ir rx ", 6) == 0 && ir_push_frame(buf + 6)) return false;
    {
        unsigned long hz, mms;
        if (sscanf(buf, "mic tone %lu %lu %c", &hz, &mms, &tail) == 2 && hz >= 1 && hz <= 8000 && mms >= 1 &&
            mms <= 600000) {
            if (mic_push((uint32_t)hz, (uint32_t)mms)) return false;
        }
        if (sscanf(buf, "mic silence %lu %c", &mms, &tail) == 1 && mms >= 1 && mms <= 600000) {
            mic_push(0, (uint32_t)mms);
            return false;
        }
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
