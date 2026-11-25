/* The VM's way out (embed mode): the public host table (gogo_host,
 * include/gogo.h) for what every host gives, and the internal board table
 * (VmBoard) for board-only hardware: port drivers, input chips, netif. A Vm
 * holds both (vm_init) and reaches its host only through the wrappers
 * below, which apply the defaults of the optional hooks (host.c). Nothing
 * in runtime/ calls a platform function by name, and nothing is weak. */
#ifndef GOGO_HOST_H
#define GOGO_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../include/gogo.h"

struct Vm;
struct PortDriver;
struct InputHw;
struct NetIf;
struct TlsHw;

/* Firmware capabilities: the optional features a board can really serve,
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
 * A board lists exactly the bits its build provides, never more. */
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

/* Board-only hardware, a static const table per board. Any field may be
 * NULL / 0 (no such hardware). ports: the port drivers, NULL-terminated;
 * input: the input chips (input.h); netif: the network interface (netif.h);
 * tls: the "tls" port's hooks (TlsHw, net/tls/tls.h); caps: the CAP_* bits the board
 * provides (a Vm gets caps & its granted caps). */
typedef struct VmBoard {
    const struct PortDriver *const *ports;
    const struct InputHw *input;
    const struct NetIf *netif;
    const struct TlsHw *tls;
    uint32_t caps;
} VmBoard;

/* 1 if every bit of requires (CAP_ANYPORT aside) is in caps; else 0.
 * *missing (may be NULL) gets the bits that are not. Pure. */
int vm_caps_ok(uint32_t requires, uint32_t caps, uint32_t *missing);

/* Writes the refusal line "gogo: image needs <names>; firmware lacks
 * <names>\n" (names: net kbd power sd imu irtx irrx mic spk tls) into buf, which must hold
 * VM_CAPS_MSG_MAX bytes, and returns its length. No NUL, no I/O: the
 * caller sends it (one vm_write_err) and decides whether to exit (boot) or
 * only refuse the module (hot load). */
#define VM_CAPS_MSG_MAX 144
size_t vm_caps_explain(char *buf, uint32_t requires, uint32_t missing);

/* ---- wrappers (host.c): the only callers of the host's hooks ---- */

/* Milliseconds since the VM's time base (vm->t0: vm_run's start). */
uint64_t vm_now(const struct Vm *vm);
/* host->write: stream 1 program output, 2 diagnostics. */
void vm_out(const struct Vm *vm, int stream, const char *s, size_t n);
#define vm_write(vm, s, n) vm_out((vm), 1, (s), (n))
#define vm_write_err(vm, s, n) vm_out((vm), 2, (s), (n))
/* host->alloc / free: the allocator's raw memory. */
void *vm_chunk_alloc(struct Vm *vm, size_t n);
void vm_chunk_free(struct Vm *vm, void *p, size_t n);
/* host->config_get, -1 (unset) without one. */
int32_t vm_config_get(const struct Vm *vm, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap);
/* host->code_alloc / code_free; NULL / nothing without them. */
void *vm_code_alloc(struct Vm *vm, size_t n);
void vm_code_free(struct Vm *vm, void *p, size_t n);
/* host->wake (any thread or ISR) and host->wait (the run loop). */
void vm_wake(const struct Vm *vm);
void vm_wait(const struct Vm *vm, uint32_t timeout_ms);
/* host->random, else a clock-seeded xorshift kept in the Vm. */
uint32_t vm_random(struct Vm *vm);

/* Orders the event ring's slot and index accesses between interrupt (or
 * another thread) and the scheduler. A compiler builtin, not a hook. */
#define vm_barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Wall clock (Unix time in milliseconds, UTC). Separate from the monotonic
 * vm_now: it can be unknown, and it can be set (sys.SetTime, set by the
 * gogo/sntp library; TLS certificate validity reads it).
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
 * now on and returns 1; ms < GOGO_BUILD_UNIX_MS (other than ms <= 0) changes
 * nothing and returns 0; ms <= 0 forgets the time (unknown) and returns 1.
 * Scheduler context only, never an ISR.
 *
 * With host->unix_time_ms (an RTC) both go to the host (the floor still
 * applies to a set). Without, the Vm keeps an offset against the host's
 * monotonic clock, unknown after vm_init; a board that knows the time at
 * start sets it after vm_init (unix: the host clock or $GOGO_UNIX_TIME_MS,
 * unknown under --sim-clock; ESP: unknown until SNTP). */
#ifndef GOGO_BUILD_UNIX_MS
#define GOGO_BUILD_UNIX_MS 1764000000000LL   /* 2025-11-24 */
#endif
int64_t vm_unix_time_ms(const struct Vm *vm);
int vm_set_unix_time_ms(struct Vm *vm, int64_t ms);

#endif
