/* vfs: path rules, mounts, a fixed slot table. Pure C99, no malloc, no VM
 * types. Backends (LittleFS, FatFs, unix directory, test memfs) register
 * through vfs_mount; paths are (ptr, len) pairs, never NUL-terminated. */
#ifndef GOGO_VFS_H
#define GOGO_VFS_H

#include <stdbool.h>
#include <stdint.h>

#ifndef GOGO_FS_MAX_OPEN
#define GOGO_FS_MAX_OPEN 4u
#endif
#ifndef GOGO_FS_SLOT_BYTES
#define GOGO_FS_SLOT_BYTES 64u      /* opaque backend per-file storage */
#endif

#define VFS_MAX_MOUNTS 2u
#define VFS_PATH_MAX 96u
#define VFS_NAME_MAX 32u

/* Same numbers as fs.Err. */
typedef enum {
    VFS_OK = 0, VFS_NOTFOUND = 1, VFS_EXISTS = 2, VFS_NOTMOUNTED = 3,
    VFS_NOSPACE = 4, VFS_TOOMANY = 5, VFS_BUSY = 6, VFS_BADPATH = 7,
    VFS_ISDIR = 8, VFS_NOTDIR = 9, VFS_NOTEMPTY = 10, VFS_NOMEMORY = 11,
    VFS_IO = 12, VFS_CLOSED = 13
} VfsErr;

enum { VFS_READ = 0, VFS_WRITE = 1, VFS_APPEND = 2 };

/* B_FS op codes (its first argument). */
enum {
    FS_OPEN = 0, FS_READ = 1, FS_WRITE = 2, FS_SYNC = 3, FS_SIZE = 4, FS_READFILE = 5,
    FS_WRITEFILE = 6, FS_STAT = 7, FS_READDIR = 8, FS_MKDIR = 9, FS_REMOVE = 10,
    FS_RENAME = 11, FS_USAGE = 12, FS_WRITEAT = 13
};

/* Most bytes one File.Read returns. */
#define VFS_READ_MAX 4096

typedef int (*VfsDirCb)(void *ctx, const char *name, bool is_dir);

/* Every function returns a VfsErr. `rel` is the path after the mount
 * prefix ("/" for the root, NUL-terminated); `f` points at the slot's
 * pointer-aligned GOGO_FS_SLOT_BYTES of storage. open leaves f unused on
 * failure. rename(replace=true) may return VFS_EXISTS when the medium
 * cannot replace atomically; vfs then removes the target and retries.
 *
 * Contract every backend (LittleFS, FatFs, unix directory, memfs) obeys:
 *  - readdir: names only, never "." or ".."; when the callback returns
 *    non-zero the walk stops and readdir still returns VFS_OK.
 *  - size(f) describes the open handle (fstat on the fd), not the path.
 *  - rename(from, to, replace=false) returns VFS_EXISTS if `to` exists
 *    (POSIX rename replaces, so check first); replace=true replaces.
 *  - remove of a non-empty directory returns VFS_NOTEMPTY (rmdir
 *    semantics); remove of a file or an empty directory returns VFS_OK.
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
    int (*open)(void *f, const char *rel, int mode);
    int (*read)(void *f, uint8_t *buf, uint32_t n, uint32_t *got);
    int (*write)(void *f, const uint8_t *buf, uint32_t n);
    int (*sync)(void *f);
    int (*size)(void *f, int32_t *out);
    int (*close)(void *f);
    int (*stat)(const char *rel, int32_t *size, bool *is_dir);
    int (*readdir)(const char *rel, VfsDirCb cb, void *ctx);
    int (*mkdir)(const char *rel);
    int (*remove)(const char *rel);
    int (*rename)(const char *from, const char *to, bool replace);
    int (*usage)(int32_t *used, int32_t *total);
    int (*write_at)(void *f, uint32_t off, const uint8_t *buf, uint32_t n);
} VfsBackend;

bool vfs_mount(const char *prefix, const VfsBackend *b);   /* prefix outlives the mount */
void vfs_reset(void);                                      /* tests: drop mounts and slots */

int vfs_open(const char *p, uint32_t len, int mode, int *slot);
int vfs_read(int slot, uint8_t *buf, uint32_t n, uint32_t *got);
int vfs_write(int slot, const uint8_t *p, uint32_t n);
/* Overwrites n bytes at off in a file opened VFS_WRITE (VFS_CLOSED for a
 * Read or Append open or a free slot); VFS_BADPATH if off + n is past the
 * end of the file (it is never extended); VFS_IO if the backend has no
 * write_at. */
int vfs_write_at(int slot, uint32_t off, const uint8_t *p, uint32_t n);
int vfs_sync(int slot);
int vfs_size(int slot, int32_t *out);
void vfs_close(int slot);
int vfs_slot_mode(int slot);           /* VFS_READ/WRITE/APPEND, or -1 if the slot is free */

int vfs_stat(const char *p, uint32_t len, int32_t *size, bool *is_dir);
int vfs_readdir(const char *p, uint32_t len, VfsDirCb cb, void *ctx);
int vfs_mkdir(const char *p, uint32_t len);
int vfs_remove(const char *p, uint32_t len);
/* Exists if b exists (also b == a); BadPath if b lies in a's subtree (case
 * folded on a case-insensitive mount, where a case-only rename goes to the
 * backend); Busy if a is a directory and any file is open on that mount. */
int vfs_rename(const char *a, uint32_t alen, const char *b, uint32_t blen);
int vfs_replace(const char *tmp, uint32_t tlen, const char *p, uint32_t len); /* rename-over (WriteFile) */
int vfs_usage(const char *prefix, uint32_t len, int32_t *used, int32_t *total);

/* builtin.c (links against vm/): points vm's fs_builtin hook at the B_FS
 * implementation and its private `file` port driver. Mount first; call
 * after vm_boot (vm_init clears the hook). */
struct Vm;
void vfs_install(struct Vm *vm);

#endif
