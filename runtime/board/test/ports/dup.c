/* Refused board ports (named.c says which, and one too new below). */
#include "gogo.h"

extern const gogo_port_driver named_driver;

GOGO_BOARD_PORT("test.alpha", &named_driver, "alpha (second)");
GOGO_BOARD_PORT("gpio", &named_driver, "not the gpio driver");
GOGO_BOARD_PORT("test.a_name_longer_than_31_bytes", &named_driver, "too long");
GOGO_BOARD_PORT("test.bad name", &named_driver, "a space");

/* An entry from a driver compiled against a newer gogo.h (its minor one
 * more than this one's): refused, with the versions said. */
static const gogo_board_port newer = { sizeof(gogo_board_port), GOGO_VERSION + 0x100u, "test.newer", &named_driver,
                                       (void *)"newer" };
static const gogo_board_port *const newer_ref GOGO_BOARD_PORT_ATTR = &newer;
