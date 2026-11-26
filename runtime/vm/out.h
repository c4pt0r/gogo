/* Buffered text output for println and crash reports. */
#ifndef GOGO_OUT_H
#define GOGO_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct Vm;

typedef struct Out {
    char buf[256];
    size_t n;
    const struct Vm *vm;      /* written with vm_out(vm, stream, ...) */
    int stream;               /* 1 program output, 2 diagnostics; 0 (15.6, B_SPRINT):
                               * into dst, or only counted (n) while dst is NULL */
    char *dst;
    size_t cap;               /* dst's room: nothing is written past it */
    int map;                  /* println (15.6): the depth of a map's entries being printed */
} Out;

void out_init(Out *o, const struct Vm *vm, int stream);
void out_flush(Out *o);
void out_bytes(Out *o, const char *s, size_t n);
void out_str(Out *o, const char *s);
void out_dec(Out *o, uint64_t u, bool neg);   /* decimal u, '-' first if neg */
void out_int(Out *o, int32_t v);
void out_int64(Out *o, int64_t v);

#endif
