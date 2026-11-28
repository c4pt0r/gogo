/* fs_dir_win.c: runtime/embed/fs_dir.c for the Windows host, a vfs medium
 * over a host directory (gogovm's --fs-root, /flash and /sd).
 *
 * Same contract and the same sandbox rules as the POSIX file (fs_dir.h,
 * runtime/vfs/vfs.h), with the *at family replaced by full UTF-16 paths
 * under the root:
 *
 *  - every access is built as <root>\<rel>, and rel comes from the vfs: it
 *    starts with '/', is at most VFS_PATH_MAX bytes, and a component that is
 *    empty, "." or "..", or holds a character Windows cannot name, is
 *    BadPath. So a program's path can never name anything outside the root;
 *  - a reparse point (symlink or junction) anywhere in the path is BadPath,
 *    the way O_NOFOLLOW refuses one: the leaf is opened with
 *    FILE_FLAG_OPEN_REPARSE_POINT and the ancestors are attributed before
 *    the leaf is touched;
 *  - a file with other hard links is BadPath when opened for writing (its
 *    other names may lie outside); reading it is allowed;
 *  - Remove and Rename act on the names themselves; ReadDir lists every
 *    entry (a reparse point is listed, as a file);
 *  - quota < 0: the host volume's usage (GetDiskFreeSpaceEx); else the bytes
 *    the regular files under the root may hold in all, counted at open and
 *    then tracked by charging growth and refunding truncation, removal and
 *    replacement. A write past it stores what still fits and fails with
 *    NoSpace.
 *
 * The host file system ignores case, so the backend is the case-insensitive
 * one (vfs then treats /a and /A as the one file, and a Write open of a
 * path open for reading is Busy, as on a FAT medium).
 */
#include "fs_dir.h"

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct FsDir {
    const gogo_host *h;
    void *hctx;
    wchar_t root[MAX_PATH];      /* absolute, no trailing backslash (but "D:\") */
    size_t root_len;
    int64_t quota;               /* -1: none */
    int64_t used;                /* bytes in regular files under root, >= 0 */
};

typedef struct { HANDLE fh; bool append; } H;
typedef char h_fits[sizeof(H) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

/* The deepest directory a valid path can name ("/a/b/..."). */
#define TREE_DEPTH_MAX (VFS_PATH_MAX / 2u)
/* A full Win32 path: the root plus the media path (UTF-16 is never longer
 * than the UTF-8 bytes it came from, plus one backslash and a NUL). */
typedef wchar_t PathBuf[MAX_PATH + VFS_PATH_MAX + 8];

static int map_err(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
        return GOGO_E_NOTFOUND;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return GOGO_E_EXISTS;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return GOGO_E_NOSPACE;
    case ERROR_DIR_NOT_EMPTY:
        return GOGO_E_NOTEMPTY;
    case ERROR_DIRECTORY:
        return GOGO_E_NOTDIR;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_FILENAME_EXCED_RANGE:
        return GOGO_E_BADPATH;
    default:
        return GOGO_E_IO;
    }
}

static int clamp32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < 0 ? 0 : (int)v; }

/* Room given back (a truncation, removal or replacement). used never goes
 * below 0, whatever the host did behind the count. */
static void refund(FsDir *d, int64_t n) { d->used = n >= d->used ? 0 : d->used - n; }

static int64_t room(const FsDir *d) { return d->quota > d->used ? d->quota - d->used : 0; }

static int64_t size_of(const BY_HANDLE_FILE_INFORMATION *bi) {
    return ((int64_t)bi->nFileSizeHigh << 32) | (int64_t)bi->nFileSizeLow;
}

/* ---- the sandbox's path rules ---- */

/* True for a component the media may not name: empty, "." or "..", a
 * character Windows reserves, or a trailing dot or space (which Windows
 * would silently drop, naming another file). */
static bool bad_component(const char *p, size_t n) {
    if (n == 0) return true;
    if (n == 1u && p[0] == '.') return true;
    if (n == 2u && p[0] == '.' && p[1] == '.') return true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20u || c == 0x7fu) return true;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') return true;
    }
    if (p[n - 1] == '.' || p[n - 1] == ' ') return true;
    return false;
}

/* The Win32 path of rel ("/a/b"; "/" is the root), and whether it is the
 * root. rel is checked before anything is touched. */
static int full_path(const FsDir *d, const char *rel, PathBuf out, bool *is_root) {
    size_t n = strlen(rel);
    if (n == 0 || rel[0] != '/' || n > VFS_PATH_MAX) return GOGO_E_BADPATH;
    if (n == 1u) {
        *is_root = true;
        if (d->root_len + 1u > (size_t)MAX_PATH + VFS_PATH_MAX + 8u) return GOGO_E_BADPATH;
        memcpy(out, d->root, (d->root_len + 1u) * sizeof(wchar_t));
        return GOGO_E_OK;
    }
    *is_root = false;
    for (const char *q = rel + 1;;) {
        size_t k = strcspn(q, "/");
        if (bad_component(q, k)) return GOGO_E_BADPATH;
        if (!q[k]) break;
        q += k + 1;
    }
    wchar_t wrel[VFS_PATH_MAX + 1];
    int m = MultiByteToWideChar(CP_UTF8, 0, rel + 1, (int)(n - 1u), wrel, VFS_PATH_MAX);
    if (m <= 0) return GOGO_E_BADPATH;
    for (int i = 0; i < m; i++) {
        if (wrel[i] == L'/') wrel[i] = L'\\';
    }
    wrel[m] = 0;
    if (d->root_len + 1u + (size_t)m + 1u > (size_t)MAX_PATH + VFS_PATH_MAX + 8u) return GOGO_E_BADPATH;
    memcpy(out, d->root, d->root_len * sizeof(wchar_t));
    out[d->root_len] = L'\\';
    memcpy(out + d->root_len + 1u, wrel, ((size_t)m + 1u) * sizeof(wchar_t));
    return GOGO_E_OK;
}

/* Refuses a path whose ancestors include a reparse point (a symlink or a
 * junction: the sandbox must not be escaped through one) or a plain file,
 * as the unix walk's openat(O_NOFOLLOW|O_DIRECTORY) does. */
static int check_ancestors(const FsDir *d, PathBuf path) {
    for (size_t i = d->root_len; path[i]; i++) {
        if (path[i] != L'\\') continue;
        path[i] = 0;
        DWORD a = GetFileAttributesW(path);
        path[i] = L'\\';
        if (a == INVALID_FILE_ATTRIBUTES) return map_err(GetLastError());
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return GOGO_E_NOTDIR;
    }
    return GOGO_E_OK;
}

/* ---- the file handles ---- */

static int h_open(void *c, void *f, const char *rel, int mode) {
    FsDir *d = c;
    PathBuf path;
    bool root;
    int e = full_path(d, rel, path, &root);
    if (e) return e;
    if (root) return GOGO_E_ISDIR;
    e = check_ancestors(d, path);
    if (e) return e;
    DWORD attr = GetFileAttributesW(path);
    bool existed = attr != INVALID_FILE_ATTRIBUTES;
    if (existed) {
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) return GOGO_E_ISDIR;
    }
    DWORD access = mode == VFS_READ ? GENERIC_READ : GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD disp = mode == VFS_READ ? OPEN_EXISTING : OPEN_ALWAYS;
    HANDLE h = CreateFileW(path, access, share, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return map_err(GetLastError());
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(h, &bi)) {
        e = map_err(GetLastError());
    } else if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        e = GOGO_E_BADPATH;
    } else if (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        e = GOGO_E_ISDIR;
    } else if (mode != VFS_READ && bi.nNumberOfLinks > 1) {
        e = GOGO_E_BADPATH;              /* another name may lie outside the root */
    } else if (mode == VFS_WRITE) {
        LARGE_INTEGER zero;
        int64_t was = size_of(&bi);
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) || !SetEndOfFile(h)) e = map_err(GetLastError());
        else refund(d, was);
    } else if (mode == VFS_APPEND) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        SetFilePointerEx(h, zero, NULL, FILE_END);
    }
    if (e) {
        CloseHandle(h);
        return e;
    }
    ((H *)f)->fh = h;
    ((H *)f)->append = mode == VFS_APPEND;
    return GOGO_E_OK;
}

static int h_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    (void)c;
    DWORD k = 0;
    *got = 0;
    if (n == 0) return GOGO_E_OK;
    if (!ReadFile(((H *)f)->fh, buf, n, &k, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return GOGO_E_OK;
        return map_err(e);
    }
    *got = (uint32_t)k;
    return GOGO_E_OK;
}

/* Writes n bytes at the position (*done = what was written). */
static int write_all(HANDLE h, const uint8_t *buf, uint32_t n, uint32_t *done) {
    *done = 0;
    while (*done < n) {
        DWORD k = 0;
        if (!WriteFile(h, buf + *done, n - *done, &k, NULL)) return map_err(GetLastError());
        if (k == 0) return GOGO_E_NOSPACE;
        *done += (uint32_t)k;
    }
    return GOGO_E_OK;
}

static int h_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    FsDir *d = c;
    H *h = f;
    uint32_t done;
    if (d->quota < 0) return write_all(h->fh, buf, n, &done);
    BY_HANDLE_FILE_INFORMATION before, after;
    LARGE_INTEGER pos, zero;
    if (!GetFileInformationByHandle(h->fh, &before)) return map_err(GetLastError());
    zero.QuadPart = 0;
    if (h->append) {
        if (!SetFilePointerEx(h->fh, zero, &pos, FILE_END)) return map_err(GetLastError());
    } else if (!SetFilePointerEx(h->fh, zero, &pos, FILE_CURRENT)) {
        return map_err(GetLastError());
    }
    /* The most that fits: up to before's size + room. */
    int64_t fit = size_of(&before) + room(d) - pos.QuadPart;
    uint32_t k = fit >= (int64_t)n ? n : fit > 0 ? (uint32_t)fit : 0u;
    int e = write_all(h->fh, buf, k, &done);
    if (GetFileInformationByHandle(h->fh, &after) && size_of(&after) > size_of(&before)) {
        d->used += size_of(&after) - size_of(&before);
    }
    return e ? e : k < n ? GOGO_E_NOSPACE : GOGO_E_OK;
}

static int h_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    FsDir *d = c;
    BY_HANDLE_FILE_INFORMATION bi;
    LARGE_INTEGER pos;
    if (d->quota >= 0) {
        if (!GetFileInformationByHandle(((H *)f)->fh, &bi)) return map_err(GetLastError());
        int64_t grow = (int64_t)off + n - size_of(&bi);
        if (grow > room(d)) return GOGO_E_NOSPACE;
        if (grow > 0) d->used += grow;
    }
    pos.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(((H *)f)->fh, pos, NULL, FILE_BEGIN)) return map_err(GetLastError());
    while (n > 0) {
        DWORD k = 0;
        if (!WriteFile(((H *)f)->fh, buf, n, &k, NULL)) return map_err(GetLastError());
        if (k == 0) return GOGO_E_NOSPACE;
        buf += k;
        off += k;
        n -= k;
    }
    return GOGO_E_OK;
}

static int h_sync(void *c, void *f) {
    (void)c;
    return FlushFileBuffers(((H *)f)->fh) ? GOGO_E_OK : map_err(GetLastError());
}

static int h_size(void *c, void *f, int32_t *out) {
    (void)c;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(((H *)f)->fh, &sz)) return map_err(GetLastError());
    *out = clamp32(sz.QuadPart);
    return GOGO_E_OK;
}

static int h_close(void *c, void *f) {
    (void)c;
    return CloseHandle(((H *)f)->fh) ? GOGO_E_OK : map_err(GetLastError());
}

static int h_stat(void *c, const char *rel, int32_t *size, bool *is_dir) {
    FsDir *d = c;
    PathBuf path;
    bool root;
    int e = full_path(d, rel, path, &root);
    if (e) return e;
    if (!root) {
        e = check_ancestors(d, path);
        if (e) return e;
    }
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return map_err(GetLastError());
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        *is_dir = true;
        *size = 0;
        return GOGO_E_OK;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return map_err(GetLastError());
        *is_dir = false;
        *size = clamp32(((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow);
    }
    return GOGO_E_OK;
}

/* path = dir + "\" + name (name NULL: dir + "\*"), the Win32 glob for
 * FindFirstFileW. Built by hand: with __USE_MINGW_ANSI_STDIO, %s in a wide
 * printf means a *narrow* string, so swprintf cannot join wide paths. */
static bool join_wide(const wchar_t *dir, const wchar_t *name, wchar_t *out, size_t cap) {
    size_t dn = wcslen(dir);
    size_t nn = name ? wcslen(name) : 1u;
    if (dn + 1u + nn + 1u > cap) return false;
    memcpy(out, dir, dn * sizeof(wchar_t));
    out[dn] = L'\\';
    if (name) memcpy(out + dn + 1u, name, nn * sizeof(wchar_t));
    else out[dn + 1u] = L'*';
    out[dn + 1u + nn] = 0;
    return true;
}

static int h_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    FsDir *d = c;
    PathBuf path;
    bool root;
    int e = full_path(d, rel, path, &root);
    if (e) return e;
    if (!root) {
        e = check_ancestors(d, path);
        if (e) return e;
    }
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return map_err(GetLastError());
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return GOGO_E_NOTDIR;
    wchar_t pat[MAX_PATH + VFS_PATH_MAX + 8];
    if (!join_wide(path, NULL, pat, MAX_PATH + VFS_PATH_MAX + 8)) return GOGO_E_BADPATH;
    WIN32_FIND_DATAW fd;
    HANDLE it = FindFirstFileW(pat, &fd);
    if (it == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND ? GOGO_E_OK : map_err(err);   /* empty directory */
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char name[VFS_PATH_MAX + 1];
        int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        if (n <= 0) continue;
        /* A reparse point is listed as a file (opening it is BadPath). */
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                      (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        if (cb(ctx, name, is_dir) != 0) break;
    } while (FindNextFileW(it, &fd));
    FindClose(it);
    return GOGO_E_OK;
}

static int h_mkdir(void *c, const char *rel) {
    FsDir *d = c;
    PathBuf path;
    bool root;
    int e = full_path(d, rel, path, &root);
    if (e) return e;
    if (root) return GOGO_E_EXISTS;
    e = check_ancestors(d, path);
    if (e) return e;
    if (!CreateDirectoryW(path, NULL)) {
        DWORD err = GetLastError();
        return err == ERROR_ALREADY_EXISTS ? GOGO_E_EXISTS : map_err(err);
    }
    return GOGO_E_OK;
}

static int h_remove(void *c, const char *rel) {
    FsDir *d = c;
    PathBuf path;
    bool root;
    int e = full_path(d, rel, path, &root);
    if (e) return e;
    if (root) return GOGO_E_BADPATH;
    e = check_ancestors(d, path);
    if (e) return e;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return map_err(GetLastError());
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryW(path)) return map_err(GetLastError());
        return GOGO_E_OK;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        int64_t was = 0;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            was = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
        }
        if (!DeleteFileW(path)) return map_err(GetLastError());
        refund(d, was);
    }
    return GOGO_E_OK;
}

static int h_rename(void *c, const char *from, const char *to, bool replace) {
    FsDir *d = c;
    PathBuf pa, pb;
    bool ra, rb;
    int e = full_path(d, from, pa, &ra);
    if (e) return e;
    e = full_path(d, to, pb, &rb);
    if (e) return e;
    if (ra || rb) return GOGO_E_BADPATH;
    e = check_ancestors(d, pa);
    if (e) return e;
    e = check_ancestors(d, pb);
    if (e) return e;
    DWORD a = GetFileAttributesW(pa);
    if (a == INVALID_FILE_ATTRIBUTES) return map_err(GetLastError());
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return GOGO_E_BADPATH;
    DWORD b = GetFileAttributesW(pb);
    bool had = b != INVALID_FILE_ATTRIBUTES;
    if (had && !replace) return GOGO_E_EXISTS;
    if (had && (b & FILE_ATTRIBUTE_DIRECTORY)) return GOGO_E_ISDIR;   /* not a replaceable name */
    if (!MoveFileExW(pa, pb, had ? MOVEFILE_REPLACE_EXISTING : 0)) return map_err(GetLastError());
    if (had) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(pb, GetFileExInfoStandard, &fad)) {
            /* the new file's bytes were charged by whoever wrote them; the
             * replaced one's are given back */
            refund(d, 0);
        }
    }
    return GOGO_E_OK;
}

static int h_usage(void *c, int32_t *used, int32_t *total) {
    FsDir *d = c;
    if (d->quota >= 0) {
        *total = clamp32(d->quota);
        *used = clamp32(d->used);
        return GOGO_E_OK;
    }
    ULARGE_INTEGER freeb, totalb;
    if (!GetDiskFreeSpaceExW(d->root, &freeb, &totalb, NULL)) return map_err(GetLastError());
    *total = clamp32((int64_t)totalb.QuadPart);
    *used = clamp32((int64_t)(totalb.QuadPart - freeb.QuadPart));
    return GOGO_E_OK;
}

const VfsBackend fs_dir_backend = { true, h_open, h_read, h_write, h_sync, h_size, h_close, h_stat, h_readdir,
                                    h_mkdir, h_remove, h_rename, h_usage, h_write_at };
/* Windows ignores case on its own file systems, so this is the table every
 * mount uses: vfs then folds case for its one-writer and Busy checks. */
static const VfsBackend fs_dir_backend_ci = { true, h_open, h_read, h_write, h_sync, h_size, h_close, h_stat,
                                              h_readdir, h_mkdir, h_remove, h_rename, h_usage, h_write_at };

const VfsBackend *fs_dir_vfs(const FsDir *d) { (void)d; return &fs_dir_backend_ci; }

/* ---- opening the medium ---- */

/* The bytes of the regular files under dir, not following reparse points. */
static int64_t tree_bytes(const wchar_t *dir, unsigned depth) {
    wchar_t pat[MAX_PATH + VFS_PATH_MAX + 8];
    if (!join_wide(dir, NULL, pat, MAX_PATH + VFS_PATH_MAX + 8)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE it = FindFirstFileW(pat, &fd);
    if (it == INVALID_HANDLE_VALUE) return 0;
    int64_t sum = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        int64_t sz = ((int64_t)fd.nFileSizeHigh << 32) | (int64_t)fd.nFileSizeLow;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth >= TREE_DEPTH_MAX) continue;
            wchar_t sub[MAX_PATH + VFS_PATH_MAX + 8];
            if (join_wide(dir, fd.cFileName, sub, MAX_PATH + VFS_PATH_MAX + 8)) {
                sum += tree_bytes(sub, depth + 1u);
            }
        } else {
            sum += sz;
        }
    } while (FindNextFileW(it, &fd));
    FindClose(it);
    return sum;
}

FsDir *fs_dir_open(const gogo_host *h, void *ctx, const char *dir, int64_t quota, int *err) {
    int e;
    if (!err) err = &e;
    *err = GOGO_EINVAL;
    if (!h || !dir) return NULL;
    wchar_t wdir[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH) <= 0) return NULL;
    DWORD a = GetFileAttributesW(wdir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(wdir, MAX_PATH, full, NULL);
    if (n == 0 || n >= MAX_PATH) return NULL;
    FsDir *d = h->alloc(ctx, sizeof *d);
    if (!d) {
        *err = GOGO_ENOMEM;
        return NULL;
    }
    memset(d, 0, sizeof *d);
    d->h = h;
    d->hctx = ctx;
    memcpy(d->root, full, ((size_t)n + 1u) * sizeof(wchar_t));
    d->root_len = n;
    while (d->root_len > 3u && d->root[d->root_len - 1u] == L'\\') d->root[--d->root_len] = 0;
    d->quota = quota < 0 ? -1 : quota;
    d->used = quota >= 0 ? tree_bytes(d->root, 0) : 0;
    *err = GOGO_OK;
    return d;
}

void fs_dir_close(FsDir *d) {
    if (!d) return;
    d->h->free(d->hctx, d, sizeof *d);
}
