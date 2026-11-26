/* A vfs backend over LittleFS (its raw lfs.h API, littlefs >= v2.7) on a
 * board's block device: what a board writes is the BlockDev (four calls on
 * its flash), and it keeps one LfsVol per volume in static storage.
 *
 * No malloc: the LittleFS state, its read / prog caches and lookahead
 * buffer are in the LfsVol, and each open file lives in its vfs slot (Lf:
 * lfs_file_t + its lfs_file_config + its 64-byte cache), opened with
 * lfs_file_opencfg. lfs.c must be built with LFS_NO_MALLOC and with malloc
 * / free renamed to gogo_lfs_malloc / gogo_lfs_free (defined in lfs_be.c,
 * they return NULL), so LittleFS cannot reach the heap; a NULL buffer would
 * be LFS_ERR_NOMEM. GOGO_FS_SLOT_BYTES must be sizeof(Lf) (160 with
 * littlefs v2.11 on a 32-bit chip; checked in lfs_be.c).
 *
 * Not in gogovm or libgogo: the repo does not carry LittleFS (the ESP
 * build fetches it as a managed component); the firmware builds it, and
 * make test-lfs runs it over a RAM block device with that copy (skipped
 * before a firmware build fetched it). Pure C99 otherwise. When another thread uses
 * the volume too (the ESP console task's `gogo push`), the board gives a
 * lock (LfsOpts.lock) and every backend call holds it. */
#ifndef GOGO_LFS_BE_H
#define GOGO_LFS_BE_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"
#include "lfs.h"

/* The read / prog cache of the volume and of each open file (bytes; a
 * board whose flash programs larger units builds with a larger one, and
 * GOGO_FS_SLOT_BYTES grows with it). */
#ifndef LFS_BE_CACHE
#define LFS_BE_CACHE     64u
#endif
#define LFS_BE_LOOKAHEAD 16u

/* A board's flash, in erase blocks of block_size bytes (block_count of
 * them). read / prog take a block and an offset in it (prog only to erased
 * bytes, in multiples of prog_size); erase erases one block; sync
 * (optional) flushes what the device buffers. Every call returns 0 or a
 * negative error. read_size / prog_size: the smallest read and program
 * (0 = 16); both must divide LFS_BE_CACHE, which must divide block_size.
 * Bounds: lfs_be calls read / prog / erase only with block < block_count
 * and off + n <= block_size, so the device needs no checks of its own. */
typedef struct BlockDev {
    void *ctx;
    uint32_t block_size, block_count;
    uint32_t read_size, prog_size;
    int (*read)(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t n);
    int (*prog)(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t n);
    int (*erase)(void *ctx, uint32_t block);
    int (*sync)(void *ctx);
} BlockDev;

/* How a volume is mounted and talks: name, the mount prefix, for messages
 * ("/flash"); format_hint, how a user formats it ("gogo device
 * format-flash"), for messages; say, printf-like console messages (NULL =
 * silent); lock (NULL = only one thread calls in): take (true) and give
 * (false) the volume's lock. */
typedef struct LfsOpts {
    const char *name;
    const char *format_hint;
    void (*say)(const char *fmt, ...);
    void (*lock)(bool take);
} LfsOpts;

/* One volume: zeroed static storage (bound in by lfs_be_mount). */
typedef struct LfsVol {
    lfs_t lfs;
    struct lfs_config cfg;
    const BlockDev *bd;
    const LfsOpts *opts;
    bool mounted;
    uint32_t read_buf[LFS_BE_CACHE / 4u], prog_buf[LFS_BE_CACHE / 4u], look_buf[LFS_BE_LOOKAHEAD / 4u];
} LfsVol;

/* The backend; its ctx is the LfsVol. On a volume that is not mounted
 * every call is GOGO_E_NOTMOUNTED. */
extern const VfsBackend lfs_be_backend;

/* lfs_be_mount's modes. */
enum {
    LFS_BE_AUTO = 0,        /* mount; format a blank or unreadable device; grow to want_blocks */
    LFS_BE_FORMAT = 1,      /* format at want_blocks first (data lost), then mount */
    LFS_BE_MOUNT_ONLY = 2   /* mount as it is: no format, grow or size checks */
};

/* Mounts v over bd (o outlives v). want_blocks: the size wanted, in
 * blocks (clamped to bd->block_count; 0 = the whole device). LFS_BE_AUTO
 * reads the size from the superblock; a larger want grows the file system
 * in place (data kept); a smaller one keeps the current size and says so
 * (shrinking needs a format). A blank or unreadable file system (first
 * boot) is formatted; any other mount error (IO), or a file system bigger
 * than the device, leaves v unmounted and its data untouched; the
 * too-big refusal applies in every mode. 0 mounted, else a negative
 * LittleFS error. Runs before any other user of v. */
int lfs_be_mount(LfsVol *v, const BlockDev *bd, const LfsOpts *o, uint32_t want_blocks, int mode);

/* The mounted file system's size in blocks (0 if not mounted). */
uint32_t lfs_be_blocks(LfsVol *v);

#endif
