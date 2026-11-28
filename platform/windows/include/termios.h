/* termios.h (Windows): the tiny slice of <termios.h> platform/unix/input_unix.c
 * uses, so that the unix input reader (script mode, key mapping, the
 * Kbd/Power/Imu/Ir/Mic/Spk HALs) compiles unchanged on Windows.
 *
 * This is NOT a general termios: it keeps just the line discipline bits the
 * unix code toggles (ICANON, ECHO), VMIN/VTIME and TCSANOW. The calls are
 * implemented in platform/windows/console_win.c over the console input
 * handle: "raw" (ICANON clear) turns off ENABLE_LINE_INPUT and
 * ENABLE_ECHO_INPUT and turns on ENABLE_VIRTUAL_TERMINAL_INPUT, so read()
 * returns each key as its VT (ANSI) sequence, exactly what the unix reader
 * expects (arrows as ESC [ A..D, Enter CR, Backspace DEL, ...).
 */
#ifndef GOGO_WIN_TERMIOS_H
#define GOGO_WIN_TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

#define NCCS 32

#define ICANON 0x0002u   /* canonical (line) input */
#define ECHO   0x0008u   /* echo input characters */
#define TCSANOW 0        /* the only action Windows needs */

#define VMIN  6
#define VTIME 5

struct termios {
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

/* fd is ignored (stdin is the console); returns 0 with c_lflag = ICANON|ECHO
 * so that the unix code's "clear ICANON|ECHO" copy means raw mode. */
int tcgetattr(int fd, struct termios *t);
/* c_lflag with ICANON set: restore the console's input mode saved by the
 * first raw call; ICANON clear: raw keyboard mode. Returns 0, or -1 when
 * stdin is not a console (a pipe: nothing to change). */
int tcsetattr(int fd, int action, const struct termios *t);

#ifdef __cplusplus
}
#endif

#endif /* GOGO_WIN_TERMIOS_H */
