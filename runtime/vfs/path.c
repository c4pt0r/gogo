/* Path validation and hashing (spec §5.4). */
#include "vfs_int.h"

static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

int vfs_path_check(const char *p, uint32_t len, uint32_t *first) {
    if (len < 2 || len > VFS_PATH_MAX || p[0] != '/') return VFS_BADPATH;
    uint32_t i = 1;
    *first = 0;
    while (i < len) {
        uint32_t s = i;
        while (i < len && p[i] != '/') {
            if (!name_char(p[i])) return VFS_BADPATH;
            i++;
        }
        uint32_t n = i - s;
        if (n == 0 || n > VFS_NAME_MAX) return VFS_BADPATH;      /* empty segment or trailing '/' */
        /* A name ending in '.' (this covers "." and ".."): FatFs strips
         * trailing dots, so "a.txt." would alias "a.txt" past the slot hash. */
        if (p[i - 1] == '.') return VFS_BADPATH;
        if (!*first) *first = i;
        if (i < len) {
            i++;
            if (i == len) return VFS_BADPATH;                    /* trailing '/' */
        }
    }
    return VFS_OK;
}

uint32_t vfs_path_hash(const char *p, uint32_t len, bool lower) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)p[i];
        if (lower && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}
