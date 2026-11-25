/* Buffered text output for println and crash reports. */
#ifndef GOGO_OUT_H
#define GOGO_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Out {
    char buf[256];
    size_t n;
    void (*write)(const char *s, size_t n);   /* platform_write or _err */
} Out;

void out_init(Out *o, void (*write)(const char *s, size_t n));
void out_flush(Out *o);
void out_bytes(Out *o, const char *s, size_t n);
void out_str(Out *o, const char *s);
void out_dec(Out *o, uint64_t u, bool neg);   /* decimal u, '-' first if neg */
void out_int(Out *o, int32_t v);
void out_int64(Out *o, int64_t v);

#endif
