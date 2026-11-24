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

#endif
