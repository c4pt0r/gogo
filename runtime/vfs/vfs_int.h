#ifndef GOGO_VFS_INT_H
#define GOGO_VFS_INT_H
#include "vfs.h"

/* VfsState, here so a board can keep it in static storage and count it in
 * its fixed-state budget (VFS_STATE_BYTES: the mounts, their count and the
 * slots). */
typedef struct { const char *prefix; uint32_t plen; const VfsBackend *be; void *ctx; } VfsMount;
/* A slot names its file by the length and the hash of its whole path:
 * two paths are "the same" (Busy, Remove, Rename) when both match. */
typedef struct {
    uint8_t used, mode, mnt;
    uint8_t plen;                /* path length (<= VFS_PATH_MAX) */
    uint32_t hash;
    union { void *p; uint32_t u; uint8_t b[GOGO_FS_SLOT_BYTES]; } f;
} VfsSlot;
struct VfsState {
    VfsMount mounts[VFS_MAX_MOUNTS];
    uint32_t n_mounts;
    VfsSlot slots[GOGO_FS_MAX_OPEN];
};
#define VFS_STATE_BYTES (sizeof(VfsState))
typedef char vfs_plen_fits[VFS_PATH_MAX <= 255u ? 1 : -1];

/* path.c */
/* 0 if p is a valid absolute path (>= 1 segment); *first = length of the first segment incl. '/' */
int vfs_path_check(const char *p, uint32_t len, uint32_t *first);
uint32_t vfs_path_hash(const char *p, uint32_t len, bool lower);   /* FNV-1a 32 */

#endif
