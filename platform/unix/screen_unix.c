#define _POSIX_C_SOURCE 200112L   /* isatty, write */
/* The unix "screen" port's display (runtime/screen): the grid drawn on the
 * terminal with ANSI escape sequences, or dumped as text.
 *
 *  - ansi (the default when stdout is a terminal): the alternate screen,
 *    each changed row redrawn at its place with SGR colours (the palette's
 *    16 colours: 30-37 / 90-97 and 40-47 / 100-107; bold 1, underline 4,
 *    reverse 7) and the terminal's cursor where the grid's is. Closing the
 *    port (or the VM ending, or SIGINT / SIGTERM) leaves the alternate
 *    screen. println output is not kept apart from the grid.
 *  - text (the default otherwise, for goldens): after every frame that
 *    changed something, the whole grid on stdout (not the blank grid of the
 *    open):
 *
 *      screen 1 40x11
 *      +----------------------------------------+
 *      |Hello                                   | 33333
 *      ...
 *      +----------------------------------------+
 *      cursor 5 0
 *
 *    The number counts the dumps. After a row come its style characters
 *    (screen_style_char), from its first cell to its last non-default one,
 *    when it has any; the "cursor x y" line is there while the cursor is
 *    shown.
 *  - none: nothing is drawn (the port still works).
 *
 * The grid is 40 x 11 by default (the boards' size); Config may ask for up
 * to 255 x 32. */
#include "screen_unix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static int mode = -1;           /* SCREEN_UNIX_*; -1: decided at the first open */
static int active;              /* a grid is on the terminal (ansi) */
static int fresh;               /* text: the open's blank grid is not dumped */
static unsigned dumps;

bool screen_unix_set_mode(const char *name) {
    if (strcmp(name, "ansi") == 0) mode = SCREEN_UNIX_ANSI;
    else if (strcmp(name, "text") == 0) mode = SCREEN_UNIX_TEXT;
    else if (strcmp(name, "none") == 0) mode = SCREEN_UNIX_NONE;
    else return false;
    return true;
}

static const char leave_seq[] = "\033[0m\033[?25h\033[?1049l";

void screen_unix_restore(void) {   /* async-signal-safe */
    if (active) {
        ssize_t r = write(STDOUT_FILENO, leave_seq, sizeof leave_seq - 1u);
        (void)r;
        active = 0;
    }
}

/* SIGINT / SIGTERM while no handler of input_unix.c is set (it calls
 * screen_unix_restore itself). */
static void on_signal(int sig) {
    screen_unix_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void at_exit(void) {
    fflush(stdout);
    screen_unix_restore();
}

static int u_open(uint8_t cols, uint8_t rows) {
    (void)cols;
    (void)rows;
    if (mode < 0) mode = isatty(STDOUT_FILENO) ? SCREEN_UNIX_ANSI : SCREEN_UNIX_TEXT;
    fresh = 1;
    if (mode == SCREEN_UNIX_ANSI) {
        static int hooked;
        if (!hooked) {
            hooked = 1;
            atexit(at_exit);
            void (*prev)(int) = signal(SIGINT, on_signal);
            if (prev != SIG_DFL) signal(SIGINT, prev);   /* input_unix.c's: it restores us */
            prev = signal(SIGTERM, on_signal);
            if (prev != SIG_DFL) signal(SIGTERM, prev);
        }
        fputs("\033[?1049h\033[?25l\033[0m\033[2J", stdout);
        fflush(stdout);
        active = 1;
    }
    return 0;
}

static void sgr(uint32_t c) {
    uint32_t fg = screen_cell_fg(c), bg = screen_cell_bg(c);
    printf("\033[0;%u;%u%s%s%sm", fg < 8 ? 30u + fg : 90u + fg - 8u, bg < 8 ? 40u + bg : 100u + bg - 8u,
           (c & SCREEN_F_BOLD) ? ";1" : "", (c & SCREEN_F_UNDERLINE) ? ";4" : "", (c & SCREEN_F_REVERSE) ? ";7" : "");
}

#define STYLE_BITS (0xFFu << SCREEN_FG_SHIFT | SCREEN_F_BOLD | SCREEN_F_UNDERLINE | SCREEN_F_REVERSE)

static void present_ansi(const ScreenGrid *g, uint32_t dirty) {
    char ch[4];
    for (int r = 0; r < g->rows; r++) {
        if (!(dirty & (1u << r))) continue;
        printf("\033[%d;1H", r + 1);
        uint32_t last = UINT32_MAX;
        for (int x = 0; x < g->cols; x++) {
            uint32_t c = screen_cell(g, x, r);
            if (c & SCREEN_F_CONT) continue;
            if ((c & STYLE_BITS) != last) {
                sgr(c);
                last = c & STYLE_BITS;
            }
            uint32_t cp = screen_cell_cp(c);
            fwrite(ch, 1, screen_utf8(cp < 0x20u ? ' ' : cp, ch), stdout);
        }
    }
    fputs("\033[0m", stdout);
    if (g->cursor_on && g->kx < g->cols && g->ky < g->rows) printf("\033[%d;%dH\033[?25h", g->ky + 1, g->kx + 1);
    else fputs("\033[?25l", stdout);
    fflush(stdout);
}

static void present_text(const ScreenGrid *g) {
    char row[SCREEN_MAX_COLS * 3u + 1u];
    printf("screen %u %ux%u\n+", ++dumps, (unsigned)g->cols, (unsigned)g->rows);
    for (int x = 0; x < g->cols; x++) putchar('-');
    puts("+");
    for (int r = 0; r < g->rows; r++) {
        screen_row_utf8(g, r, row, sizeof row);
        printf("|%s|", row);
        int end = 0;
        for (int x = 0; x < g->cols; x++)
            if (screen_style_char(screen_cell(g, x, r)) != '.') end = x + 1;
        if (end) {
            putchar(' ');
            for (int x = 0; x < end; x++) putchar(screen_style_char(screen_cell(g, x, r)));
        }
        putchar('\n');
    }
    putchar('+');
    for (int x = 0; x < g->cols; x++) putchar('-');
    puts("+");
    if (g->cursor_on) printf("cursor %u %u\n", (unsigned)g->kx, (unsigned)g->ky);
}

static void u_present(const ScreenGrid *g, uint32_t dirty) {
    if (mode == SCREEN_UNIX_ANSI) present_ansi(g, dirty);
    else if (mode == SCREEN_UNIX_TEXT && !fresh) present_text(g);
    fresh = 0;
}

static void u_close(void) {
    if (mode == SCREEN_UNIX_ANSI) {
        fflush(stdout);
        screen_unix_restore();
    }
}

static const ScreenHw unix_screen_hw = { 40, 11, SCREEN_MAX_COLS, SCREEN_MAX_ROWS, SCREEN_A_ALL, u_open, u_present, u_close };

static int s_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    return screen_port_open(vm, p, cfg, n, &unix_screen_hw);
}

const PortDriver screen_unix_driver = SCREEN_PORT_DRIVER(s_open);
