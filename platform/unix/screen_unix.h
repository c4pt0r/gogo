/* The unix "screen" port's display: ANSI terminal or text dump
 * (screen_unix.c). */
#ifndef GOGO_SCREEN_UNIX_H
#define GOGO_SCREEN_UNIX_H

#include <stdbool.h>
#include "screen_port.h"

enum { SCREEN_UNIX_NONE = 0, SCREEN_UNIX_TEXT = 1, SCREEN_UNIX_ANSI = 2 };

/* --screen ansi|text|none; false for another name. Without it: ansi when
 * stdout is a terminal, else text. */
bool screen_unix_set_mode(const char *name);

/* Leaves the alternate screen if the grid is on the terminal; async-signal
 * safe (input_unix.c's fatal-signal handler calls it). */
void screen_unix_restore(void);

extern const PortDriver screen_unix_driver;

#endif
