/* unix input simulation: the one reader of stdin shared by the gpio, kbd and
 * power ports, and the unix input devices' HALs (spec input §5.6). Used by gpio_unix.c,
 * platform_unix.c and main.c. */
#ifndef GOGO_INPUT_UNIX_H
#define GOGO_INPUT_UNIX_H

#include <stdint.h>
#include <stdbool.h>
#include "input.h"

/* The simulated keyboard, battery, motion sensor, infrared and audio;
 * main.c's board lists a driver instance over each (INPUT_*_DRIVER). */
extern const KbdHal input_unix_kbd;
extern const PowerHal input_unix_power;
extern const ImuHal input_unix_imu;
extern const IrTxHal input_unix_ir_tx;
extern const IrRxHal input_unix_ir_rx;
extern const MicHal input_unix_mic;
extern const SpkHal input_unix_spk;

/* --key <c>=<pin> (tty mode): sends character c to gpio pin only; false when
 * the table is full or the pin invalid. */
int input_unix_add_key(char c, int pin);

/* A port that can produce events opened / closed (gpio input; kbd and power
 * through their HAL hooks). While any is open stdin is read; in tty mode
 * the terminal is in raw mode. */
void input_unix_attach(void);
void input_unix_detach(void);

/* f (async-signal-safe) runs in the SIGINT / SIGTERM handler that restores
 * the terminal, before the default action: the screen port leaves the
 * alternate screen there. */
void input_unix_on_fatal(void (*f)(void));

/* True when stdin is a terminal (decided on first use). */
bool input_unix_tty(void);
/* False once stdin hit EOF (or Ctrl-D): no port can produce events. */
bool input_unix_live(void);

/* platform_wait hook. Script mode (stdin not a tty): when a port is open and
 * stdin is not at EOF, consumes ONE line and returns 1 (the caller returns at
 * once so the scheduler rescans); else 0. */
int input_unix_wait(uint32_t ms);
/* tty mode: stdin fd to poll in platform_wait while a port is open, else -1;
 * input_unix_read_tty consumes what is readable. */
int input_unix_poll_fd(void);
void input_unix_read_tty(void);

#endif
