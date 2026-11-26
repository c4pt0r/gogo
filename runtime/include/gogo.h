/* gogo embedding API: run gogo images inside a host program, the way Lua is
 * embedded. Any number of VMs may live in one process; they share nothing.
 *
 * A host
 *  1. fills a gogo_host table (starting from GOGO_HOST_INIT, or takes
 *     gogo_host_posix()) and a gogo_config (GOGO_CONFIG_INIT), and makes a
 *     VM with gogo_new;
 *  2. gives it an image (`gogo build` output) with gogo_load and starts its
 *     main with gogo_start;
 *  3. either calls gogo_run (or gogo_run_thread), or steps the VM itself:
 *     gogo_step until GOGO_DONE, waiting between steps while it returns
 *     GOGO_IDLE until gogo_next_deadline or a wake;
 *  4. frees it with gogo_free.
 *
 * ABI. The structs a host fills begin with their size: start each from
 * its GOGO_*_INIT, which sets it to the size this header gives. The
 * library reads only the fields it knows: fields that a newer header
 * appends are ignored by an older library, and fields that an older
 * header lacks take their defaults (0, NULL). Fields are only ever
 * appended, and a field that may be ignored is one whose 0 means what
 * the library did before it existed.
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
 * Hook results. A hook that can fail returns 0 for success and a negative
 * GOGO_E* for failure; a hook that returns a value (a length, a time)
 * documents its own.
 *
 * Messages from the host. gogo_post(vm, "name", kind, data, len) sends the
 * actor registered under "name" (actor.Register) a port.Post{From, Kind,
 * Data} message, From the zero PID. Posts are queued in a lock-free inbox and delivered in order per
 * posting thread at the VM's next step, as ordinary messages under the
 * receiver's mailbox quota. A post whose name is not registered when it is
 * delivered is dropped and counted (gogo_get_stats): in particular posts
 * sent before the program has registered the name (wait for a sign from
 * the program, such as an output line or a post of its own, first). A post
 * over the receiver's quota is dropped and counted too. While posts are
 * open and some actor holds a registered name, the VM keeps running (it
 * can still get posts); gogo_close_posts ends that, and the VM is then
 * done when its actors are, as on a board. */
#ifndef GOGO_H
#define GOGO_H

#include <stddef.h>
#include <stdint.h>
#include "gogo_errors.h"

/* Marks the API functions. libgogo.a is built with -fvisibility=hidden
 * and exports exactly these (its internal functions are local to the
 * library); a host may predefine GOGO_API, e.g. for a DLL import. */
#ifndef GOGO_API
#if defined(__GNUC__) || defined(__clang__)
#define GOGO_API __attribute__((visibility("default")))
#else
#define GOGO_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gogo_host {
    /* sizeof(gogo_host) as the host was compiled (GOGO_HOST_INIT); the
     * word after it is reserved, 0. */
    uint32_t size;
    uint32_t reserved;
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
     * offset clock, unknown until set. set returns 0, or a negative
     * GOGO_E* to refuse (sys.SetTime then returns false); both or
     * neither. */
    int64_t  (*unix_time_ms)(void *ctx);
    int      (*set_unix_time_ms)(void *ctx, int64_t ms);
    /* 32 random bits unpredictable to a network peer. NULL: the VM's own
     * clock-seeded mixer (not for secrets). */
    uint32_t (*random)(void *ctx);
    /* sys.Config: copies the value of the kl-byte key k into buf (at most
     * cap bytes; buf may be NULL when cap is 0) and returns the value's full
     * length, or -1 if unset. NULL: nothing is set. */
    int32_t  (*config_get)(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap);
    /* Special memory for hot-loaded module code (e.g. PSRAM): n bytes,
     * 4-byte aligned, only read as words; NULL = none (the VM keeps the
     * code itself). code_free gets the same n back. Both or neither. */
    void    *(*code_alloc)(void *ctx, size_t n);
    void     (*code_free)(void *ctx, void *p, size_t n);
    /* Called once, with the ctx, after the VM's last reference is gone
     * (gogo_free, and the posting references of gogo_retain), on the
     * thread that dropped it: the host frees its context here. No other
     * hook runs after it. NULL: nothing. Not called when gogo_new fails
     * (the ctx stays the host's). */
    void     (*destroy)(void *ctx);
    /* Fields are appended here, never inserted. */
} gogo_host;

/* A gogo_host with its size set and every hook NULL. C99 designated
 * initializers; in C++ before C++20, value-initialize and set .size. */
#define GOGO_HOST_INIT { .size = sizeof(gogo_host) }

/* A NULL config is GOGO_CONFIG_INIT: every name the VM has granted, no
 * memory cap, the default actor table, no storage, no hot loading. */
typedef struct gogo_config {
    uint32_t size;            /* sizeof(gogo_config) as the host was compiled (GOGO_CONFIG_INIT) */
    uint32_t flags;           /* GOGO_CONFIG_* bits; an unknown bit makes gogo_new fail */
    uint64_t mem_limit;       /* bytes the whole VM may allocate, over every actor's own limit; 0 = none.
                               * Queued posts are outside it (host memory, at most
                               * GOGO_POST_MAX of them) until delivered. */
    uint32_t max_actors;      /* live actors at once; 0 = the build's default (16384), at most 65520 */
    uint32_t reserved;        /* 0 */
    /* The requirement names the VM may serve (NULL-terminated; strings
     * and array must stay valid while the VM exists), or NULL: all it has.
     * A name is a port driver ("ui", a host port) or a mount prefix
     * ("/flash"). Whatever is not listed is absent for this VM: an image
     * requiring it is refused (GOGO_EREQUIRES), port.Open of it fails, and
     * /flash is not mounted. */
    const char *const *grant;
    /* An existing host directory served as /flash, or NULL: no storage, every gogo/fs call is
     * NotMounted. The program is confined to it: paths are walked from the
     * directory without following symlinks (a symlink, FIFO or device is
     * BadPath, and so is opening a file with other hard links for
     * writing). On a host file system that ignores case (macOS by
     * default) /flash does too, as /sd does on a board. /sd is never
     * mounted. The string is not kept.
     * The directory must not be writable by other users (they could link
     * their files in), and the roots of VMs running at the same time must
     * not nest (an outer program moving a directory out of an inner root
     * during the inner VM's walk would take that walk outside). */
    const char *fs_root;
    /* 0 = no quota (the host file system's own limits; fs.Usage reports
     * its usage). Else the bytes the regular files under fs_root may hold:
     * what is there is counted when the VM is made (a walk of the whole
     * tree), then the VM tracks how much each write grows a file and what
     * truncation, removal and replacement give back; a write past it
     * stores what fits and is NoSpace, and fs.Usage reports (used,
     * fs_quota). Changes made by others (the host, another VM on the same
     * directory) are not seen. Negative: gogo_new fails. */
    int64_t  fs_quota;
    /* Fields are appended here, never inserted. */
} gogo_config;

#define GOGO_CONFIG_INIT { .size = sizeof(gogo_config) }

/* gogo_config.flags: code.Load may load modules. Without it every
 * code.Load returns NoMemory (bundled images only). */
#define GOGO_CONFIG_CODE_LOAD 1u

typedef struct gogo_vm gogo_vm;

/* gogo_step results; every call returns a negative GOGO_E* on error. */
enum { GOGO_RUNNABLE = 0, GOGO_IDLE = 1, GOGO_DONE = 2 };

/* Errors. Each is a shared code of gogo_errors.h (the codes gogo programs
 * see, gogo/errors) negated, so one table describes both (gogo_strerror);
 * GOGO_ESTATE alone is the API's own, from 36..99, which are never
 * shared codes. */
enum {
    GOGO_OK = GOGO_E_OK,
    GOGO_ENOTFOUND = -GOGO_E_NOTFOUND,    /* gogo_start: the image has no main */
    GOGO_EEXIST = -GOGO_E_EXISTS,         /* gogo_register_port: the name is taken */
    GOGO_ENOMEM = -GOGO_E_NOMEMORY,       /* out of memory (or over mem_limit) */
    GOGO_EFULL = -GOGO_E_TOOMANY,         /* a fixed table is full: GOGO_POST_MAX posts queued, the host ports */
    GOGO_EBUSY = -GOGO_E_BUSY,            /* called from a hook while the VM is stepping */
    GOGO_EINVAL = -GOGO_E_BADARG,         /* a bad argument */
    GOGO_ECLOSED = -GOGO_E_CLOSED,        /* the VM is being freed, or was freed; posts are closed; a host port is closed */
    GOGO_EREQUIRES = -GOGO_E_UNSUPPORTED, /* the image requires a driver or mount the VM lacks (named on stream 2) */
    GOGO_EIMAGE = -GOGO_E_BADIMAGE,       /* not a valid image */
    GOGO_E2BIG = -GOGO_E_OVERFLOW,        /* a name or data is too long */
    GOGO_EVERSION = -GOGO_E_BADVERSION,   /* the image's format is another major, or a newer minor than this VM runs */
    GOGO_ESTATE = -36                     /* out of order: no image loaded, loaded twice, not started, started twice */
};

/* A short description of a result: a GOGO_E* (or a shared code from
 * gogo_errors.h, such as a port.Closed reason, as is). A static string,
 * "unknown error" for a code without one. */
GOGO_API const char *gogo_strerror(int rc);

/* The library's version, (major << 16) | (minor << 8) | patch: what
 * GOGO_VERSION was when it was built. A host compiled against this
 * header needs a library of the same major, during 0.x the same minor,
 * at least as new: gogo_version() >= GOGO_VERSION. */
#define GOGO_VERSION_MAJOR 0
#define GOGO_VERSION_MINOR 2
#define GOGO_VERSION_PATCH 0
#define GOGO_VERSION ((GOGO_VERSION_MAJOR << 16) | (GOGO_VERSION_MINOR << 8) | GOGO_VERSION_PATCH)
GOGO_API uint32_t gogo_version(void);

/* The image formats this library loads: (major << 16) | newest minor.
 * An image of format M.m (header word 1, M << 16 | m) loads if M is this
 * major and m is at most this minor; others are GOGO_EVERSION. */
GOGO_API uint32_t gogo_image_format(void);
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

/* Makes a VM on host h, whose hooks get host_ctx, and stores it in *out.
 * The table is copied (h need not outlive the call); host_ctx must stay
 * valid until the host's destroy hook runs, or, without one, until the
 * VM is freed and released. 0, or (with *out NULL):
 * GOGO_EINVAL: the table is incomplete (write, alloc, free and now_ms are
 * required; wake with wait; the unix_time and code pairs both or
 * neither), or cfg is invalid (a size or flag this library does not
 * know, max_actors too large, a negative fs_quota, an fs_root that is not
 * a directory); GOGO_ENOMEM. */
GOGO_API int gogo_new(const gogo_host *h, void *host_ctx, const gogo_config *cfg, gogo_vm **out);

/* Loads the image (len bytes, little-endian words) as the VM's program.
 * Once, before gogo_start. GOGO_EIMAGE, GOGO_EVERSION (both formats are
 * named on stream 2), GOGO_EREQUIRES (the image requires a port driver or
 * mount this VM lacks: a host port must be registered before the load;
 * the missing name is on stream 2), GOGO_ENOMEM, GOGO_EINVAL (borrowed and unaligned),
 * GOGO_ESTATE (already loaded). After a failure the VM may load again. */
GOGO_API int gogo_load(gogo_vm *vm, const void *image, size_t len, int flags);

/* ---- inspecting an image, without a VM ----
 *
 * Pure functions, any thread: they read the image (len bytes, 4-byte
 * aligned) and keep nothing. Each validates the whole image as gogo_load
 * does: GOGO_EINVAL (no image, unaligned, no out), GOGO_EIMAGE,
 * GOGO_EVERSION (another major or a newer minor; info->format is still
 * filled in). */
typedef struct gogo_image_info {
    uint32_t size;          /* set by the caller: sizeof(gogo_image_info) (GOGO_IMAGE_INFO_INIT) */
    uint32_t format;        /* the image's format, major << 16 | minor */
    uint32_t n_requires;    /* its requirements (gogo_image_require) */
    uint32_t n_exports;     /* its exported entry points (main among them) */
    /* Fields are appended here, never inserted: the library fills the
     * first size bytes it knows. */
} gogo_image_info;
#define GOGO_IMAGE_INFO_INIT { .size = sizeof(gogo_image_info) }
GOGO_API int gogo_image_info_get(const void *image, size_t len, gogo_image_info *info);

/* Requirement i (< n_requires, else GOGO_EINVAL) of the image: a port
 * driver's name ("ui", a host port's, "tcp") or a mount prefix ("/flash",
 * "/sd"), as gogo_load checks them. *name points into the image (valid
 * while it is; not NUL-terminated) and *name_len is its length. A host
 * may read them to register exactly the host ports an image needs. */
GOGO_API int gogo_image_require(const void *image, size_t len, uint32_t i, const char **name, size_t *name_len);

/* Starts the image's main. GOGO_ESTATE without an image or if started
 * before; GOGO_ENOTFOUND if the image has no main, GOGO_ENOMEM if main
 * cannot start for want of memory (both also on stream 2). */
GOGO_API int gogo_start(gogo_vm *vm);

/* Runs at most budget scheduler passes (each delivers posts and events,
 * fires due timers and runs one time slice of one actor; 0 = until idle or
 * done). GOGO_RUNNABLE: work remains, step again; GOGO_IDLE: nothing to do
 * until gogo_next_deadline or a wake (wait, latched); GOGO_DONE: the
 * program has finished (every actor exited or is blocked for good, and,
 * while posts are open, none holds a registered name); later steps return
 * GOGO_DONE too, and posts are closed from then on.
 * GOGO_ESTATE before gogo_start. */
GOGO_API int gogo_step(gogo_vm *vm, uint32_t budget);

/* When the VM has work next without a wake: an absolute host now_ms value
 * (it may already have passed: step at once), or GOGO_NEVER. */
GOGO_API int64_t gogo_next_deadline(gogo_vm *vm);

/* Any thread: queues port.Post{kind, data[0..len)} for the actor registered
 * as name (NUL-terminated) and wakes the host. The data is copied. 0, or
 * GOGO_EFULL, GOGO_E2BIG, GOGO_ENOMEM, GOGO_EINVAL, GOGO_ECLOSED (posts
 * closed, the program finished, or the VM is being freed). The caller must hold a reference: the
 * owner thread its own; any other thread that may post while or after the
 * owner frees the VM must hold one from gogo_retain. */
GOGO_API int gogo_post(gogo_vm *vm, const char *name, int32_t kind, const void *data, size_t len);

/* Any thread (holding a reference): later posts return GOGO_ECLOSED,
 * registered names no longer keep the VM alive, and the host is woken, so
 * a VM whose actors only wait for posts ends (gogo_run returns 0). Posts
 * already queued are still delivered. A post concurrent with the close
 * may return 0 and still be discarded (the VM finished first); it is
 * counted in posts_dropped. gogo_free implies it. */
GOGO_API void gogo_close_posts(gogo_vm *vm);

/* Any thread: keeps the handle valid for gogo_post (which then returns
 * GOGO_ECLOSED) after gogo_free, until the matching gogo_release. Call it
 * while holding a reference already. */
GOGO_API void gogo_retain(gogo_vm *vm);
GOGO_API void gogo_release(gogo_vm *vm);

/* Starts the VM if it is not yet started, then steps it, waiting in
 * host->wait while it is idle, until it is done. 0 when the program has
 * finished; GOGO_EINVAL without a wait hook; GOGO_ECLOSED if gogo_free
 * stopped it (gogo_run_thread); a gogo_start error. */
GOGO_API int gogo_run(gogo_vm *vm);

/* gogo_run on a new thread (POSIX threads): the VM then belongs to that
 * thread until gogo_join, which returns gogo_run's result. Meanwhile other
 * threads may only post, retain, release, or gogo_free (which stops the
 * thread at its next step, joins it and frees the VM). GOGO_EINVAL without
 * wait, GOGO_ESTATE if a thread is already running, GOGO_ENOMEM. */
GOGO_API int gogo_run_thread(gogo_vm *vm);
GOGO_API int gogo_join(gogo_vm *vm);

/* ---- host ports: the embedder's own extension surface ----
 *
 * A host defines ports of its own, which Go code opens by name like any
 * port: port.Open("ui", port.Host{Arg: 1, Data: cfg}) calls the driver's
 * open; actor.Send(p, port.Post{Kind: k, Data: d}) calls its command; the
 * port's events reach its owner as port.Post{From, Kind, Data}, From the
 * port's PID (gogo_port_emit,
 * gogo_port_post). Ports keep their usual rules: the owner's exit,
 * port.Close or actor.Kill closes one (the driver's close runs once),
 * port.Handoff gives it to another actor, a stale PID is ignored. What a
 * host port may do is the host's own business. An image that opens a host
 * port by a constant name requires it: register the port before
 * gogo_load, or the load is refused (GOGO_EREQUIRES). While posts are open (gogo_close_posts), an
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
 * its callbacks getting ctx. Before gogo_load (a registered port
 * satisfies an image's requirement of its name). 0, or GOGO_EEXIST (a
 * port of this name exists, or it names one of the runtime's drivers,
 * "gpio", "tcp", "screen", ..., or one of the board's), GOGO_EFULL (GOGO_MAX_HOST_PORTS already), GOGO_E2BIG (the name),
 * GOGO_EINVAL (no name or driver), GOGO_ESTATE (an image is loaded),
 * GOGO_EBUSY. A port registered under a name the config's grant leaves
 * out is accepted but never visible to the program. */
GOGO_API int gogo_register_port(gogo_vm *vm, const char *name, const gogo_port_driver *drv, void *ctx);

/* VM thread (a callback, or between steps): sends the port's owner
 * port.Post{kind, data[0..len)} (copied) now. 0, or GOGO_ECLOSED (the port
 * is closed or closing, or its owner is exiting), GOGO_EFULL (over the
 * owner's mailbox quota or Bytes limit: dropped, the owner is not
 * killed), GOGO_ENOMEM,
 * GOGO_E2BIG (over GOGO_POST_DATA_MAX), GOGO_EINVAL. */
GOGO_API int gogo_port_emit(gogo_vm *vm, gogo_port port, int32_t kind, const void *data, size_t len);

/* Any thread: gogo_port_emit through the inbox, as gogo_post: queued,
 * wakes the host, delivered at the VM's next step. Returns as gogo_post
 * does (GOGO_ECLOSED once posts are closed). A post the port can no longer
 * take when it is delivered (closed since, its owner exiting, over quota)
 * is dropped and counted in posts_dropped. */
GOGO_API int gogo_port_post(gogo_vm *vm, gogo_port port, int32_t kind, const void *data, size_t len);

/* Counters. On the owning thread: with gogo_run_thread, after gogo_join
 * (or from a hook). */
typedef struct gogo_stats {
    uint64_t posts_delivered;  /* posts queued on their receiver */
    uint64_t posts_dropped;    /* posts not delivered: unknown name, over quota, out of memory,
                                * or the VM finished first */
    size_t   mem_used;         /* the VM allocator's bytes in use */
    size_t   mem_peak;         /* its highest use */
} gogo_stats;
GOGO_API void gogo_get_stats(gogo_vm *vm, gogo_stats *out);

/* Closes posts (gogo_close_posts) and the program's ports and returns
 * every byte the VM took from its host (pending posts included); a VM running in gogo_run_thread is
 * stopped and joined first. Never from a hook. The handle stays valid for
 * gogo_post / gogo_release while retained. NULL is ignored. */
GOGO_API void gogo_free(gogo_vm *vm);

/* The POSIX default host: malloc, CLOCK_MONOTONIC, CLOCK_REALTIME as the
 * wall clock (setting it is refused), getentropy (random aborts if it
 * fails), stdout/stderr, and a latched condition-variable wait and wake.
 * Its state lives in a context: gogo_posix_ctx_new, or gogo_new_posix,
 * which makes a VM that owns one (freed with the VM's last reference, as
 * a destroy hook would). */
GOGO_API const gogo_host *gogo_host_posix(void);
GOGO_API void *gogo_posix_ctx_new(void);         /* NULL if out of memory */
GOGO_API void gogo_posix_ctx_free(void *ctx);
GOGO_API int gogo_new_posix(const gogo_config *cfg, gogo_vm **out);   /* as gogo_new */

#ifdef __cplusplus
}
#endif

#endif
