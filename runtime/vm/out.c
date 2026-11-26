#include "out.h"
#include "host.h"
#include <string.h>

void out_init(Out *o, const struct Vm *vm, int stream) {
    o->n = 0;
    o->vm = vm;
    o->stream = stream;
    o->dst = NULL;
    o->map = -1;
}

void out_flush(Out *o) {
    if (o->n && o->stream) {
        vm_out(o->vm, o->stream, o->buf, o->n);
        o->n = 0;
    }
}

void out_bytes(Out *o, const char *s, size_t n) {
    if (!o->stream) {                      /* into memory, or counted */
        if (o->dst && o->n + n <= o->cap) memcpy(o->dst + o->n, s, n);
        o->n += n;
        return;
    }
    if (n > sizeof o->buf - o->n) {
        out_flush(o);
        if (n > sizeof o->buf) {           /* too big to buffer: write through */
            vm_out(o->vm, o->stream, s, n);
            return;
        }
    }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
}

void out_str(Out *o, const char *s) { out_bytes(o, s, strlen(s)); }

/* 64-bit division (a slow software routine on the 32-bit MCU targets)
 * only while the value does not fit 32 bits: println of ints is hot. */
void out_dec(Out *o, uint64_t u, bool neg) {
    char tmp[21];
    int i = (int)sizeof tmp;
    while (u >> 32) {
        tmp[--i] = (char)('0' + u % 10u);
        u /= 10u;
    }
    uint32_t w = (uint32_t)u;
    do {
        tmp[--i] = (char)('0' + w % 10u);
        w /= 10u;
    } while (w);
    if (neg) tmp[--i] = '-';
    out_bytes(o, tmp + i, sizeof tmp - (size_t)i);
}

void out_int(Out *o, int32_t v) { out_int64(o, v); }

void out_int64(Out *o, int64_t v) {
    out_dec(o, v < 0 ? 0u - (uint64_t)v : (uint64_t)v, v < 0);
}
