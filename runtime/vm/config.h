/* Platform-tunable compile-time constants. Override with -D. */
#ifndef GOGO_CONFIG_H
#define GOGO_CONFIG_H

/* Per-actor memory limit in words (heap block + stack + queued messages)
 * for an actor spawned without an explicit limit. A board build sets it
 * (4096: 16 KB); the host's is 1 MB. */
#ifndef GOGO_DEFAULT_MAX_WORDS
#define GOGO_DEFAULT_MAX_WORDS 262144u
#endif

/* The word counts of an actor's memory (Actor.mem_words, hp, sp, fp,
 * mbox_words, max_words; frames, the GC, message charges): 16 bits in the
 * board configuration (GOGO_DEFAULT_MAX_WORDS <= 65535), which keeps the
 * board's Actor at 48 bytes, so an actor holds at most 65535 words there
 * (~256 KB); 32 bits otherwise (the host). GOGO_ACTOR_MAX_WORDS is the
 * ceiling of an actor's memory: 65535 words on a board, 2^28 (1 GB) on
 * the host, where heap refs (word offset << 2, term.h) leave 30 bits. */
#include <stdint.h>
#ifndef GOGO_WORDS_16   /* its own knob too: a host build may lower the default alone */
#if GOGO_DEFAULT_MAX_WORDS <= 65535u
#define GOGO_WORDS_16 1
#else
#define GOGO_WORDS_16 0
#endif
#endif
#if GOGO_WORDS_16
typedef uint16_t gogo_words_t;
#define GOGO_WORDS_MAX 65535u
#else
typedef uint32_t gogo_words_t;
#define GOGO_WORDS_MAX 0xFFFFFFFFu
#endif
#ifndef GOGO_ACTOR_MAX_WORDS
#if GOGO_WORDS_16
#define GOGO_ACTOR_MAX_WORDS 65535u
#else
#define GOGO_ACTOR_MAX_WORDS (1u << 28)
#endif
#endif
#if GOGO_ACTOR_MAX_WORDS > GOGO_WORDS_MAX || GOGO_ACTOR_MAX_WORDS > (1u << 28) || GOGO_DEFAULT_MAX_WORDS > GOGO_ACTOR_MAX_WORDS
#error "GOGO_ACTOR_MAX_WORDS must fit gogo_words_t and heap refs, and hold GOGO_DEFAULT_MAX_WORDS"
#endif

/* Emergency memory the VM reserves at startup to finish exit processing
 * when the whole machine is out of memory. */
#ifndef GOGO_RESERVE_BYTES
#define GOGO_RESERVE_BYTES 1024u
#endif

/* Actor table capacity: the most live actors at once. The unix `chain`
 * golden needs 10002. */
#ifndef GOGO_TABLE_CAP
#define GOGO_TABLE_CAP 16384u
#endif

/* Port table size and event queue capacity (used by the port layer). */
#ifndef GOGO_MAX_PORTS
/* The default most ports open at once per Vm (a Vm may set its own,
 * vm_set_max_ports, at most PORT_MAX_CEILING): 16 on a board; host builds
 * pass a larger one (the Makefile: 1024). The table grows as ports open. */
#define GOGO_MAX_PORTS 16u
#endif
/* The default most node links of distributed actors (runtime/dist,
 * DIST_LINKS) when the config key node.max_links is not set: 8 on a
 * board; gogovm passes its own (512). */
#ifndef GOGO_MAX_LINKS
#define GOGO_MAX_LINKS 8u
#endif
/* Extra event sources (net, input, the screen port's Done retry, the gpio
 * driver's debounce) the port layer folds in. */
#ifndef GOGO_MAX_SOURCES
#define GOGO_MAX_SOURCES 4u
#endif
#ifndef GOGO_EVQ_CAP
#define GOGO_EVQ_CAP 64u
#endif

/* Cap on bytes the unix platform may take from the system for VM memory
 * (simulates a small MCU); 0 means no cap. */
#ifndef GOGO_ARENA_BYTES
#define GOGO_ARENA_BYTES 0u
#endif

/* Off-heap bytes.Bytes (bytes.h): the default handle table entries for
 * the whole VM and handles one actor may hold at once (config key
 * vm.handles, runtime/board/params.h; 64 and 16 on a board, 4096 and 256 on the
 * host), the most entries a VM may have (it sizes the distinct-root sets
 * on the C stack: 1024 on a board, 65535, the most a handle index holds,
 * on the host), and the largest Buffer.Freeze result kept on the heap
 * (larger ones go off-heap). */
#ifndef GOGO_BYTES_HANDLES
#if GOGO_WORDS_16
#define GOGO_BYTES_HANDLES 64u
#else
#define GOGO_BYTES_HANDLES 4096u
#endif
#endif
#ifndef GOGO_BYTES_PER_ACTOR
#if GOGO_WORDS_16
#define GOGO_BYTES_PER_ACTOR 16u
#else
#define GOGO_BYTES_PER_ACTOR 256u
#endif
#endif
#ifndef GOGO_BYTES_HANDLES_MAX
#if GOGO_WORDS_16
#define GOGO_BYTES_HANDLES_MAX 1024u
#else
#define GOGO_BYTES_HANDLES_MAX 65535u
#endif
#endif
#ifndef GOGO_BYTES_INLINE
#define GOGO_BYTES_INLINE 64u
#endif

/* Module table (module.h): loaded modules at once, the boot image (module
 * 0) included. At most 8: the slot is 3 bits of a module tag (term.h). */
#ifndef GOGO_MAX_MODULES
#define GOGO_MAX_MODULES 8u
#endif
/* Code memory limit in bytes for all loaded modules together (the boot
 * image aside) when the config key code.mem is not set (module.h): 64 KB
 * on a board, 16 MB on the host. */
#ifndef GOGO_CODE_MEM_DEFAULT
#if GOGO_WORDS_16
#define GOGO_CODE_MEM_DEFAULT 65536u
#else
#define GOGO_CODE_MEM_DEFAULT 16777216u
#endif
#endif
#if GOGO_MAX_MODULES < 1 || GOGO_MAX_MODULES > 8
#error "GOGO_MAX_MODULES must be 1..8"
#endif

/* Floating point (float32, float64; floats.h): 1 builds the float ops,
 * K_FLOAT, float println and the gogo/math / strconv float builtins
 * (runtime/floatlib), and the VM then has the requirement "float"; 0 leaves
 * them out, and an image that uses floats is refused at load ("image
 * requires float, which this VM lacks"). ESP: Kconfig GOGO_FLOAT. */
#ifndef GOGO_FLOAT
#define GOGO_FLOAT 1
#endif

/* 1: debug build (the sanitizer test builds, host only). GOGO_ASSERT checks
 * internal invariants and aborts with the failed expression on stderr;
 * va_alloc keeps each block's size in front of it for va_free to check.
 * 0 (every other build): GOGO_ASSERT compiles to nothing. Host config only:
 * the 8-byte size header on every block does not fit the MCU arena
 * (GOGO_ARENA_BYTES; test_c_mcu's timer_heap_order runs out of memory). */
#ifndef GOGO_DEBUG
#define GOGO_DEBUG 0
#endif
#if GOGO_DEBUG
#include <stdio.h>
#include <stdlib.h>
#define GOGO_ASSERT(c) \
    ((c) ? (void)0 : (fprintf(stderr, "gogo: %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #c), abort()))
#else
#define GOGO_ASSERT(c) ((void)0)
#endif

#endif
