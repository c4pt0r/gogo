/* VfsBackend on POSIX: a host directory as a sandboxed medium (fs_dir.h).
 * The slot storage holds the fd. Contract notes: see the VfsBackend comment
 * in runtime/vfs/vfs.h.
 *
 * Portability: only POSIX.1-2008 calls (openat, fstatat, mkdirat,
 * renameat, unlinkat, fdopendir, O_DIRECTORY, O_NOFOLLOW,
 * AT_SYMLINK_NOFOLLOW), the same on Linux and macOS. Linux's
 * openat2(RESOLVE_BENEATH) would do the walk in the kernel; the walk below
 * gives the same guarantee for the program everywhere. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "fs_dir.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>          /* renameat, snprintf */
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct FsDir {
    const gogo_host *h;
    void *hctx;
    int root;            /* the directory's fd: every access starts here */
    int64_t quota;       /* -1: none */
    int64_t used;        /* bytes in regular files under root (tracked with a quota), >= 0 */
    bool ci;             /* the host file system ignores case under root */
};

typedef struct { int fd; bool append; } H;
typedef char h_fits[sizeof(H) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

/* One step of a walk: a directory, never through a symlink. */
#define DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
/* The deepest directory a valid path can name ("/a/b/..."). */
#define TREE_DEPTH_MAX (VFS_PATH_MAX / 2u)

static int map_errno(int e) {
    switch (e) {
    case ENOENT: return GOGO_E_NOTFOUND;
    case EEXIST: return GOGO_E_EXISTS;
    case ENOSPC: return GOGO_E_NOSPACE;
    case EISDIR: return GOGO_E_ISDIR;
    case ENOTDIR: return GOGO_E_NOTDIR;
    case ENOTEMPTY: return GOGO_E_NOTEMPTY;
    case ELOOP: case ENAMETOOLONG: return GOGO_E_BADPATH;   /* a symlink (O_NOFOLLOW) */
    default: return GOGO_E_IO;
    }
}

/* The error of an openat of name in fd: a symlink is BadPath, whatever
 * the system says (Linux gives ENOTDIR with O_DIRECTORY, macOS ELOOP). */
static int open_err(int fd, const char *name, int e) {
    struct stat st;
    if ((e == ENOTDIR || e == ELOOP) && fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode))
        return GOGO_E_BADPATH;
    return map_errno(e);
}

static int clamp32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < 0 ? 0 : (int)v; }

/* Room given back (a truncation, removal or replacement). used never goes
 * below 0, whatever the host did behind the count. */
static void refund(FsDir *d, int64_t n) { d->used = n >= d->used ? 0 : d->used - n; }

static int64_t room(const FsDir *d) { return d->quota > d->used ? d->quota - d->used : 0; }

static void put_dir(const FsDir *d, int fd) {
    if (fd != d->root) close(fd);
}

/* Opens every directory of rel ("/a/b/c"; "/" is the root) before its last
 * component, one at a time from the root without following symlinks. buf
 * holds VFS_PATH_MAX + 1 bytes. On success *dir is the parent (d->root or
 * an fd for put_dir) and *leaf the last name in buf ("" for the root). */
static int walk(const FsDir *d, const char *rel, char *buf, int *dir, const char **leaf) {
    size_t n = strlen(rel);
    if (rel[0] != '/' || n > VFS_PATH_MAX) return GOGO_E_BADPATH;
    *dir = d->root;
    *leaf = buf + 1;
    if (n == 1u) {
        buf[1] = 0;
        return GOGO_E_OK;
    }
    /* Every component is checked before the first is opened. */
    for (const char *q = rel + 1;;) {
        size_t k = strcspn(q, "/");
        if (k == 0 || (q[0] == '.' && (k == 1 || (k == 2 && q[1] == '.')))) return GOGO_E_BADPATH;
        if (!q[k]) break;
        q += k + 1;
    }
    memcpy(buf, rel, n + 1u);
    int fd = d->root;
    char *p = buf + 1;
    for (;;) {
        char *s = strchr(p, '/');
        if (!s) break;
        *s = 0;
        int next = openat(fd, p, DIR_FLAGS);
        if (next < 0) {
            int e = open_err(fd, p, errno);
            put_dir(d, fd);
            return e;
        }
        put_dir(d, fd);
        fd = next;
        p = s + 1;
    }
    *dir = fd;
    *leaf = p;
    return GOGO_E_OK;
}

/* Writes n bytes at the position; *done = what was written. */
static int write_all(int fd, const uint8_t *buf, uint32_t n, uint32_t *done) {
    *done = 0;
    while (*done < n) {
        ssize_t k = write(fd, buf + *done, n - *done);
        if (k < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        *done += (uint32_t)k;
    }
    return GOGO_E_OK;
}

static int h_open(void *c, void *f, const char *rel, int mode) {
    FsDir *d = c;
    char buf[VFS_PATH_MAX + 1];
    const char *leaf;
    int dir;
    int e = walk(d, rel, buf, &dir, &leaf);
    if (e) return e;
    if (!*leaf) return GOGO_E_ISDIR;
    /* Look before opening: a device, FIFO or socket is refused unopened
     * (opening one can have side effects). */
    struct stat pre, st;
    bool existed = fstatat(dir, leaf, &pre, AT_SYMLINK_NOFOLLOW) == 0;
    if (existed && S_ISDIR(pre.st_mode)) e = GOGO_E_ISDIR;
    else if (existed && !S_ISREG(pre.st_mode)) e = GOGO_E_BADPATH;
    if (e) {
        put_dir(d, dir);
        return e;
    }
    /* Never O_TRUNC here: a file is checked before it is changed.
     * O_NONBLOCK and O_NOCTTY: should the name change to a FIFO or a
     * terminal after the look, the open neither blocks nor adopts it. */
    int fl = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY;
    fl |= mode == VFS_READ ? O_RDONLY : O_WRONLY | O_CREAT | (mode == VFS_APPEND ? O_APPEND : 0);
    int fd = openat(dir, leaf, fl, 0666);
    if (fd < 0) e = open_err(dir, leaf, errno);
    put_dir(d, dir);
    if (e) return e;
    if (fstat(fd, &st) != 0) e = map_errno(errno);
    else if (S_ISDIR(st.st_mode)) e = GOGO_E_ISDIR;
    else if (!S_ISREG(st.st_mode) || (existed && (st.st_dev != pre.st_dev || st.st_ino != pre.st_ino)) ||
             (mode != VFS_READ && st.st_nlink > 1)) e = GOGO_E_BADPATH;
    else if (fcntl(fd, F_SETFL, fl & O_APPEND) != 0) e = map_errno(errno);   /* drops O_NONBLOCK */
    else if (mode == VFS_WRITE && st.st_size > 0) {
        if (ftruncate(fd, 0) != 0) e = map_errno(errno);
        else refund(d, st.st_size);
    }
    if (e) {
        close(fd);
        return e;
    }
    ((H *)f)->fd = fd;
    ((H *)f)->append = mode == VFS_APPEND;
    return GOGO_E_OK;
}

static int h_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    (void)c;
    ssize_t k;
    do k = read(((H *)f)->fd, buf, n); while (k < 0 && errno == EINTR);
    if (k < 0) return map_errno(errno);
    *got = (uint32_t)k;
    return GOGO_E_OK;
}

/* The quota is charged by how much a write grows the file, measured
 * (fstat before and after), not by the bytes written: a write at a
 * position past the end (the file was truncated under an open handle)
 * pays for the hole it leaves too. */
static int h_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    FsDir *d = c;
    H *h = f;
    uint32_t done;
    if (d->quota < 0) return write_all(h->fd, buf, n, &done);
    struct stat before, after;
    off_t start;
    if (fstat(h->fd, &before) != 0) return map_errno(errno);
    if (h->append) start = before.st_size;
    else if ((start = lseek(h->fd, 0, SEEK_CUR)) < 0) return map_errno(errno);
    /* The most that fits: up to before.st_size + room. */
    int64_t fit = (int64_t)before.st_size + room(d) - (int64_t)start;
    uint32_t k = fit >= (int64_t)n ? n : fit > 0 ? (uint32_t)fit : 0u;
    int e = write_all(h->fd, buf, k, &done);
    if (fstat(h->fd, &after) == 0 && after.st_size > before.st_size) d->used += after.st_size - before.st_size;
    return e ? e : k < n ? GOGO_E_NOSPACE : GOGO_E_OK;
}

static int h_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    FsDir *d = c;
    struct stat st;
    /* vfs checked off + n against the size; a file shrunk since (by the
     * host) would grow, and that growth is charged. */
    if (d->quota >= 0) {
        if (fstat(((H *)f)->fd, &st) != 0) return map_errno(errno);
        int64_t grow = (int64_t)off + n - (int64_t)st.st_size;
        if (grow > room(d)) return GOGO_E_NOSPACE;
        if (grow > 0) d->used += grow;
    }
    while (n > 0) {
        ssize_t k = pwrite(((H *)f)->fd, buf, n, (off_t)off);   /* inside the file (vfs checked): no growth */
        if (k < 0) {
            if (errno == EINTR) continue;
            return map_errno(errno);
        }
        buf += k;
        off += (uint32_t)k;
        n -= (uint32_t)k;
    }
    return GOGO_E_OK;
}
static int h_sync(void *c, void *f) { (void)c; return fsync(((H *)f)->fd) == 0 ? GOGO_E_OK : map_errno(errno); }
static int h_size(void *c, void *f, int32_t *out) {
    (void)c;
    struct stat st;
    if (fstat(((H *)f)->fd, &st) != 0) return map_errno(errno);
    *out = clamp32(st.st_size);
    return GOGO_E_OK;
}
static int h_close(void *c, void *f) { (void)c; return close(((H *)f)->fd) == 0 ? GOGO_E_OK : map_errno(errno); }

static int h_stat(void *c, const char *rel, int32_t *size, bool *is_dir) {
    FsDir *d = c;
    char buf[VFS_PATH_MAX + 1];
    const char *leaf;
    int dir;
    struct stat st;
    int e = walk(d, rel, buf, &dir, &leaf);
    if (e) return e;
    if (!*leaf ? fstat(dir, &st) != 0 : fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) e = map_errno(errno);
    put_dir(d, dir);
    if (e) return e;
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return GOGO_E_BADPATH;
    *is_dir = S_ISDIR(st.st_mode);
    *size = *is_dir ? 0 : clamp32(st.st_size);
    return GOGO_E_OK;
}

static int h_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    FsDir *d = c;
    char buf[VFS_PATH_MAX + 1];
    const char *leaf;
    int dir;
    int e = walk(d, rel, buf, &dir, &leaf);
    if (e) return e;
    if (!*leaf) leaf = ".";
    int fd = openat(dir, leaf, DIR_FLAGS);
    if (fd < 0) e = open_err(dir, leaf, errno);
    put_dir(d, dir);
    if (e) return e;
    DIR *dp = fdopendir(fd);
    if (!dp) {
        e = map_errno(errno);
        close(fd);
        return e;
    }
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        struct stat st;
        /* Names that break the path rules are still listed (opening them
         * gives BadPath); so are symlinks, as files. Entries gone since
         * the listing are skipped. */
        if (fstatat(dirfd(dp), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (cb(ctx, ent->d_name, S_ISDIR(st.st_mode)) != 0) break;
    }
    closedir(dp);
    return GOGO_E_OK;
}

static int h_mkdir(void *c, const char *rel) {
    FsDir *d = c;
    char buf[VFS_PATH_MAX + 1];
    const char *leaf;
    int dir;
    int e = walk(d, rel, buf, &dir, &leaf);
    if (e) return e;
    if (!*leaf) return GOGO_E_EXISTS;
    if (mkdirat(dir, leaf, 0777) != 0) e = map_errno(errno);
    put_dir(d, dir);
    return e;
}

static int h_remove(void *c, const char *rel) {
    FsDir *d = c;
    char buf[VFS_PATH_MAX + 1];
    const char *leaf;
    int dir;
    struct stat st;
    int e = walk(d, rel, buf, &dir, &leaf);
    if (e) return e;
    if (!*leaf) return GOGO_E_BADPATH;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        unlinkat(dir, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) != 0) e = map_errno(errno);
    else if (S_ISREG(st.st_mode) && st.st_nlink == 1) refund(d, st.st_size);
    put_dir(d, dir);
    return e;
}

static int h_rename(void *c, const char *from, const char *to, bool replace) {
    FsDir *d = c;
    char abuf[VFS_PATH_MAX + 1], bbuf[VFS_PATH_MAX + 1];
    const char *la, *lb;
    int da, db;
    struct stat sa, sb;
    int e = walk(d, from, abuf, &da, &la);
    if (e) return e;
    e = walk(d, to, bbuf, &db, &lb);
    if (e) {
        put_dir(d, da);
        return e;
    }
    bool had = false;
    if (!*la || !*lb) e = GOGO_E_BADPATH;
    else if (fstatat(da, la, &sa, AT_SYMLINK_NOFOLLOW) != 0) e = map_errno(errno);
    else if ((had = fstatat(db, lb, &sb, AT_SYMLINK_NOFOLLOW) == 0) && !replace) e = GOGO_E_EXISTS;
    else if (renameat(da, la, db, lb) != 0) e = map_errno(errno);
    else if (had && S_ISREG(sb.st_mode) && sb.st_nlink == 1 && !(sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino))
        refund(d, sb.st_size);   /* the replaced file's bytes */
    put_dir(d, da);
    put_dir(d, db);
    return e;
}

static int h_usage(void *c, int32_t *used, int32_t *total) {
    FsDir *d = c;
    if (d->quota >= 0) {
        *total = clamp32(d->quota);
        *used = clamp32(d->used);
        return GOGO_E_OK;
    }
    struct statvfs sv;
    if (fstatvfs(d->root, &sv) != 0) return map_errno(errno);
    unsigned long long fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    unsigned long long t = fr * sv.f_blocks, u = t - fr * sv.f_bfree;
    *total = t > INT32_MAX ? INT32_MAX : (int32_t)t;
    *used = u > INT32_MAX ? INT32_MAX : (int32_t)u;
    return GOGO_E_OK;
}

const VfsBackend fs_dir_backend = { false, h_open, h_read, h_write, h_sync, h_size, h_close, h_stat, h_readdir,
                                    h_mkdir, h_remove, h_rename, h_usage, h_write_at };
/* The same on a host file system that ignores case: vfs then folds case
 * for its one-writer and Busy checks, as /a and /A are one file. */
static const VfsBackend fs_dir_backend_ci = { true, h_open, h_read, h_write, h_sync, h_size, h_close, h_stat,
                                              h_readdir, h_mkdir, h_remove, h_rename, h_usage, h_write_at };

const VfsBackend *fs_dir_vfs(const FsDir *d) { return d->ci ? &fs_dir_backend_ci : &fs_dir_backend; }

/* The bytes of the regular files under the directory fd (which it closes),
 * not following symlinks. */
static int64_t tree_bytes(int fd, unsigned depth) {
    DIR *dp = fdopendir(fd);
    if (!dp) {
        close(fd);
        return 0;
    }
    int64_t sum = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        struct stat st;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..") ||
            fstatat(dirfd(dp), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            sum += st.st_size;
        } else if (S_ISDIR(st.st_mode) && depth < TREE_DEPTH_MAX) {
            int sub = openat(dirfd(dp), ent->d_name, DIR_FLAGS);
            if (sub >= 0) sum += tree_bytes(sub, depth + 1u);
        }
    }
    closedir(dp);
    return sum;
}

/* Whether the file system under the directory fd ignores case. macOS
 * answers directly; elsewhere a probe file is made under a lower-case
 * name and looked up under the upper-case one. If it cannot be made, the
 * file system is taken to respect case. */
static bool ignores_case(int root) {
#if defined(_PC_CASE_SENSITIVE)
    long v = fpathconf(root, _PC_CASE_SENSITIVE);
    if (v >= 0) return v == 0;
#endif
    char lo[48], up[48];
    snprintf(lo, sizeof lo, ".gogo-case-%lx-%lx", (unsigned long)getpid(), (unsigned long)(uintptr_t)lo);
    for (size_t i = 0; i < sizeof lo; i++) up[i] = (char)toupper((unsigned char)lo[i]);
    int fd = openat(root, lo, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    struct stat a, b;
    bool same = fstat(fd, &a) == 0 && fstatat(root, up, &b, AT_SYMLINK_NOFOLLOW) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    close(fd);
    unlinkat(root, lo, 0);
    return same;
}

FsDir *fs_dir_open(const gogo_host *h, void *ctx, const char *dir, int64_t quota, int *err) {
    int e;
    if (!err) err = &e;
    *err = GOGO_EINVAL;
    if (!h || !dir) return NULL;
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NULL;
    FsDir *d = h->alloc(ctx, sizeof *d);
    if (!d) {
        close(fd);
        *err = GOGO_ENOMEM;
        return NULL;
    }
    *err = GOGO_OK;
    d->h = h;
    d->hctx = ctx;
    d->root = fd;
    d->quota = quota < 0 ? -1 : quota;
    d->used = 0;
    d->ci = ignores_case(fd);
    if (quota >= 0) {
        int t = openat(fd, ".", DIR_FLAGS);
        if (t >= 0) d->used = tree_bytes(t, 0);
    }
    return d;
}

void fs_dir_close(FsDir *d) {
    if (!d) return;
    close(d->root);
    d->h->free(d->hctx, d, sizeof *d);
}
