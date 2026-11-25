#define _POSIX_C_SOURCE 200809L
/* VfsBackend on POSIX: a host directory as a medium. The slot storage
 * holds the fd. Contract notes: see the VfsBackend comment in runtime/vfs/vfs.h. */
#include "fs_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>


typedef struct { int fd; } H;
typedef char h_fits[sizeof(H) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

static char roots[FS_HOST_INSTANCES][FS_HOST_ROOT_MAX];

static int map_errno(int e) {
    switch (e) {
    case ENOENT: return VFS_NOTFOUND;
    case EEXIST: return VFS_EXISTS;
    case ENOSPC: return VFS_NOSPACE;
    case EISDIR: return VFS_ISDIR;
    case ENOTDIR: return VFS_NOTDIR;
    case ENOTEMPTY: return VFS_NOTEMPTY;
    default: return VFS_IO;
    }
}

static int clamp32(off_t v) { return v > INT32_MAX ? INT32_MAX : (int)v; }

static bool join(const char *root, const char *rel, char *out) {
    size_t r = strlen(root);
    size_t n = (rel[0] == '/' && !rel[1]) ? 0 : strlen(rel);
    if (r + n + 1 > PATH_MAX || r + n + 1 > FS_HOST_ROOT_MAX + VFS_PATH_MAX) return false;
    memcpy(out, root, r);
    memcpy(out + r, rel, n);
    out[r + n] = 0;
    return true;
}

static int h_open(void *c, void *f, const char *rel, int mode) {
    char p[PATH_MAX];
    if (!join(c, rel, p)) return VFS_BADPATH;
    int fl = O_CLOEXEC;
    if (mode == VFS_READ) fl |= O_RDONLY;
    else if (mode == VFS_WRITE) fl |= O_WRONLY | O_CREAT | O_TRUNC;
    else fl |= O_WRONLY | O_CREAT | O_APPEND;
    int fd = open(p, fl, 0666);
    if (fd < 0) return map_errno(errno);
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); return map_errno(e); }
    if (S_ISDIR(st.st_mode)) { close(fd); return VFS_ISDIR; }
    ((H *)f)->fd = fd;
    return VFS_OK;
}
static int h_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    (void)c;
    ssize_t k;
    do k = read(((H *)f)->fd, buf, n); while (k < 0 && errno == EINTR);
    if (k < 0) return map_errno(errno);
    *got = (uint32_t)k;
    return VFS_OK;
}
/* Test only (gogovm --fs-quota): the most bytes all writes may add up to;
 * a write past it stores what still fits and fails with ENOSPC. */
static int64_t quota_left = -1;   /* -1: no limit */
void fs_host_set_quota(int64_t bytes) { quota_left = bytes; }

static int h_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    (void)c;
    if (quota_left >= 0 && (int64_t)n > quota_left) {
        uint32_t fit = (uint32_t)quota_left;
        quota_left = 0;
        while (fit > 0) {
            ssize_t k = write(((H *)f)->fd, buf, fit);
            if (k < 0) { if (errno == EINTR) continue; break; }
            buf += k;
            fit -= (uint32_t)k;
        }
        return VFS_NOSPACE;
    }
    if (quota_left >= 0) quota_left -= n;
    while (n > 0) {
        ssize_t k = write(((H *)f)->fd, buf, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        buf += k;
        n -= (uint32_t)k;
    }
    return VFS_OK;
}
static int h_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    (void)c;
    while (n > 0) {
        ssize_t k = pwrite(((H *)f)->fd, buf, n, (off_t)off);   /* vfs allows Write opens only: no O_APPEND */
        if (k < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        buf += k;
        off += (uint32_t)k;
        n -= (uint32_t)k;
    }
    return VFS_OK;
}
static int h_sync(void *c, void *f) { (void)c; return fsync(((H *)f)->fd) == 0 ? VFS_OK : map_errno(errno); }
static int h_size(void *c, void *f, int32_t *out) {
    (void)c;
    struct stat st;
    if (fstat(((H *)f)->fd, &st) != 0) return map_errno(errno);
    *out = clamp32(st.st_size);
    return VFS_OK;
}
static int h_close(void *c, void *f) { (void)c; return close(((H *)f)->fd) == 0 ? VFS_OK : map_errno(errno); }

static int h_stat(void *c, const char *rel, int32_t *size, bool *is_dir) {
    char p[PATH_MAX];
    struct stat st;
    if (!join(c, rel, p)) return VFS_BADPATH;
    if (stat(p, &st) != 0) return map_errno(errno);
    *is_dir = S_ISDIR(st.st_mode);
    *size = *is_dir ? 0 : clamp32(st.st_size);
    return VFS_OK;
}
static int h_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    char p[PATH_MAX], q[PATH_MAX + VFS_NAME_MAX + 2];
    if (!join(c, rel, p)) return VFS_BADPATH;
    DIR *d = opendir(p);
    if (!d) return map_errno(errno);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ) continue;
        struct stat st;
        /* Names that break the path rules are still listed (opening them
         * gives BadPath). Entries we cannot stat (dangling symlinks) are skipped. */
        if ((size_t)snprintf(q, sizeof q, "%s/%s", p, e->d_name) >= sizeof q || stat(q, &st) != 0) continue;
        if (cb(ctx, e->d_name, S_ISDIR(st.st_mode)) != 0) break;
    }
    closedir(d);
    return VFS_OK;
}
static int h_mkdir(void *c, const char *rel) {
    char p[PATH_MAX];
    if (!join(c, rel, p)) return VFS_BADPATH;
    return mkdir(p, 0777) == 0 ? VFS_OK : map_errno(errno);
}
static int h_remove(void *c, const char *rel) {
    char p[PATH_MAX];
    struct stat st;
    if (!join(c, rel, p)) return VFS_BADPATH;
    if (lstat(p, &st) != 0) return map_errno(errno);
    int rc = S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p);
    return rc == 0 ? VFS_OK : map_errno(errno);
}
static int h_rename(void *c, const char *from, const char *to, bool replace) {
    char a[PATH_MAX], b[PATH_MAX];
    struct stat st;
    if (!join(c, from, a) || !join(c, to, b)) return VFS_BADPATH;
    if (!replace && lstat(b, &st) == 0) return VFS_EXISTS;
    return rename(a, b) == 0 ? VFS_OK : map_errno(errno);
}
static int h_usage(void *c, int32_t *used, int32_t *total) {
    struct statvfs sv;
    if (statvfs((const char *)c, &sv) != 0) return map_errno(errno);
    unsigned long long fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    unsigned long long t = fr * sv.f_blocks, u = t - fr * sv.f_bfree;
    *total = t > INT32_MAX ? INT32_MAX : (int32_t)t;
    *used = u > INT32_MAX ? INT32_MAX : (int32_t)u;
    return VFS_OK;
}

const VfsBackend fs_host_be = { false, h_open, h_read, h_write, h_sync, h_size, h_close, h_stat, h_readdir,
                                 h_mkdir, h_remove, h_rename, h_usage, h_write_at };

void *fs_host_open(int i, const char *dir) {
    if (i < 0 || i >= FS_HOST_INSTANCES || strlen(dir) >= FS_HOST_ROOT_MAX) return NULL;
    size_t n = strlen(dir);
    memcpy(roots[i], dir, n + 1);
    while (n > 1 && roots[i][n - 1] == '/') roots[i][--n] = 0;
    return roots[i];
}
