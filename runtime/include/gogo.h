/* gogo embedding API (embed mode). For now only the host callback table:
 * everything the VM needs from the program it runs in. The rest of the
 * public API (gogo_new, gogo_step, gogo_post, ...) comes with libgogo.
 *
 * Every hook gets the host_ctx given with the table. Unless noted, hooks
 * are called on the thread that owns the VM. Optional hooks may be NULL.
 *
 * Each VM keeps all of its state in its own instance: VMs in one process
 * share nothing, with one exception. The "tls" port's mbedTLS allocator,
 * time and RNG hooks are process-global (mbedTLS gives them no context):
 * every VM in a process must use the same board TLS table (TlsHw,
 * runtime/net/tls/tls.h); a VM with another one has its tls opens refused.
 * VMs on different threads may each run TLS: certificate checks read the
 * wall clock of the VM the calling thread is running, and that shared TlsHw
 * is then called from several threads at once, so its hooks must be
 * thread-safe. */
#ifndef GOGO_H
#define GOGO_H

#include <stddef.h>
#include <stdint.h>

typedef struct gogo_host {
    /* Program output (stream 1, println) and diagnostics (stream 2, crash
     * reports and refusals). Required. */
    void     (*write)(void *ctx, int stream /*1 out, 2 err*/, const char *s, size_t n);
    /* Raw memory for the VM's allocator: n bytes (any alignment malloc
     * gives), NULL = refuse (the VM sees out of memory). free gets the same
     * n back. Required. */
    void    *(*alloc)(void *ctx, size_t n);
    void     (*free)(void *ctx, void *p, size_t n);
    /* Monotonic milliseconds, any epoch (the VM counts from its start).
     * Required. */
    uint64_t (*now_ms)(void *ctx);
    /* Wakes a wait in progress or latches one for the next wait. Any
     * thread or ISR; never locks or allocates. Required whenever the host
     * sleeps between steps, through wait or a wait of its own: event
     * sources (ports, net RX) and posts from other threads call it. NULL
     * only if the host never sleeps (it steps the VM in a busy loop) or if
     * no event source, other thread or post can make work. */
    void     (*wake)(void *ctx);
    /* Idle wait (the VM's own run loop only): returns after timeout_ms
     * (UINT32_MAX = none) or earlier once woken. Latched: a wake since the
     * previous wait returned makes it return at once. Spurious early
     * returns are fine. NULL: the host steps the VM itself between waits
     * of its own (the VM's run loop refuses to start); those waits must be
     * latched the same way, with the latch consumed before each step and
     * never between an idle result and the wait. */
    void     (*wait)(void *ctx, uint32_t timeout_ms);
    /* Wall clock (an RTC), Unix ms, 0 = unknown. NULL: the VM keeps its own
     * offset clock, unknown until set. set returns 1, or 0 to refuse;
     * both or neither. */
    int64_t  (*unix_time_ms)(void *ctx);
    int      (*set_unix_time_ms)(void *ctx, int64_t ms);
    /* 32 random bits unpredictable to a network peer. NULL: the VM's own
     * clock-seeded mixer (not for secrets). */
    uint32_t (*random)(void *ctx);
    /* Cryptographic random bytes (TLS); 0 ok. Optional. */
    int      (*random_bytes)(void *ctx, uint8_t *buf, size_t n);
    /* config.Get: copies the value of the kl-byte key k into buf (at most
     * cap bytes; buf may be NULL when cap is 0) and returns the value's full
     * length, or -1 if unset. NULL: nothing is set. */
    int32_t  (*config_get)(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap);
    /* Special memory for hot-loaded module code (e.g. PSRAM): n bytes,
     * 4-byte aligned, only read as words; NULL = none (the VM keeps the
     * code itself). code_free gets the same n back. Optional. */
    void    *(*code_alloc)(void *ctx, size_t n);
    void     (*code_free)(void *ctx, void *p, size_t n);
} gogo_host;

#endif
