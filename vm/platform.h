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
 * at start, so this is time since VM start on every platform). */
uint64_t platform_now_ms(void);
void platform_time_reset(void);

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

/* Configuration store (config.Get, spec M5 §5.4): copies the value of the
 * klen-byte key into buf, at most cap bytes (buf may be NULL when cap is
 * 0), and returns the value's full length, or -1 if the key is not set.
 * The VM never asks for "wifi." keys. */
int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap);

/* Storage (gogo/fs): B_FS calls this hook with BUILTIN's ra, rb, rc and
 * the frame's register count (ra and rb..rb+rc-1 are checked; a
 * two-result op checks ra+1 itself). It returns R_NORMAL or the crash
 * reason and may GC. NULL (no storage linked): B_FS is badcode. Set by
 * vfs_install (vfs/builtin.c). */
struct Vm;
struct Actor;
extern uint32_t (*vm_fs_builtin)(struct Vm *vm, struct Actor *a, uint32_t ra, uint32_t rb,
                                 uint32_t rc, uint32_t nregs);

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
#define CAP_ANYPORT (1u << 31)

/* The CAP_* bits this firmware provides. vm/caps.c has a weak default that
 * returns 0 (basic I/O only), so a new platform links without defining it. */
uint32_t platform_caps(void);

/* 1 if every bit of requires (CAP_ANYPORT aside) is in caps; else 0.
 * *missing (may be NULL) gets the bits that are not. Pure. */
int vm_caps_ok(uint32_t requires, uint32_t caps, uint32_t *missing);

/* Writes the refusal line "gogo: image needs <names>; firmware lacks
 * <names>\n" (names: net kbd power sd imu irtx irrx mic spk) into buf, which must hold
 * VM_CAPS_MSG_MAX bytes, and returns its length. No NUL, no I/O: the
 * caller sends it (one platform_write_err call) and decides whether to exit
 * (boot) or only refuse the module (hot load). */
#define VM_CAPS_MSG_MAX 128
size_t vm_caps_explain(char *buf, uint32_t requires, uint32_t missing);

#endif
