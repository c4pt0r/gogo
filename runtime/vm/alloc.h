/* Small-block allocator: size classes of 8 bytes up to 256, carved from
 * 4 KB chunks; larger requests go straight to the platform. No per-block
 * header, so callers must pass the original size to va_free. Each Vm owns
 * its allocator state (Vm.va). */
#ifndef GOGO_ALLOC_H
#define GOGO_ALLOC_H

#include <stddef.h>

#define VA_GRAIN      8u
#define VA_MAX_SMALL  256u
#define VA_CLASSES    (VA_MAX_SMALL / VA_GRAIN)
#define VA_CHUNK      4096u

struct Vm;
struct VaFree;

/* Scalars first: Vm embeds this ahead of its large arrays, so they stay at
 * short load offsets on the MCU. */
typedef struct VaHeap {
    char *chunk_cur, *chunk_end;              /* unused tail of newest chunk */
    size_t used, limit, peak;
    struct VaFree *free_list[VA_CLASSES + 1]; /* indexed by size / VA_GRAIN */
} VaHeap;

void *va_alloc(struct Vm *vm, size_t bytes);         /* NULL on failure or over limit */
void va_free(struct Vm *vm, void *p, size_t bytes);
size_t va_used_bytes(const struct Vm *vm);           /* handed out, rounded to class size */
void va_set_limit(struct Vm *vm, size_t bytes);      /* 0 = unlimited (for OOM tests) */
size_t va_peak_bytes(const struct Vm *vm);           /* highest va_used_bytes since reset */
void va_reset_peak(struct Vm *vm);                   /* peak = current use */
void va_reset(struct Vm *vm);                        /* forget all state (tests) */

#endif
