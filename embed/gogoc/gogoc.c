/* gogoc.h's functions over the Go ones of main.go (Go has no const). */
#include <stdlib.h>
#include "gogoc.h"
#include "_cgo_export.h"

int gogoc_compile(const char *input, size_t input_len, uint32_t flags, uint8_t **out, size_t *out_len, char **err) {
    return gogocCompile((char *)input, input_len, flags, out, out_len, err);
}

void gogoc_free(void *p) { free(p); }

uint32_t gogoc_image_format(void) { return gogocImageFormat(); }
