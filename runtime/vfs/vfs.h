/* vfs: path rules, mounts, a fixed slot table. Pure C99, no malloc, no VM
 * types. Backends (LittleFS, FatFs, unix directory, test memfs) register
 * through vfs_mount; paths are (ptr, len) pairs, never NUL-terminated. */
#ifndef GOGO_VFS_H
#define GOGO_VFS_H

#include <stdbool.h>
#include <stdint.h>
#include "../include/gogo_errors.h"

#ifndef GOGO_FS_MAX_OPEN
#define GOGO_FS_MAX_OPEN 4u
#endif
#ifndef GOGO_FS_SLOT_BYTES
#define GOGO_FS_SLOT_BYTES 64u      /* opaque backend per-file storage */
#endif

#define VFS_MAX_MOUNTS 2u
#define VFS_PATH_MAX 96u
#define VFS_NAME_MAX 32u

enum { VFS_READ = 0, VFS_WRITE = 1, VFS_APPEND = 2 };

/* B_FS op codes (its first argument). */
enum {
    FS_OPEN = 0, FS_READ = 1, FS_WRITE = 2, FS_SYNC = 3, FS_SIZE = 4, FS_READFILE = 5,
    FS_WRITEFILE = 6, FS_STAT = 7, FS_READDIR = 8, FS_MKDIR = 9, FS_REMOVE = 10,
    FS_RENAME = 11, FS_USAGE = 12, FS_WRITEAT = 13,
    FS_OPENERR = 14   /* (15.4) -> code: vm->open_err, why the last refused port open failed
                         (port.Open); answered by the VM itself (VM_FS_OPENERR, interp.c) */
};

/* Most bytes one File.Read returns. */
#define VFS_READ_MAX 4096

typedef int (*VfsDirCb)(void *ctx, const char *name, bool is_dir);

/* Every function returns an error code: GOGO_E_OK or another shared code
 * (gogo_errors.h; fs.Err has the same numbers); `ctx` is the be_ctx given to vfs_mount
 * (one backend table can serve several media). `rel` is the path after the mount
 * prefix ("/" for the root, NUL-terminated); `f` points at the slot's
 * pointer-aligned GOGO_FS_SLOT_BYTES of storage. open leaves f unused on
 * failure. rename(replace=true) may return GOGO_E_EXISTS when the medium
 * cannot replace atomically; vfs then removes the target and retries.
 *
 * Contract every backend (LittleFS, FatFs, unix directory, memfs) obeys:
 *  - readdir: names only, never "." or ".."; when the callback returns
 *    non-zero the walk stops and readdir still returns GOGO_E_OK.
 *  - size(f) describes the open handle (fstat on the fd), not the path.
 *  - rename(from, to, replace=false) returns GOGO_E_EXISTS if `to` exists
 *    (POSIX rename replaces, so check first); replace=true replaces.
 *  - remove of a non-empty directory returns GOGO_E_NOTEMPTY (rmdir
 *    semantics); remove of a file or an empty directory returns GOGO_E_OK.
 *  - write_at(f, off, buf, n) overwrites bytes already in the file (vfs
 *    checked off + n <= size and the Write mode): it never extends the
 *    file, and it leaves the position where the next write() goes (the end
 *    of the file) alone. It needs no new space on FAT and one block on
 *    LittleFS.
 *  - errors map: not found -> NOTFOUND, exists -> EXISTS, no space ->
 *    NOSPACE, is a directory -> ISDIR, not a directory -> NOTDIR, not
 *    empty -> NOTEMPTY, anything else -> IO. */
typedef struct VfsBackend {
    /* A FAT-like medium: names compare ignoring case (the slot hash is
     * lower-cased), and a Write open (create or truncate) of a path that is
     * open for reading is Busy, because FatFs would free the clusters under
     * the reader's handle (it could then read another file's data). Append
     * and Read opens are not affected. */
    bool case_insensitive;
    int (*open)(void *ctx, void *f, const char *rel, int mode);
    int (*read)(void *ctx, void *f, uint8_t *buf, uint32_t n, uint32_t *got);
    int (*write)(void *ctx, void *f, const uint8_t *buf, uint32_t n);
    int (*sync)(void *ctx, void *f);
    int (*size)(void *ctx, void *f, int32_t *out);
    int (*close)(void *ctx, void *f);
    int (*stat)(void *ctx, const char *rel, int32_t *size, bool *is_dir);
    int (*readdir)(void *ctx, const char *rel, VfsDirCb cb, void *cb_ctx);
    int (*mkdir)(void *ctx, const char *rel);
    int (*remove)(void *ctx, const char *rel);
    int (*rename)(void *ctx, const char *from, const char *to, bool replace);
    int (*usage)(void *ctx, int32_t *used, int32_t *total);
    int (*write_at)(void *ctx, void *f, uint32_t off, const uint8_t *buf, uint32_t n);
} VfsBackend;

/* One instance's mounts and open-file slots (vfs_int.h; VFS_STATE_BYTES).
 * Zeroed storage (a static) is an empty state; so is one after vfs_reset.
 * Every call below runs on the thread that owns the Vm it is attached to. */
typedef struct VfsState VfsState;
uint32_t vfs_state_bytes(void);                            /* sizeof(VfsState) */

/* prefix outlives the mount; be_ctx is passed to every op of b. */
bool vfs_mount(VfsState *st, const char *prefix, const VfsBackend *b, void *be_ctx);
void vfs_reset(VfsState *st);                              /* drop mounts and slots (no backend close) */

int vfs_open(VfsState *st, const char *p, uint32_t len, int mode, int *slot);
int vfs_read(VfsState *st, int slot, uint8_t *buf, uint32_t n, uint32_t *got);
int vfs_write(VfsState *st, int slot, const uint8_t *p, uint32_t n);
/* Overwrites n bytes at off in a file opened VFS_WRITE (GOGO_E_CLOSED for a
 * Read or Append open or a free slot); GOGO_E_BADPATH if off + n is past the
 * end of the file (it is never extended); GOGO_E_IO if the backend has no
 * write_at. */
int vfs_write_at(VfsState *st, int slot, uint32_t off, const uint8_t *p, uint32_t n);
int vfs_sync(VfsState *st, int slot);
int vfs_size(VfsState *st, int slot, int32_t *out);
void vfs_close(VfsState *st, int slot);
int vfs_slot_mode(VfsState *st, int slot);   /* VFS_READ/WRITE/APPEND, or -1 if the slot is free */

int vfs_stat(VfsState *st, const char *p, uint32_t len, int32_t *size, bool *is_dir);
int vfs_readdir(VfsState *st, const char *p, uint32_t len, VfsDirCb cb, void *ctx);
int vfs_mkdir(VfsState *st, const char *p, uint32_t len);
int vfs_remove(VfsState *st, const char *p, uint32_t len);
/* Exists if b exists (also b == a); BadPath if b lies in a's subtree (case
 * folded on a case-insensitive mount, where a case-only rename goes to the
 * backend); Busy if a is a directory and any file is open on that mount. */
int vfs_rename(VfsState *st, const char *a, uint32_t alen, const char *b, uint32_t blen);
int vfs_replace(VfsState *st, const char *tmp, uint32_t tlen, const char *p, uint32_t len); /* rename-over (WriteFile) */
int vfs_usage(VfsState *st, const char *prefix, uint32_t len, int32_t *used, int32_t *total);

/* builtin.c (links against vm/): attaches st to vm (vm->vfs), points
 * vm's fs_builtin hook at the B_FS implementation and its private `file`
 * port driver, and mounts the board's file systems that have a backend
 * (VmBoard.mounts, host.h) in their order. Mount others before or after;
 * call after vm_boot (vm_init clears the hook). st must outlive the Vm. */
struct Vm;
void vfs_init(struct Vm *vm, VfsState *st);

#endif
