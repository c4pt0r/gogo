/* gogoc.h: the gogo compiler as a C library (libgogoc.a), so that a
 * program embedding gogo (gogo.h, libgogo.a) can compile source at run
 * time, in process, as `gogo build` does.
 *
 * libgogoc.a is a Go c-archive: it carries the Go runtime, which starts
 * when the host program starts (a few threads, some megabytes of address
 * space) and is shared by every call. A process may link one Go
 * c-archive at most, and a Go program does not link it: it imports
 * gogotools/embed and calls embed.Compile. The Go runtime installs signal
 * handlers of its own: a host that handles signals itself (SIGSEGV,
 * SIGPIPE, ...) reads the notes on non-Go programs calling Go code in the
 * documentation of Go's os/signal package.
 *
 *   make libgogoc            # build/libgogoc.a
 *   cc -Iembed/gogoc -Iruntime/include -o host host.c build/libgogoc.a build/libgogo.a -pthread
 *
 * The library exports exactly the gogoc_* functions below; the Go
 * runtime's own symbols are local to it.
 *
 * Every function may be called from any thread, also at the same time. */
#ifndef GOGOC_H
#define GOGOC_H

#include <stddef.h>
#include <stdint.h>

/* Marks the API functions, as GOGO_API does in gogo.h. */
#ifndef GOGOC_API
#if defined(__GNUC__) || defined(__clang__)
#define GOGOC_API __attribute__((visibility("default")))
#else
#define GOGOC_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* gogoc_compile flags. Without GOGOC_SOURCE, input is a path: a
 * directory (the build root, its .gg files package main) or one .gg
 * file, read in place. */
#define GOGOC_SOURCE   1u  /* input is the text of one source file, package main */
#define GOGOC_GOGOPATH 2u  /* search $GOGOPATH (else $HOME/gogo) for user packages,
                            * as gogo build does; without it only the build root
                            * and the gogo libraries are read */

/* Compiles input (input_len bytes, under 2 GiB, need not be
 * NUL-terminated) to an
 * image, which it stores in *out (*out_len bytes, from malloc: free it
 * with gogoc_free) for gogo_load. Returns 0, or -1 if the program does
 * not compile, or on a bad argument (an unknown flag, a NULL out or
 * out_len, input NULL with a length), with *out NULL.
 * err may be NULL. If not, *err is set: on failure to the message
 * (positions such as "main.gg:3:5: ..."), on success to the warnings
 * gogo build would print, one per line, or NULL if there are none; a
 * non-NULL *err is a NUL-terminated string to free with gogoc_free. */
GOGOC_API int gogoc_compile(const char *input, size_t input_len, uint32_t flags, uint8_t **out, size_t *out_len, char **err);

/* Frees what gogoc_compile returned (an image or a message). NULL is
 * ignored. */
GOGOC_API void gogoc_free(void *p);

/* The newest image format the compiler emits, (major << 16) | minor,
 * as gogo_image_format() gives a VM's. An image is stamped with the
 * lowest minor its program needs, so a VM whose major is the same runs
 * every image if its minor is at least this one, and most images
 * anyway (gogo_load says GOGO_EVERSION otherwise). */
GOGOC_API uint32_t gogoc_image_format(void);

#ifdef __cplusplus
}
#endif

#endif
