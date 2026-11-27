/* Refused board ports (named.c says which). */
#include "gogo.h"

extern const gogo_port_driver named_driver;

GOGO_BOARD_PORT("alpha", &named_driver, "alpha (second)");
GOGO_BOARD_PORT("gpio", &named_driver, "not the gpio driver");
GOGO_BOARD_PORT("a_name_longer_than_thirty_one_bytes", &named_driver, "too long");
