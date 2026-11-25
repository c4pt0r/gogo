/* Host shim: ESP-IDF's mbedTLS tree has bignum.c include "mbedtls/bignum.h",
 * which ESP-IDF supplies from its port/ directory. */
#include "mbedtls/private/bignum.h"
