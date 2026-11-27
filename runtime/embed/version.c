/* gogo_version and gogo_image_format (gogo.h): stateless, so linked by
 * libgogo and by a board with board ports alike (a driver may check the
 * version wherever it runs). */
#include "../include/gogo.h"
#include "image.h"

uint32_t gogo_version(void) { return GOGO_VERSION; }

uint32_t gogo_image_format(void) { return IMAGE_VERSION; }
