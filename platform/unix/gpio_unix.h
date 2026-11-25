/* unix gpio simulation (spec M3 5.4). Used by platform_unix.c and main.c. */
#ifndef GOGO_GPIO_UNIX_H
#define GOGO_GPIO_UNIX_H

#include <stdint.h>
#include "port.h"

extern const PortDriver gpio_unix_driver;

#define GPIO_UNIX_NPINS 64

/* Pin `pin` is now at `level` (0 or 1): an event if it changed and an input
 * port on it listens for that edge. Called by the stdin reader (input_unix.c). */
void gpio_unix_set_level(int pin, int level);

#endif
