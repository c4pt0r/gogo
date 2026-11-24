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
#ifndef GOGO_EVQ_CAP
#define GOGO_EVQ_CAP 64u
#endif

/* Cap on bytes the unix platform may take from the system for VM memory
 * (simulates a small MCU); 0 means no cap. */
#ifndef GOGO_ARENA_BYTES
#define GOGO_ARENA_BYTES 0u
#endif

#endif
