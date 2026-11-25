/* Unix VfsBackend over a host directory (gogovm --fs-root). */
#ifndef GOGO_FS_HOST_H
#define GOGO_FS_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"

#define FS_HOST_INSTANCES 2
#define FS_HOST_ROOT_MAX 512   /* longest directory string, with its NUL */

/* Instance i (0..1) serves the host directory `dir` (must exist; the
 * string is copied). Returns the backend to pass to vfs_mount, or NULL. */
const VfsBackend *fs_host_open(int i, const char *dir);

/* Test only: limit the bytes all writes may add up to (ENOSPC beyond). */
void fs_host_set_quota(int64_t bytes);

#endif
