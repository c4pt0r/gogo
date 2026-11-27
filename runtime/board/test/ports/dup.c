/* Refused board ports (named.c says which). */
#include "gogo.h"

extern const gogo_port_driver named_driver;

GOGO_BOARD_PORT("test.alpha", &named_driver, "alpha (second)");
GOGO_BOARD_PORT("gpio", &named_driver, "not the gpio driver");
GOGO_BOARD_PORT("test.a_name_longer_than_31_bytes", &named_driver, "too long");
GOGO_BOARD_PORT("test.bad name", &named_driver, "a space");
