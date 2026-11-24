/* unix gpio simulation (spec M3 5.4). Used by platform_unix.c and main.c. */
#ifndef GOGO_GPIO_UNIX_H
#define GOGO_GPIO_UNIX_H

#include <stdint.h>
#include "port.h"

extern const PortDriver gpio_unix_driver;

/* --key <c>=<pin> (tty mode); false when the table is full or pin invalid. */
int gpio_unix_add_key(char c, int pin);

/* platform_wait hook. Script mode (stdin not a tty): when an input port is
 * open and stdin is not at EOF, consumes ONE line and returns 1 (the caller
 * returns at once so the scheduler rescans); else 0. */
int gpio_unix_wait(uint32_t ms);
/* tty mode: stdin fd to poll in platform_wait while input ports are open,
 * else -1; gpio_unix_read_tty consumes what is readable. */
int gpio_unix_poll_fd(void);
void gpio_unix_read_tty(void);

#endif
