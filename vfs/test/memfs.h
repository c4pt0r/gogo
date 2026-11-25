/* In-memory fake backend for vfs tests: two independent instances, a byte
 * budget (NoSpace), a case-insensitive switch, and a switch for whether
 * rename(replace=true) works (LittleFS-like) or returns Exists (FAT-like). */
#ifndef GOGO_MEMFS_H
#define GOGO_MEMFS_H
#include "vfs.h"

extern VfsBackend memfs_be[2];

/* Reset instance i (all files gone); budget < 0 = unlimited. */
void memfs_init(int i, bool case_insensitive, bool atomic_replace, int32_t budget);
void memfs_set_budget(int i, int32_t budget);
int memfs_rename_calls(int i);          /* count of rename(replace=true) attempts */
void memfs_set_stat_err(int i, int err); /* nonzero: every stat returns err */
void memfs_set_close_fails(int i, bool fails);   /* close returns VFS_IO */
void memfs_fail_next_rename(int i, int err);   /* next rename(replace=false) returns err, once */
int memfs_name(int i, const char *rel, char *out, uint32_t cap);   /* stored name of rel */
#endif
