#include "alloc.h"
#include "platform.h"

#define VA_GRAIN      8u
#define VA_MAX_SMALL  256u
#define VA_CLASSES    (VA_MAX_SMALL / VA_GRAIN)
#define VA_CHUNK      4096u

typedef struct FreeBlock { struct FreeBlock *next; } FreeBlock;

static FreeBlock *free_list[VA_CLASSES + 1];  /* indexed by size / VA_GRAIN */
static char *chunk_cur, *chunk_end;           /* unused tail of newest chunk */
static size_t used, limit;

static size_t round_up(size_t n) {
    if (n == 0) n = 1;
    return (n + VA_GRAIN - 1) & ~(size_t)(VA_GRAIN - 1);
}

void *va_alloc(size_t bytes) {
    size_t size = bytes > VA_MAX_SMALL ? bytes : round_up(bytes);
    if (limit && used + size > limit) return NULL;

    void *p;
    if (bytes > VA_MAX_SMALL) {
        p = platform_chunk_alloc(bytes);
    } else {
        size_t cls = size / VA_GRAIN;
        if (free_list[cls]) {
            p = free_list[cls];
            free_list[cls] = free_list[cls]->next;
        } else {
            if ((size_t)(chunk_end - chunk_cur) < size) {
                /* Tail of the old chunk is abandoned; at most 248 bytes. */
                char *c = platform_chunk_alloc(VA_CHUNK);
                if (!c) return NULL;
                chunk_cur = c;
                chunk_end = c + VA_CHUNK;
            }
            p = chunk_cur;
            chunk_cur += size;
        }
    }
    if (p) used += size;
    return p;
}

void va_free(void *p, size_t bytes) {
    if (!p) return;
    if (bytes > VA_MAX_SMALL) {
        platform_chunk_free(p, bytes);
        used -= bytes;
    } else {
        size_t size = round_up(bytes);
        FreeBlock *b = p;
        b->next = free_list[size / VA_GRAIN];
        free_list[size / VA_GRAIN] = b;
        used -= size;
    }
}

size_t va_used_bytes(void) { return used; }
void va_set_limit(size_t bytes) { limit = bytes; }

void va_reset(void) {
    /* Chunks are intentionally not returned: blocks may still be live. */
    for (size_t i = 0; i <= VA_CLASSES; i++) free_list[i] = NULL;
    chunk_cur = chunk_end = NULL;
    used = 0;
    limit = 0;
}
