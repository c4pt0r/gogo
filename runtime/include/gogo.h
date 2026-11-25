/* gogo embedding API: run gogo images inside a host program, the way Lua is
 * embedded. Any number of VMs may live in one process; they share nothing
 * (one exception: the TLS hooks, below).
 *
 * A host
 *  1. fills a gogo_host table (or takes gogo_host_posix()) and a
 *     gogo_config, and makes a VM with gogo_new;
 *  2. gives it an image (`gogo build` output) with gogo_load and starts its
 *     main with gogo_start;
 *  3. either calls gogo_run (or gogo_run_thread), or steps the VM itself:
 *     gogo_step until GOGO_DONE, waiting between steps while it returns
 *     GOGO_IDLE until gogo_next_deadline or a wake;
 *  4. frees it with gogo_free.
 *
 * Threads. A VM is single-threaded: every gogo_* call runs on the thread
 * that owns the VM, except gogo_post, gogo_port_post, gogo_close_posts,
 * gogo_retain and gogo_release, which any thread holding a reference may
 * call at any time.
 * Hooks run on the owning thread unless noted.
 *
 * Re-entrancy. Hooks run inside gogo_step. A hook must not call gogo_step,
 * gogo_run, gogo_load or gogo_start (they return GOGO_EBUSY) nor gogo_free
 * (undefined); gogo_post is fine.
 *
 * Messages from the host. gogo_post(vm, "name", kind, data, len) sends the
 * actor registered under "name" (actor.Register) a port.Post{Kind, Data}
 * message. Posts are queued in a lock-free inbox and delivered in order per
 * posting thread at the VM's next step, as ordinary messages under the
 * receiver's mailbox quota. A post whose name is not registered when it is
 * delivered is dropped and counted (gogo_get_stats): in particular posts
 * sent before the program has registered the name (wait for a sign from
 * the program, such as an output line or a post of its own, first). A post
 * over the receiver's quota is dropped and counted too. While posts are
 * open and some actor holds a registered name, the VM keeps running (it
 * can still get posts); gogo_close_posts ends that, and the VM is then
 * done when its actors are, as on a board.
 *
 * TLS. The "tls" port's mbedTLS allocator, time and RNG hooks are
 * process-global (mbedTLS gives them no context): every VM in a process
 * must use the same board TLS table, and its hooks must be thread-safe. */
#ifndef GOGO_H
#define GOGO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gogo_host {
    /* Program output (stream 1, println) and diagnostics (stream 2, crash
     * reports and refusals). Required. */
    void     (*write)(void *ctx, int stream /*1 out, 2 err*/, const char *s, size_t n);
    /* Raw memory: n bytes (any alignment malloc gives), NULL = refuse (the
     * VM sees out of memory). free gets the same n back. Required. Also
     * called from gogo_post's thread (the inbox), so they must be
     * thread-safe when other threads post. */
    void    *(*alloc)(void *ctx, size_t n);
    void     (*free)(void *ctx, void *p, size_t n);
    /* Monotonic milliseconds, any epoch (the VM counts from its start).
     * Required. */
    uint64_t (*now_ms)(void *ctx);
    /* Wakes a wait in progress or latches one for the next wait. Called
     * from any thread (posts, I/O threads) and, on boards, from ISRs; it
     * must not call into the VM. Required with wait, and whenever the host
     * sleeps between steps in a wait of its own: event sources and posts
     * from other threads call it. NULL only if the host never sleeps (it
     * steps the VM in a busy loop) or if no event source, other thread or
     * post can make work. */
    void     (*wake)(void *ctx);
    /* Idle wait (gogo_run): returns after timeout_ms (UINT32_MAX = none) or
     * earlier once woken. Latched: a wake since the previous wait returned
     * makes it return at once. Spurious early returns are fine. NULL: the
     * host steps the VM itself between waits of its own (gogo_run refuses);
     * those waits must be latched the same way, with the latch consumed
     * before each step and never between an idle result and the wait. */
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
     * code itself). code_free gets the same n back. Both or neither. */
    void    *(*code_alloc)(void *ctx, size_t n);
    void     (*code_free)(void *ctx, void *p, size_t n);
} gogo_host;

/* Capability bits (gogo_config.caps; an image's requires word). A VM gets
 * the granted bits; an image that needs one it lacks is refused
 * (GOGO_ECAPS, the reason on stream 2). Basic I/O (print, actors, timers)
 * needs none. This version provides no capability yet: grant none. */
#define GOGO_CAP_NET    1u
#define GOGO_CAP_KBD    2u
#define GOGO_CAP_POWER  4u
#define GOGO_CAP_SD     8u
#define GOGO_CAP_IMU    16u
#define GOGO_CAP_IR_TX  32u
#define GOGO_CAP_IR_RX  64u
#define GOGO_CAP_MIC    128u
#define GOGO_CAP_SPK    256u
#define GOGO_CAP_TLS    512u

/* A NULL config is the all-zero one. */
typedef struct gogo_config {
    uint32_t caps;            /* granted GOGO_CAP_* bits; a bit this build cannot give makes gogo_new fail */
    size_t   mem_limit;       /* bytes the whole VM may allocate, over every actor's own limit; 0 = none.
                               * Queued posts are outside it (host memory, at most
                               * GOGO_POST_MAX of them) until delivered. */
    uint16_t max_actors;      /* live actors at once; 0 = the build's default (GOGO_TABLE_CAP) */
    const char *fs_root;      /* a host directory as /flash; not supported in this version: must be NULL */
    int64_t  fs_quota;        /* bytes under fs_root; -1 = none */
    int      allow_code_load; /* 0: code.Load returns NoMemory (bundled images only) */
} gogo_config;

typedef struct gogo_vm gogo_vm;

/* gogo_step results; every call returns a negative GOGO_E* on error. */
enum { GOGO_RUNNABLE = 0, GOGO_IDLE = 1, GOGO_DONE = 2 };
enum {
    GOGO_OK = 0,
    GOGO_EINVAL = -1,     /* a bad argument */
    GOGO_ENOMEM = -2,     /* out of memory (or over mem_limit) */
    GOGO_EIMAGE = -3,     /* not a valid image of this VM's format */
    GOGO_ECAPS = -4,      /* the image needs a capability the VM lacks */
    GOGO_ESTATE = -5,     /* out of order: no image loaded, loaded twice, not started */
    GOGO_ESTARTED = -6,   /* gogo_start again: a VM runs once */
    GOGO_ESTART = -7,     /* main could not start: no main export, out of memory */
    GOGO_EBUSY = -8,      /* called from a hook while the VM is stepping */
    GOGO_EFULL = -9,      /* the inbox holds GOGO_POST_MAX posts */
    GOGO_E2BIG = -10,     /* a post's name or data is too long */
    GOGO_ECLOSED = -11,   /* the VM is being freed, or was freed; a host port is closed */
    GOGO_EEXIST = -12     /* gogo_register_port: the name is taken */
};
#define GOGO_NEVER (-1)

/* Posts queued and not yet delivered, at most. */
#define GOGO_POST_MAX 256
/* Longest name and data of one post (the data goes in one message block).
 * In practice a post must also fit the receiver's memory limit beside its
 * heap and mailbox (256 KB in all by default), or it is dropped. */
#define GOGO_POST_NAME_MAX 255u
#define GOGO_POST_DATA_MAX 262080u

/* gogo_load flags: copy the image (else it is borrowed: it must stay valid
 * and unchanged until gogo_free, and be 4-byte aligned). */
#define GOGO_LOAD_COPY 1

/* A new VM on host h (whose hooks get host_ctx). h and host_ctx must stay
 * valid until the VM is freed and released. NULL if the table is
 * incomplete (write, alloc, free and now_ms are required; wake with wait;
 * the unix_time and code pairs both or neither), cfg is invalid (fs_root,
 * caps this build cannot give) or memory runs out. */
gogo_vm *gogo_new(const gogo_host *h, void *host_ctx, const gogo_config *cfg);

/* Loads the image (len bytes, little-endian words) as the VM's program.
 * Once, before gogo_start. GOGO_EIMAGE, GOGO_ECAPS (the reason is on
 * stream 2), GOGO_ENOMEM, GOGO_EINVAL (borrowed and unaligned),
 * GOGO_ESTATE (already loaded). After a failure the VM may load again. */
int gogo_load(gogo_vm *vm, const void *image, size_t len, int flags);

/* Starts the image's main. GOGO_ESTATE without an image, GOGO_ESTARTED if
 * started before, GOGO_ESTART if main cannot start (the reason is on
 * stream 2). */
int gogo_start(gogo_vm *vm);

/* Runs at most budget scheduler passes (each delivers posts and events,
 * fires due timers and runs one time slice of one actor; 0 = until idle or
 * done). GOGO_RUNNABLE: work remains, step again; GOGO_IDLE: nothing to do
 * until gogo_next_deadline or a wake (wait, latched); GOGO_DONE: the
 * program has finished (every actor exited or is blocked for good, and,
 * while posts are open, none holds a registered name); later steps return
 * GOGO_DONE too, and posts are closed from then on.
 * GOGO_ESTATE before gogo_start. */
int gogo_step(gogo_vm *vm, uint32_t budget);

/* When the VM has work next without a wake: an absolute host now_ms value
 * (it may already have passed: step at once), or GOGO_NEVER. */
int64_t gogo_next_deadline(gogo_vm *vm);

/* Any thread: queues port.Post{kind, data[0..len)} for the actor registered
 * as name (NUL-terminated) and wakes the host. The data is copied. 0, or
 * GOGO_EFULL, GOGO_E2BIG, GOGO_ENOMEM, GOGO_EINVAL, GOGO_ECLOSED (posts
 * closed, the program finished, or the VM is being freed). The caller must hold a reference: the
 * owner thread its own; any other thread that may post while or after the
 * owner frees the VM must hold one from gogo_retain. */
int gogo_post(gogo_vm *vm, const char *name, int32_t kind, const void *data, size_t len);

/* Any thread (holding a reference): later posts return GOGO_ECLOSED,
 * registered names no longer keep the VM alive, and the host is woken, so
 * a VM whose actors only wait for posts ends (gogo_run returns 0). Posts
 * already queued are still delivered. A post concurrent with the close
 * may return 0 and still be discarded (the VM finished first); it is
 * counted in posts_dropped. gogo_free implies it. */
void gogo_close_posts(gogo_vm *vm);

/* Any thread: keeps the handle valid for gogo_post (which then returns
 * GOGO_ECLOSED) after gogo_free, until the matching gogo_release. Call it
 * while holding a reference already. */
void gogo_retain(gogo_vm *vm);
void gogo_release(gogo_vm *vm);

/* Starts the VM if it is not yet started, then steps it, waiting in
 * host->wait while it is idle, until it is done. 0 when the program has
 * finished; GOGO_EINVAL without a wait hook; GOGO_ECLOSED if gogo_free
 * stopped it (gogo_run_thread); a gogo_start error. */
int gogo_run(gogo_vm *vm);

/* gogo_run on a new thread (POSIX threads): the VM then belongs to that
 * thread until gogo_join, which returns gogo_run's result. Meanwhile other
 * threads may only post, retain, release, or gogo_free (which stops the
 * thread at its next step, joins it and frees the VM). GOGO_EINVAL without
 * wait, GOGO_ESTATE if a thread is already running, GOGO_ENOMEM. */
int gogo_run_thread(gogo_vm *vm);
int gogo_join(gogo_vm *vm);

/* ---- host ports: the embedder's own extension surface ----
 *
 * A host defines ports of its own, which Go code opens by name like any
 * port: port.Open("ui", port.Host{Arg: 1, Data: cfg}) calls the driver's
 * open; actor.Send(p, port.Post{Kind, Data}) calls its command; the port's
 * events reach its owner as port.Post{Kind, Data} (gogo_port_emit,
 * gogo_port_post). Ports keep their usual rules: the owner's exit,
 * port.Close or actor.Kill closes one (the driver's close runs once),
 * port.Handoff gives it to another actor, a stale PID is ignored. Host
 * ports need no capability bit (they work with caps 0): what one may do is
 * the host's own business. While posts are open (gogo_close_posts), an
 * open host port keeps the VM alive, as it can still get events; after
 * the close it no longer does (a host that only emits between steps
 * closes posts when it is done with its ports). port.Post carries no
 * source: an actor that owns several host ports, or also gets gogo_post
 * messages, tells them apart by Kind.
 *
 * The callbacks run on the VM thread inside gogo_step (close also inside
 * gogo_free) and are hooks: gogo_step, gogo_run, gogo_load and gogo_start
 * return GOGO_EBUSY there. data is valid during the call only; open's is
 * the cfg's Data, command's the command's Data. A port is named by its
 * index and a 12-bit generation bumped at each close of its slot: a closed
 * port's value names no later port until that slot has been opened and
 * closed 4096 more times (as for port PIDs). */
typedef struct gogo_port { uint16_t index, gen; } gogo_port;
typedef struct gogo_port_driver {
    /* port.Open: 0 to accept, anything else refuses (Open's ok is false).
     * NULL accepts every open. Events it emits (gogo_port_emit returns 0)
     * are queued on the owner once the open succeeds, then subject to its
     * quota like any event (over it, dropped); if it refuses, they are
     * discarded. */
    int  (*open)(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len);
    /* A port.Post sent to the port (by any actor). NULL ignores them; so
     * is any other message. */
    void (*command)(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len);
    /* The port has closed (any reason, gogo_free too); gogo_port_emit to
     * it returns GOGO_ECLOSED already. NULL: nothing. */
    void (*close)(void *ctx, gogo_vm *vm, gogo_port port);
} gogo_port_driver;

/* Most host ports per VM, and the longest name. */
#define GOGO_MAX_HOST_PORTS 4
#define GOGO_PORT_NAME_MAX 31u

/* Registers a host port driver as name (NUL-terminated; both are copied),
 * its callbacks getting ctx. Before gogo_start. 0, or GOGO_EEXIST (a
 * port of this name exists, or it names a board driver: "gpio", "tcp",
 * ...), GOGO_EFULL (GOGO_MAX_HOST_PORTS already), GOGO_E2BIG (the name),
 * GOGO_EINVAL (no name or driver), GOGO_ESTARTED, GOGO_EBUSY. */
int gogo_register_port(gogo_vm *vm, const char *name, const gogo_port_driver *drv, void *ctx);

/* VM thread (a callback, or between steps): sends the port's owner
 * port.Post{kind, data[0..len)} (copied) now. 0, or GOGO_ECLOSED (the port
 * is closed or closing, or its owner is exiting), GOGO_EFULL (over the
 * owner's mailbox quota or Bytes limit: dropped, the owner is not
 * killed), GOGO_ENOMEM,
 * GOGO_E2BIG (over GOGO_POST_DATA_MAX), GOGO_EINVAL. */
int gogo_port_emit(gogo_vm *vm, gogo_port port, int32_t kind, const void *data, size_t len);

/* Any thread: gogo_port_emit through the inbox, as gogo_post: queued,
 * wakes the host, delivered at the VM's next step. Returns as gogo_post
 * does (GOGO_ECLOSED once posts are closed). A post the port can no longer
 * take when it is delivered (closed since, its owner exiting, over quota)
 * is dropped and counted in posts_dropped. */
int gogo_port_post(gogo_vm *vm, gogo_port port, int32_t kind, const void *data, size_t len);

/* Counters. On the owning thread: with gogo_run_thread, after gogo_join
 * (or from a hook). */
typedef struct gogo_stats {
    uint64_t posts_delivered;  /* posts queued on their receiver */
    uint64_t posts_dropped;    /* posts not delivered: unknown name, over quota, out of memory,
                                * or the VM finished first */
    size_t   mem_used;         /* the VM allocator's bytes in use */
    size_t   mem_peak;         /* its highest use */
} gogo_stats;
void gogo_get_stats(gogo_vm *vm, gogo_stats *out);

/* Closes posts (gogo_close_posts) and the program's ports and returns
 * every byte the VM took from its host (pending posts included); a VM running in gogo_run_thread is
 * stopped and joined first. Never from a hook. The handle stays valid for
 * gogo_post / gogo_release while retained. NULL is ignored. */
void gogo_free(gogo_vm *vm);

/* The POSIX default host: malloc, CLOCK_MONOTONIC, CLOCK_REALTIME as the
 * wall clock (setting it is refused), getentropy (random aborts if it
 * fails), stdout/stderr, and a latched condition-variable wait and wake.
 * Its state lives in a context: gogo_posix_ctx_new, or gogo_new_posix,
 * which makes a VM that owns one (freed with the VM's last reference). */
const gogo_host *gogo_host_posix(void);
void *gogo_posix_ctx_new(void);         /* NULL if out of memory */
void gogo_posix_ctx_free(void *ctx);
gogo_vm *gogo_new_posix(const gogo_config *cfg);

#ifdef __cplusplus
}
#endif

#endif
