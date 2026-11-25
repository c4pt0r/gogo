#ifndef GOGO_CAPS_UNIX_H
#define GOGO_CAPS_UNIX_H
#include <stdint.h>
/* Test only (gogovm --caps <hex>): override platform_caps(). */
void platform_unix_set_caps(uint32_t caps);
#endif
