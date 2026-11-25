/* Platform-tunable compile-time constants. Override with -D. */
#ifndef GOGO_CONFIG_H
#define GOGO_CONFIG_H

/* Per-actor memory limit in words (heap block + queued messages) for an
 * actor spawned without an explicit limit. At most 65535 (max_words is
 * 16-bit). The RP2040 build uses 4096 (16 KB). */
#ifndef GOGO_DEFAULT_MAX_WORDS
#define GOGO_DEFAULT_MAX_WORDS 65535u
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
#define GOGO_MAX_PORTS 16u
#endif
/* Extra event sources (net, input, ...) the port layer folds in. */
#ifndef GOGO_MAX_SOURCES
#define GOGO_MAX_SOURCES 2u
#endif
#ifndef GOGO_EVQ_CAP
#define GOGO_EVQ_CAP 64u
#endif

/* Cap on bytes the unix platform may take from the system for VM memory
 * (simulates a small MCU); 0 means no cap. */
#ifndef GOGO_ARENA_BYTES
#define GOGO_ARENA_BYTES 0u
#endif

/* Off-heap bytes.Bytes (bytes.h): handle table entries for the whole VM
 * (1..65535), the handles one actor may hold at once (at least 1), and the
 * largest Buffer.Freeze result kept on the heap (larger ones go
 * off-heap). */
#ifndef GOGO_BYTES_HANDLES
#define GOGO_BYTES_HANDLES 64u
#endif
#ifndef GOGO_BYTES_PER_ACTOR
#define GOGO_BYTES_PER_ACTOR 16u
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
 * image aside) when the config key code.mem is not set (module.h). */
#ifndef GOGO_CODE_MEM_DEFAULT
#define GOGO_CODE_MEM_DEFAULT 65536u
#endif
#if GOGO_MAX_MODULES < 1 || GOGO_MAX_MODULES > 8
#error "GOGO_MAX_MODULES must be 1..8"
#endif

#endif
