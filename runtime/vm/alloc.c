#include "alloc.h"
#include "actor.h"

typedef struct VaFree { struct VaFree *next; } FreeBlock;

static size_t round_up(size_t n) {
    if (n == 0) n = 1;
    return (n + VA_GRAIN - 1) & ~(size_t)(VA_GRAIN - 1);
}

void *va_alloc(Vm *vm, size_t bytes) {
    VaHeap *h = &vm->va;
    size_t size = bytes > VA_MAX_SMALL ? bytes : round_up(bytes);
    if (h->limit && h->used + size > h->limit) return NULL;

    char *p;
    if (bytes > VA_MAX_SMALL) {
        p = vm_chunk_alloc(vm, size + VA_HDR);
    } else {
        size_t cls = (size + VA_HDR) / VA_GRAIN;
        if (h->free_list[cls]) {
            p = (char *)h->free_list[cls];
            h->free_list[cls] = h->free_list[cls]->next;
        } else {
            if ((size_t)(h->chunk_end - h->chunk_cur) < size + VA_HDR) {
                /* Tail of the old chunk is abandoned; at most 248 bytes (256 with VA_HDR). */
                char *c = vm_chunk_alloc(vm, VA_CHUNK);
                if (!c) return NULL;
                h->chunk_cur = c;
                h->chunk_end = c + VA_CHUNK;
            }
            p = h->chunk_cur;
            h->chunk_cur += size + VA_HDR;
        }
    }
    if (!p) return NULL;
    h->used += size;
    if (h->used > h->peak) h->peak = h->used;
#if GOGO_DEBUG
    *(size_t *)(void *)p = size;
#endif
    return p + VA_HDR;
}

void va_free(Vm *vm, void *p, size_t bytes) {
    if (!p) return;
    VaHeap *h = &vm->va;
    size_t size = bytes > VA_MAX_SMALL ? bytes : round_up(bytes);
    char *b = (char *)p - VA_HDR;
    GOGO_ASSERT(*(const size_t *)(const void *)b == size);   /* the size it was allocated with */
    GOGO_ASSERT(h->used >= size);
    h->used -= size;
    if (bytes > VA_MAX_SMALL) {
        vm_chunk_free(vm, b, size + VA_HDR);
    } else {
        FreeBlock *f = (FreeBlock *)(void *)b;
        size_t cls = (size + VA_HDR) / VA_GRAIN;
        f->next = h->free_list[cls];
        h->free_list[cls] = f;
    }
}

void va_reset_peak(Vm *vm) { vm->va.peak = vm->va.used; }

void va_reset(Vm *vm) {
    /* Chunks are intentionally not returned: blocks may still be live. */
    VaHeap *h = &vm->va;
    for (size_t i = 0; i <= VA_CLASSES; i++) h->free_list[i] = NULL;
    h->chunk_cur = h->chunk_end = NULL;
    h->used = 0;
    h->limit = 0;
    h->peak = 0;
}
