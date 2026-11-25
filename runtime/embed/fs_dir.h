/* A host directory as a vfs medium (POSIX: Linux, macOS): /flash of an
 * embedded VM (gogo_config.fs_root) and of the unix gogovm (--fs-root).
 *
 * The directory is a sandbox. Every access is relative to the root's
 * directory fd and walks the path one component at a time without
 * following symlinks, so a name inside the root can never reach outside it:
 *  - a symlink anywhere in a path (as a directory or as the file itself),
 *    a FIFO, socket or device, and a ".", ".." or empty component are
 *    BadPath; so is a path longer than VFS_PATH_MAX;
 *  - a file with other hard links is BadPath when opened for writing (its
 *    other names may lie outside); reading it is allowed;
 *  - Remove and Rename act on the names themselves (a symlink is removed,
 *    never its target).
 * ReadDir lists every entry (symlinks as files); opening one is BadPath.
 * A directory that something outside the VM moves out of the root while
 * a walk holds it is not caught: the sandbox is against the program, not
 * against other processes of the host. */
#ifndef GOGO_FS_DIR_H
#define GOGO_FS_DIR_H

#include <stdint.h>
#include "vfs.h"
#include "../include/gogo.h"

typedef struct FsDir FsDir;

/* The backend; its ctx is the FsDir * fs_dir_open returns. */
extern const VfsBackend fs_dir_backend;

/* Serves the existing directory `dir` (the string is not kept). quota < 0:
 * no limit; else the bytes the regular files under dir may hold in all
 * (what is there now is counted here, then every write, truncation,
 * removal and replacement is tracked): a write past it stores what still
 * fits and fails with NoSpace, and Usage reports (used, quota). Memory
 * comes from h->alloc(ctx). NULL if dir cannot be opened as a directory
 * or memory runs out. */
FsDir *fs_dir_open(const gogo_host *h, void *ctx, const char *dir, int64_t quota);

/* Closes the root (files opened through it must be closed first) and
 * frees d. NULL is ignored. */
void fs_dir_close(FsDir *d);

#endif
