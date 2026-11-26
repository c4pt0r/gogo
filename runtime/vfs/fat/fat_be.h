/* A vfs backend over FatFs (its f_* API) on a board's removable disk: what
 * a board writes is the DiskHal (bring the medium up, check it is still
 * there, read and write 512-byte sectors), the hookup of FatFs's disk
 * layer to it (fat_be_disk_*), and one FatVol in static storage.
 *
 * Mounting is lazy. Every path call (open, stat, readdir, mkdir, remove,
 * rename, usage) first makes sure a medium is there: when mounted, check
 * says it still answers; when not (or it stopped answering), attach brings
 * it up and FatFs is mounted, then once more with attach's second attempt
 * (a slower clock) if that failed (not when the medium reads fine but
 * holds no FAT). A medium inserted later is mounted by the next call,
 * without a reboot. A disk error (FR_DISK_ERR / FR_NOT_READY) during any
 * call unmounts, so the next call remounts; open files of the old mount
 * then fail with IO (FatFs checks the volume id) and can still be closed.
 * An unformatted medium is NotMounted (never formatted here).
 *
 * FatFs must be built with 512-byte sectors (FF_MAX_SS 512), the static
 * window (no FF_USE_DYN_BUFFER), FF_FS_TINY, long names on the stack
 * (FF_USE_LFN 2, FF_MAX_LFN 64) and no lock (checked in fat_be.c): no
 * malloc on the file path. The FATFS object is in the FatVol, files live
 * in their vfs slot (a FIL), long names on the stack. One volume, drive
 * "0:" (physical drive 0).
 *
 * FAT specifics: names compare ignoring case (case_insensitive); rename
 * cannot replace, so rename(replace=true) returns Exists and vfs removes
 * then renames (not atomic); case_insensitive also makes vfs refuse a
 * Write open (truncate) while the path is open for reading
 * (runtime/vfs/vfs.h). Usage above 2 GB is clamped to INT32_MAX bytes.
 *
 * Not in gogovm or libgogo (the repo carries FatFs only as the ESP
 * component); the firmware builds it, and make test-fat runs it over a
 * RAM disk with that FatFs built for the host. Only the VM thread calls
 * in. */
#ifndef GOGO_FAT_BE_H
#define GOGO_FAT_BE_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"
#include "ff.h"
#include "diskio.h"

#define FAT_BE_SECTOR 512u

/* A board's disk, in 512-byte sectors. Every hook runs on the VM thread.
 *  - attach: brings the medium up for mount attempt `attempt` (0, then 1
 *    at a safer setting such as a slower clock); 0 ok, 1 failed (the next
 *    attempt may work), 2 failed for good (no bus).
 *  - check: the medium mounted earlier still answers (an SD card's
 *    CMD13); 0 yes.
 *  - read / write: n sectors at sector; 0 ok.
 *  - sectors: the medium's size in sectors.
 *  - mounted (optional): called after each successful mount, for the
 *    board's log line (exfat: the file system is exFAT, else FAT). */
typedef struct DiskHal {
    void *ctx;
    int (*attach)(void *ctx, int attempt);
    int (*check)(void *ctx);
    int (*read)(void *ctx, uint32_t sector, uint8_t *buf, uint32_t n);
    int (*write)(void *ctx, uint32_t sector, const uint8_t *buf, uint32_t n);
    uint32_t (*sectors)(void *ctx);
    void (*mounted)(void *ctx, bool exfat);
} DiskHal;

/* One volume: static storage, set up by fat_be_init. */
typedef struct FatVol {
    FATFS fs;
    const DiskHal *hal;
    const char *name;                    /* the mount prefix, for messages ("/sd") */
    void (*say)(const char *fmt, ...);   /* console messages; NULL = silent */
    bool mounted;
    bool said_nofs;                      /* the no-FAT message was given for this medium */
} FatVol;

/* The backend; its ctx is the FatVol. */
extern const VfsBackend fat_be_backend;

/* Binds v to hal (nothing is touched until the first call). */
void fat_be_init(FatVol *v, const DiskHal *hal, const char *name, void (*say)(const char *fmt, ...));

/* FatFs's disk layer for physical drive 0, on v's DiskHal: the board's
 * FatFs glue (disk_read, ... or a registered driver table) calls these.
 * They return FatFs's DRESULT / DSTATUS values. */
DSTATUS fat_be_disk_status(FatVol *v);
DRESULT fat_be_disk_read(FatVol *v, BYTE *buf, LBA_t sector, UINT n);
DRESULT fat_be_disk_write(FatVol *v, const BYTE *buf, LBA_t sector, UINT n);
DRESULT fat_be_disk_ioctl(FatVol *v, BYTE cmd, void *buf);

#endif
