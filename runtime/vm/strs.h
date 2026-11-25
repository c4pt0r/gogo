/* Sequence builtins (strs.c): strings (concatenation, len, indexing,
 * slicing, ordering, content equality), slices (make, len, cap, index
 * read/write, slicing, append, copy, []byte <-> string), bytes.Bytes and
 * bytes.Buffer. */
#ifndef GOGO_STRS_H
#define GOGO_STRS_H

#include <stdint.h>
#include <stdbool.h>
#include "actor.h"

/* Runs sequence builtin `id` (B_STR_CONCAT, B_LEN, B_INDEX, B_SETINDEX,
 * B_SLICE, B_MAKE_SLICE, B_APPEND, B_COPY, B_CAP, B_STR_FROM,
 * B_BYTESLICE_FROM, B_STR_CMP, B_BUF_NEW, B_BUF_WRITE, B_BUF_FREEZE,
 * B_BYTES_FROM, B_BYTES_EQUAL, B_BYTES_COPYTO, B_CONFIG_GET) for BUILTIN
 * ra, rb, rc: arguments in registers rb..rb+rc-1 of a's top frame, result
 * into register ra (B_CONFIG_GET: value and ok into ra and ra+1). The
 * caller has checked that ra (ra+1 for B_CONFIG_GET) and rb..rb+rc-1 are
 * registers of the frame. Returns R_NORMAL, R_BAD_CODE
 * (wrong argument count or kind, index or slice bounds out of range) or
 * R_OUT_OF_MEMORY. May GC, so the caller reloads its register pointer
 * after every call. Writes ra only on success, after reading every
 * argument. */
uint32_t strs_builtin(Vm *vm, Actor *a, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc);

/* True if both terms are strings; then *eq is whether their bytes are
 * equal (const and heap strings, in any combination). */
bool strs_equal(const Vm *vm, const Actor *a, term x, term y, bool *eq);

#endif
