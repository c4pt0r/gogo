#ifndef GOGO_VFS_INT_H
#define GOGO_VFS_INT_H
#include "vfs.h"

/* vfs.c's tables, here so a platform can count them in its fixed-state
 * budget (VFS_STATE_BYTES: the mounts, their count and the slots). */
typedef struct { const char *prefix; uint32_t plen; const VfsBackend *be; } VfsMount;
typedef struct {
    uint8_t used, mode, mnt;
    uint32_t hash;
    union { void *p; uint32_t u; uint8_t b[GOGO_FS_SLOT_BYTES]; } f;
} VfsSlot;
#define VFS_STATE_BYTES (sizeof(VfsMount) * VFS_MAX_MOUNTS + sizeof(uint32_t) + sizeof(VfsSlot) * GOGO_FS_MAX_OPEN)

/* path.c */
/* 0 if p is a valid absolute path (>= 1 segment); *first = length of the first segment incl. '/' */
int vfs_path_check(const char *p, uint32_t len, uint32_t *first);
uint32_t vfs_path_hash(const char *p, uint32_t len, bool lower);   /* FNV-1a 32 */

#endif
