/* Platform interface: the only I/O the VM core needs. Implemented per
 * target (e.g. platform/unix/). */
#ifndef GOGO_PLATFORM_H
#define GOGO_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

void platform_write(const char *s, size_t n);      /* program output (stdout) */
void platform_write_err(const char *s, size_t n);  /* diagnostics (stderr) */

/* Raw memory for the allocator (unix: malloc/free). bytes is always the
 * size originally requested. Returns NULL on failure. */
void *platform_chunk_alloc(size_t bytes);
void platform_chunk_free(void *p, size_t bytes);

/* Monotonic milliseconds since the last platform_time_reset (vm_run calls it
 * at start, so this is time since VM start on every platform). It returns
 * the wall clock the VM starts with (Unix ms, 0 = unknown; vm_run hands it
 * to vm_set_unix_time_ms, so the floor applies). */
uint64_t platform_now_ms(void);
int64_t platform_time_reset(void);

/* Wall clock (Unix time in milliseconds, UTC). Separate from the monotonic
 * platform_now_ms: it can be unknown, and it can be set (sys.SetTime, set by
 * the gogo/sntp library; TLS certificate validity reads it).
 *
 * The wall clock is TRUSTED IMAGE STATE: gogo has no privilege model (every
 * image and module already has the file system and the network), so any code
 * may set it, and SNTP is unauthenticated. The one protection is a floor:
 * the time of the firmware / VM build (GOGO_BUILD_UNIX_MS, given by the
 * build systems; the fallback below is only for builds that do not give it).
 * A time earlier than the floor is refused, so a clock set backwards can
 * never make a certificate that expired before the build valid again.
 *
 * vm_unix_time_ms(vm) returns the current Unix time in ms, or 0 when the
 * time is unknown. It never returns a negative number; a time that is known
 * is always > 0. vm_set_unix_time_ms(vm, ms) makes ms the current time from
 * now on (it keeps advancing with platform_now_ms) and returns 1;
 * ms < GOGO_BUILD_UNIX_MS (other than ms <= 0) changes nothing and returns
 * 0; ms <= 0 forgets the time (unknown) and returns 1. Scheduler context
 * only, never an ISR.
 *
 * vm/walltime.c keeps the clock per Vm as an offset against
 * platform_now_ms, unknown after vm_init. vm_run sets it from what
 * platform_time_reset returns: unix gives the host clock, or
 * $GOGO_UNIX_TIME_MS (a number of ms, 0 = unknown; goldens use it; subject
 * to the floor), and unknown with --sim-clock; the ESP gives 0 (unknown). */
#ifndef GOGO_BUILD_UNIX_MS
#define GOGO_BUILD_UNIX_MS 1764000000000LL   /* 2025-11-24 */
#endif
struct Vm;
int64_t vm_unix_time_ms(const struct Vm *vm);
int vm_set_unix_time_ms(struct Vm *vm, int64_t ms);

/* Randomness (sys.Random, gogo/sntp's nonce): 32 random bits, unpredictable
 * to a network peer. Every platform defines it from the hardware (unix:
 * getentropy, ESP: esp_random); the VM reads it through vm_random(vm). */
uint32_t platform_random(void);
#define vm_random(vm) ((void)(vm), platform_random())

/* Idle wait. Called by the scheduler when nothing is runnable: returns after
 * timeout_ms, or earlier when an event may have been pushed to the event
 * ring (spurious early returns are fine; the scheduler re-checks).
 * UINT32_MAX means no timeout.
 *
 * Latched wakeup (required): any platform_notify() since the previous
 * platform_wait returned makes the next platform_wait return immediately,
 * and a notify during a wait ends it. The scheduler drains the event ring
 * and then waits; an event pushed in that gap must not be slept through. */
void platform_wait(uint32_t timeout_ms);

/* Called by event producers after evq_push. ISR-safe on MCUs. Latches a
 * pending wakeup for platform_wait. */
void platform_notify(void);

/* Memory barrier ordering the event ring's slot and index accesses between
 * interrupt and scheduler context. */
void platform_barrier(void);

/* Configuration store (config.Get): copies the value of the
 * klen-byte key into buf, at most cap bytes (buf may be NULL when cap is
 * 0), and returns the value's full length, or -1 if the key is not set.
 * The VM never asks for "wifi." keys. */
int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap);

/* Storage (gogo/fs): B_FS calls the Vm's fs_builtin hook (actor.h) with
 * BUILTIN's ra, rb, rc and the frame's register count (ra and rb..rb+rc-1
 * are checked; a two-result op checks ra+1 itself). It returns R_NORMAL or
 * the crash reason and may GC. NULL (no storage linked): B_FS is badcode.
 * Set by vfs_install(vm) (vfs/builtin.c). */

/* The port drivers this platform provides (port.h), NULL-terminated. */
struct PortDriver;
const struct PortDriver *const *platform_port_drivers(void);

/* Special memory for hot-loaded module code (gogo/code, module.h), such
 * as PSRAM. platform_code_alloc returns n bytes, 4-byte aligned, that the
 * VM may read as words, or NULL for "no special memory": the module then
 * runs from its Bytes in place (off-heap and aligned) or from a copy in
 * the VM's own memory. Code is only read (never executed as machine code)
 * and stays put until the module unloads, when platform_code_free gets the
 * pointer and the same n back. A NULL return is never an error. Called
 * only from the scheduler, never from an ISR. vm/module.c has weak
 * defaults (NULL / nothing), so a platform without such memory defines
 * neither. */
void *platform_code_alloc(size_t n);
void platform_code_free(void *p, size_t n);

/* Firmware capabilities: the optional features this build can really serve,
 * checked against an image's `requires` word (image.h v9, compiler Cap*) at
 * load. Basic I/O (print, actors, gpio, timers) needs none.
 *   CAP_NET    the net stack and a link (gogo/net, dhcp, dns, http, udp ports)
 *   CAP_KBD    the "kbd" port driver
 *   CAP_POWER  the "power" port driver
 *   CAP_SD     a mounted-on-demand /sd file system
 *   CAP_IMU    the "imu" port driver
 *   CAP_IR_TX  the "ir_tx" port driver
 *   CAP_IR_RX  the "ir_rx" port driver
 *   CAP_MIC    the "mic" port driver
 *   CAP_SPK    the "spk" port driver
 *   CAP_TLS    the "tls" port driver
 *   CAP_ANYPORT is only a build-time warning marker (the image opens a port
 *   driver whose name is not known at build time); it never counts as missing.
 * A platform returns exactly the bits its build provides, never more. */
#define CAP_NET     1u
#define CAP_KBD     2u
#define CAP_POWER   4u
#define CAP_SD      8u
#define CAP_IMU     16u
#define CAP_IR_TX   32u
#define CAP_IR_RX   64u
#define CAP_MIC     128u
#define CAP_SPK     256u
#define CAP_TLS     512u
#define CAP_ANYPORT (1u << 31)

/* The CAP_* bits this firmware provides. vm/caps.c has a weak default that
 * returns 0 (basic I/O only), so a new platform links without defining it. */
uint32_t platform_caps(void);

/* 1 if every bit of requires (CAP_ANYPORT aside) is in caps; else 0.
 * *missing (may be NULL) gets the bits that are not. Pure. */
int vm_caps_ok(uint32_t requires, uint32_t caps, uint32_t *missing);

/* Writes the refusal line "gogo: image needs <names>; firmware lacks
 * <names>\n" (names: net kbd power sd imu irtx irrx mic spk tls) into buf, which must hold
 * VM_CAPS_MSG_MAX bytes, and returns its length. No NUL, no I/O: the
 * caller sends it (one platform_write_err call) and decides whether to exit
 * (boot) or only refuse the module (hot load). */
#define VM_CAPS_MSG_MAX 144
size_t vm_caps_explain(char *buf, uint32_t requires, uint32_t missing);

#endif
