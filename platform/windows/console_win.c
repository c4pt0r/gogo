/* console_win.c: the Windows console layer of gogovm.
 *
 * Two jobs, both invisible to the shared unix sources:
 *
 *  1. The output side: a console window understands ANSI escape sequences
 *     only once ENABLE_VIRTUAL_TERMINAL_PROCESSING is on, and the screen
 *     port draws the grid with UTF-8 box-drawing characters, so the code
 *     page must be UTF-8 (65001). Both are set once at startup for the
 *     standard output handle; a redirected stdout has no console mode and is
 *     left alone (then platform/unix/screen_unix.c's default is the text
 *     dump, which is what the goldens compare).
 *
 *  2. The input side: the <termios.h> shim of platform/windows/include,
 *     implemented over the console input handle. "Raw" (ICANON cleared by
 *     input_unix.c) turns off line input and echo and turns on
 *     ENABLE_VIRTUAL_TERMINAL_INPUT. ENABLE_PROCESSED_INPUT stays on, so
 *     Ctrl-C still raises the CRT's SIGINT and the fatal-signal handler
 *     restores the console before the process dies. stdin is switched to
 *     binary for the raw read so Ctrl-Z is not an end-of-file and CRLF is
 *     not folded.
 */
#include <windows.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "termios.h"

static DWORD saved_in_mode;      /* the console input mode before the first raw */
static int in_mode_saved;

/* ---- output: VT sequences and UTF-8 ---- */

void platform_win_console_init(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    /* The screen port and stray UTF-8 program output: 65001 is UTF-8. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    /* Output is the program's bytes, as on unix: no CRLF translation and no
     * Ctrl-Z end-of-file games. gogo's output (goldens, the tests that
     * compare it with Go's) is LF-only; msvcrt's text mode would turn every
     * "\n" into "\r\n" and make a piped run differ from the same run on a
     * POSIX host. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

__attribute__((constructor)) static void console_ctor(void) { platform_win_console_init(); }

/* ---- input: the termios shim ---- */

int tcgetattr(int fd, struct termios *t) {
    (void)fd;
    if (!t) return -1;
    memset(t, 0, sizeof *t);
    t->c_lflag = ICANON | ECHO;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
    return 0;
}

int tcsetattr(int fd, int action, const struct termios *t) {
    (void)fd;
    (void)action;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleMode(in, &mode)) return -1;      /* a pipe: nothing to change */
    if (!t || (t->c_lflag & ICANON)) {              /* restore */
        if (in_mode_saved) {
            SetConsoleMode(in, saved_in_mode);
            in_mode_saved = 0;
        }
        _setmode(_fileno(stdin), _O_TEXT);
        return 0;
    }
    if (!in_mode_saved) {                           /* raw: save once */
        saved_in_mode = mode;
        in_mode_saved = 1;
        _setmode(_fileno(stdin), _O_BINARY);
    }
    {
        DWORD raw = mode;
        raw &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        raw |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(in, raw)) {
            /* A console too old for VT input: line input off is still better
             * than nothing (keys arrive one at a time then). */
            SetConsoleMode(in, mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
        }
    }
    return 0;
}

/* True when a key is waiting: platform_win.c polls this instead of the unix
 * poll() over stdin (a console handle is not a select()able byte stream).
 * Reads no character. */
int platform_win_kbd_ready(void) { return _kbhit(); }
