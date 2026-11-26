/* The unix board (platform_unix.c): its gogo_host table, netif and base
 * port drivers, and the board functions the other unix files (and the C
 * tests) use directly. Shared by gogovm (main.c) and the C test builds. */
#ifndef GOGO_PLATFORM_UNIX_H
#define GOGO_PLATFORM_UNIX_H

#include <stddef.h>
#include <stdint.h>
#include "host.h"
#include "netif.h"
#include "port.h"

/* The unix host: stdout / stderr, malloc under GOGO_ARENA_BYTES, the
 * monotonic (or --sim-clock) clock, a self-pipe wake latch, getentropy,
 * the config file. The ctx is unused (NULL). */
extern const gogo_host unix_host;
/* The --net-fd link (or a test's tx hook). */
extern const NetIf unix_netif;
/* gpio and the net drivers, NULL-terminated (gogovm adds the input ones). */
extern const PortDriver *const unix_base_ports[];

/* The board functions behind unix_host, for board code and tests. */
void platform_write(const char *s, size_t n);
void platform_write_err(const char *s, size_t n);
extern void (*platform_unix_err_hook)(const char *, size_t);  /* test-only stderr capture */
uint64_t platform_now_ms(void);          /* ms since first use, or the simulated clock */
uint32_t platform_random(void);
void platform_wait(uint32_t ms);         /* latched: see platform_notify */
void platform_notify(void);              /* any thread, async-signal-safe */
/* The program's arguments, as sys.Args returns them (argv[0..argc) are the
 * strings after the image on the command line; they must outlive the VM). */
void platform_set_args(int argc, char *const *argv);
int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap);

/* The wall clock a run starts with (Unix ms, 0 = unknown): the host clock,
 * or $GOGO_UNIX_TIME_MS (goldens), unknown under --sim-clock. gogovm sets
 * it with vm_set_unix_time_ms after vm_init (the floor applies). */
int64_t platform_unix_wall_start(void);

/* Extra fds platform_wait polls besides the wake pipe and the tty (the
 * --host-net sockets, hostnet.c): fills up to max pollfds (events set) and
 * returns how many; with p NULL, returns how many there are. NULL hook =
 * none. Its readiness only ends the wait: the owner's event source handles
 * it. */
struct pollfd;
extern int (*platform_unix_extra_fds)(struct pollfd *p, int max);

#endif
