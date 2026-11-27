/* gogo embedding API: run gogo images inside a host program, the way Lua is
 * embedded. Any number of VMs may live in one process; they share nothing.
 * This header is C99 and needs no threads; gogo_posix.h adds the POSIX
 * host and a VM on a thread of its own.
 *
 * A host
 *  1. fills a gogo_host table (starting from GOGO_HOST_INIT, or takes
 *     gogo_posix.h's gogo_host_posix()) and a gogo_config
 *     (GOGO_CONFIG_INIT), and makes a VM with gogo_new;
 *  2. gives it an image (`gogo build` output) with gogo_load and starts its
 *     main with gogo_start;
 *  3. either calls gogo_run (or gogo_posix.h's gogo_run_thread), or steps
 *     the VM itself:
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
 * the library did before it existed. A size below the struct's v0.2 size
 * (the first sized release), and a reserved word that is not 0, make
 * the call fail with GOGO_EINVAL; so does an output struct (gogo_stats,
 * gogo_image_info) whose size is below its v0.2 size.
 *
 * Stability. Within a major version (during 0.x, a minor) functions keep
 * their signatures and meaning, and structs only grow at the end. What is
 * marked GOGO_EXPERIMENTAL is outside that promise and may change in any
 * minor release: host ports (with board ports, the driver API: it joins
 * the promise in v0.3, when its functions keep their meaning and its
 * size-first structs only grow, so a driver built against one header
 * runs on later releases of the same major), the fs_root, fs_quota and grant settings,
 * the GOGO_CONFIG_CODE_LOAD flag, gogo_get_stats, image inspection, and
 * gogo_posix.h's threads. GOGO_EXPERIMENTAL expands to nothing unless the host defines
 * it before including this header, for example as
 * __attribute__((deprecated("experimental"))), to have the compiler point
 * out every use.
 *
 * Threads. A VM is single-threaded: every call that takes a gogo_vm runs
 * on the thread that owns the VM. Ownership may pass from thread to
 * thread: the library keeps no thread identity, so what it needs is that
 * those calls never overlap and that each happens before the next (a
 * hand-off through a mutex or a queue, as a Go goroutine moving between
 * OS threads gets). Other threads reach it only through a
 * gogo_poster (gogo_poster_new), whose calls any thread may make at any
 * time, also while or after the owner frees the VM: the type says which
 * calls are which. Hooks run on the owning thread unless noted.
 *
 * Re-entrancy. Hooks run inside gogo_step. A hook must not call gogo_step,
 * gogo_run, gogo_load or gogo_start (they return GOGO_EBUSY) nor gogo_free
 * (undefined); gogo_post is fine.
 *
 * Names. Every name the API takes is a pointer and a length (it need not
 * be NUL-terminated), except the config's paths and grant list. A name
 * the API gives back is the same pair.
 *
 * Hook results. A hook that can fail returns 0 for success and a negative
 * GOGO_E* for failure; a hook that returns a value (a length, a time)
 * documents its own.
 *
 * Messages from the host. gogo_post(vm, "name", 4, kind, data, len) (or
 * gogo_poster_post from another thread) sends the actor registered under
 * "name" (actor.Register) a port.Post{From, Kind, Data} message, From the
 * zero PID. Posts are queued in a lock-free inbox and delivered in order per
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

#ifndef GOGO_EXPERIMENTAL
#define GOGO_EXPERIMENTAL
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
     * called from posters' threads (the inbox, and the poster's own
     * block), so they must be thread-safe when other threads post. */
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
     * (gogo_free, and every gogo_poster_free), on the thread that dropped
     * it: the host frees its context here. No other
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
    uint32_t max_actors;      /* live actors at once; 0 = the build's default (16384), at most 65536 - max_ports
                               * (65520 with 16 ports) */
    uint32_t reserved;        /* 0 */
    /* The requirement names the VM may serve (NULL-terminated; strings
     * and array must stay valid while the VM exists), or NULL: all it has.
     * A name is a port driver ("ui", a host port) or a mount prefix
     * ("/flash"). Whatever is not listed is absent for this VM: an image
     * requiring it is refused (GOGO_EREQUIRES), port.Open of it fails, and
     * /flash is not mounted. */
    GOGO_EXPERIMENTAL const char *const *grant;
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
    GOGO_EXPERIMENTAL const char *fs_root;
    /* 0 = no quota (the host file system's own limits; fs.Usage reports
     * its usage). Else the bytes the regular files under fs_root may hold:
     * what is there is counted when the VM is made (a walk of the whole
     * tree), then the VM tracks how much each write grows a file and what
     * truncation, removal and replacement give back; a write past it
     * stores what fits and is NoSpace, and fs.Usage reports (used,
     * fs_quota). Changes made by others (the host, another VM on the same
     * directory) are not seen. Negative: gogo_new fails. */
    GOGO_EXPERIMENTAL int64_t fs_quota;
    /* Ports open at once (the table grows as they open); 0 = the library's
     * default, 1024, or fewer when max_actors leaves less room: actor and
     * port PIDs share 65536 slots, so max_actors + max_ports may be at most
     * 65536. At most 2048 (gogo_new fails above). */
    GOGO_EXPERIMENTAL uint32_t max_ports;
    /* Node links of distributed actors at once (an embedded VM has no
     * "node" port today: kept for a server node in an embedding host);
     * 0 = the node port's default, 8. At most 2048. */
    GOGO_EXPERIMENTAL uint32_t max_links;
    /* The VM's parameters (as gogovm's flags; book 18.3). Each beats the
     * host's config key of the same parameter (read through config_get
     * when the host has one), which beats the library's default; a value
     * past the ceiling is clamped, with one line on stream 2. 0: not set.
     * actor_memory: an actor's default memory limit in bytes (key
     * vm.actor_memory; default 1 MB, at least 1 KB, at most 1 GB);
     * bytes_handles and bytes_per_actor: off-heap Bytes handles VM-wide
     * (vm.handles; default 4096, at most 65535) and per actor
     * (default 256, at most bytes_handles). mem_limit and max_actors above
     * are the vm.memory and vm.max_actors of the same order. */
    GOGO_EXPERIMENTAL uint32_t actor_memory;
    GOGO_EXPERIMENTAL uint32_t bytes_handles;
    GOGO_EXPERIMENTAL uint32_t bytes_per_actor;
    /* Fields are appended here, never inserted. */
} gogo_config;

#define GOGO_CONFIG_INIT { .size = sizeof(gogo_config) }

/* gogo_config.flags: code.Load may load modules. Without it every
 * code.Load returns NoMemory (bundled images only). GOGO_EXPERIMENTAL
 * (a flag, so only this comment carries the tag). */
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

/* A short description of a result: GOGO_OK, a GOGO_E*, or one of
 * gogo_step's GOGO_IDLE and GOGO_DONE (which are not errors). A shared
 * code from gogo_errors.h, such as a port.Closed reason, is described
 * negated: gogo_strerror(-reason). A static string, "unknown error" for
 * any other value. */
GOGO_API const char *gogo_strerror(int rc);

/* The library's version, (major << 16) | (minor << 8) | patch: what
 * GOGO_VERSION was when it was built. A host compiled against this
 * header needs a library of the same major, during 0.x the same minor,
 * at least as new: gogo_version() >= GOGO_VERSION. tools/release.sh
 * refuses a release tag vX.Y.Z that differs from these. */
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
 * heap and mailbox (1 MB in all by default), or it is dropped. */
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

/* ---- inspecting an image, without a VM (GOGO_EXPERIMENTAL) ----
 *
 * Pure functions, any thread: they read the image (len bytes, 4-byte
 * aligned) and keep nothing. Each validates the whole image as gogo_load
 * does: GOGO_EINVAL (no image, unaligned, no out), GOGO_EIMAGE,
 * GOGO_EVERSION (another major or a newer minor; info->format is still
 * filled in). */
typedef struct gogo_image_info {
    uint32_t size;          /* in: the caller's sizeof(gogo_image_info) (GOGO_IMAGE_INFO_INIT);
                             * out: the bytes this library filled (no more than in) */
    uint32_t format;        /* the image's format, major << 16 | minor */
    uint32_t n_requires;    /* its requirements (gogo_image_requires) */
    uint32_t n_exports;     /* its exported entry points (main among them) */
    /* Fields are appended here, never inserted. The library writes every
     * field within the caller's size that it knows (0 on an error) and
     * sets size to how far that is: a field past it, which a newer
     * header has and this library lacks, keeps the caller's value, so
     * check size before reading it. */
} gogo_image_info;
#define GOGO_IMAGE_INFO_INIT { .size = sizeof(gogo_image_info) }
GOGO_API GOGO_EXPERIMENTAL int gogo_image_info_get(const void *image, size_t len, gogo_image_info *info);

/* A name the API gives back: a pointer (not NUL-terminated) and a length.
 * A plain pair, fixed for good (it is used in arrays). */
typedef struct gogo_name {
    const char *ptr;
    size_t len;
} gogo_name;

/* The image's requirements, validating it once: each a port driver's
 * name ("ui", a host port's, "tcp") or a mount prefix ("/flash", "/sd"),
 * as gogo_load checks them. *n is how many the image has; the first
 * min(*n, cap) go to out[] (out may be NULL when cap is 0, to count).
 * The names point into the image (valid while it is). A host may read
 * them to register exactly the host ports an image needs. */
GOGO_API GOGO_EXPERIMENTAL int gogo_image_requires(const void *image, size_t len, gogo_name *out, uint32_t cap,
                                                   uint32_t *n);

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

/* Owner thread: queues port.Post{kind, data[0..len)} for the actor
 * registered as name (name_len bytes) and wakes the host. The data is
 * copied. 0, or GOGO_EFULL, GOGO_E2BIG, GOGO_ENOMEM, GOGO_EINVAL,
 * GOGO_ECLOSED (posts closed, or the program finished). Other threads post
 * with gogo_poster_post. */
GOGO_API int gogo_post(gogo_vm *vm, const char *name, size_t name_len, int32_t kind, const void *data, size_t len);

/* Owner thread: later posts return GOGO_ECLOSED, registered names no
 * longer keep the VM alive, and the host is woken, so a VM whose actors
 * only wait for posts ends (gogo_run returns 0). Posts already queued are
 * still delivered. A post concurrent with the close may return 0 and
 * still be discarded (the VM finished first); it is counted in
 * posts_dropped. gogo_free implies it. gogo_poster_close_posts is the
 * same from another thread. */
GOGO_API void gogo_close_posts(gogo_vm *vm);

/* Starts the VM if it is not yet started, then steps it, waiting in
 * host->wait while it is idle, until it is done. 0 when the program has
 * finished; GOGO_EINVAL without a wait hook; GOGO_ECLOSED if gogo_free
 * stopped it (gogo_run_thread, gogo_posix.h); a gogo_start error. */
GOGO_API int gogo_run(gogo_vm *vm);

/* ---- host ports: the embedder's own extension surface ----
 *
 * GOGO_EXPERIMENTAL: everything in this section.
 *
 * A host defines ports of its own, which Go code opens by name like any
 * port: port.Open("ui", port.Host{Arg: 1, Data: cfg}) calls the driver's
 * open; actor.Send(p, port.Post{Kind: k, Data: d}) calls its command; the
 * port's events reach its owner as port.Post{From, Kind, Data}, From the
 * port's PID (gogo_port_emit, gogo_poster_port_post). Ports keep their usual rules: the owner's exit,
 * port.Close or actor.Kill closes one (the driver's close runs once),
 * port.Handoff gives it to another actor, a stale PID is ignored. What a
 * host port may do is the host's own business. An image that opens a host
 * port by a constant name requires it: register the port before
 * gogo_load, or the load is refused (GOGO_EREQUIRES). While posts are open (gogo_close_posts), an
 * open host port keeps the VM alive, as it can still get events; after
 * the close it no longer does (a host that only emits between steps
 * closes posts when it is done with its ports). An event's From is its
 * port, so an actor that owns several host ports, or also gets posts
 * (From the zero PID), tells them apart. A port that ends on the host's
 * side (a device unplugged) is closed with gogo_port_close.
 *
 * The callbacks run on the VM thread inside gogo_step (close also inside
 * gogo_free) and are hooks: gogo_step, gogo_run, gogo_load and gogo_start
 * return GOGO_EBUSY there. data is valid during the call only; open's is
 * the cfg's Data, command's the command's Data. A gogo_port names one
 * open port: a nonzero value, opaque to the host, made of the port's slot
 * and a 12-bit generation bumped at each close of the slot, so a closed
 * port's value names no later port until that slot has been opened and
 * closed 4096 more times (as for port PIDs). */
typedef uint32_t gogo_port;
typedef struct gogo_port_driver {
    uint32_t size;          /* sizeof(gogo_port_driver) as the host was compiled (GOGO_PORT_DRIVER_INIT) */
    uint32_t reserved;      /* 0 */
    /* port.Open: 0 to accept, nonzero to refuse. A negative GOGO_E* of a
     * shared code (GOGO_EINVAL, GOGO_ENOMEM, GOGO_EBUSY, ... down to
     * -GOGO_E_SHARED_LAST) is the error port.Open returns (errors.BadArg,
     * errors.NoMemory, errors.Busy, ...); any other nonzero value
     * (GOGO_ESTATE, a positive one) is errors.Busy. -1 is GOGO_ENOTFOUND:
     * return GOGO_EBUSY for "taken". NULL accepts every open. Events it emits (gogo_port_emit
     * returns 0) are queued on the owner once the open succeeds, then
     * subject to its quota like any event (over it, dropped); if it
     * refuses, they are discarded. */
    int  (*open)(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len);
    /* A port.Post sent to the port (by any actor). NULL ignores them; so
     * is any other message. */
    void (*command)(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len);
    /* The port has closed (any reason, gogo_port_close and gogo_free
     * too); gogo_port_emit to it returns GOGO_ECLOSED already. NULL:
     * nothing. */
    void (*close)(void *ctx, gogo_vm *vm, gogo_port port);
    /* Fields are appended here, never inserted. */
} gogo_port_driver;

#define GOGO_PORT_DRIVER_INIT { .size = sizeof(gogo_port_driver) }

/* The longest host port name. A VM takes a few host ports (4 in this
 * version; GOGO_EFULL beyond). */
#define GOGO_PORT_NAME_MAX 31u

/* Registers a host port driver as name (name_len bytes, no NUL among
 * them; both are copied),
 * its callbacks getting ctx. Before gogo_load (a registered port
 * satisfies an image's requirement of its name). 0, or GOGO_EEXIST (a
 * port of this name exists, or it names one of the runtime's drivers,
 * "gpio", "tcp", "screen", ..., or one of the board's), GOGO_EFULL (the
 * VM has as many host ports as it takes), GOGO_E2BIG (the name),
 * GOGO_EINVAL (no name, or no driver or one without a size),
 * GOGO_ESTATE (an image is loaded),
 * GOGO_EBUSY. A port registered under a name the config's grant leaves
 * out is accepted but never visible to the program. */
GOGO_API GOGO_EXPERIMENTAL int gogo_register_port(gogo_vm *vm, const char *name, size_t name_len, const gogo_port_driver *drv,
                                void *ctx);

/* VM thread (a callback, or between steps): sends the port's owner
 * port.Post{kind, data[0..len)} (copied) now. 0, or GOGO_ECLOSED (the port
 * is closed or closing, or its owner is exiting), GOGO_EFULL (over the
 * owner's mailbox quota or Bytes limit: dropped, the owner is not
 * killed), GOGO_ENOMEM,
 * GOGO_E2BIG (over GOGO_POST_DATA_MAX), GOGO_EINVAL. */
GOGO_API GOGO_EXPERIMENTAL int gogo_port_emit(gogo_vm *vm, gogo_port port, int32_t kind, const void *data, size_t len);

/* VM thread (a callback, or between steps): the port has ended on the
 * host's side. Every post already queued is delivered first, in order
 * (the whole inbox, as the next step would: so are the port's own
 * gogo_poster_port_post events); then its owner gets port.Closed{From:
 * the port, Reason: reason}, which is never dropped (it was made when the
 * port opened and is queued outside the owner's quota, like an exit
 * notice); then the port is closed and the driver's close runs, as for
 * port.Close. reason is a positive code, a shared one of gogo_errors.h
 * such as GOGO_E_IO (0 would be stream Normal, "the peer finished
 * sending"). 0; GOGO_ECLOSED (the port is closed or closing, or its owner
 * is exiting); GOGO_EBUSY (from the port's own open: refuse the open
 * instead); GOGO_EINVAL (a reason <= 0 or over 2^30 - 1, a bad port). */
GOGO_API GOGO_EXPERIMENTAL int gogo_port_close(gogo_vm *vm, gogo_port port, int32_t reason);

/* ---- board ports: the same drivers, compiled into a firmware or gogovm ----
 *
 * GOGO_EXPERIMENTAL, as host ports are.
 *
 * A board (the ESP32-S3 firmware, gogovm) does not boot through
 * gogo_new, but it runs the same host-port layer when a driver of its
 * build registers itself with GOGO_BOARD_PORT, in a file of its own
 * (an ESP-IDF component of the product's, a source given to gogovm's
 * build):
 *
 *     static const gogo_port_driver dice = { .size = sizeof(gogo_port_driver),
 *                                            .open = dice_open, .command = dice_command };
 *     GOGO_BOARD_PORT("dice", &dice, NULL);
 *
 * At boot, after the VM is made and before the image loads, the board
 * registers every such port as gogo_register_port would (so an image
 * requirement of "dice" is satisfied), in name order. A port refused
 * (its name taken by one of the runtime's or the board's drivers, too
 * long, too many ports, a bad entry) is named on stream 2 at boot and
 * left out; so are all the entries of a name two or more of them have. The driver then works exactly
 * as a host port: its callbacks run on the VM's thread (the VM task) and
 * get a gogo_vm handle that takes gogo_port_emit, gogo_port_close,
 * gogo_post and gogo_poster_new, and nothing else of this header (those
 * calls are libgogo's alone). gogo_close_posts and
 * gogo_poster_close_posts link but do nothing on a board: posts belong
 * to every driver of the board, and close when the program ends. A poster made from it
 * may be used from any other thread or task, never from an interrupt
 * handler: a post allocates. The board's host-port layer is a build
 * option (CONFIG_GOGO_BOARD_PORTS on the ESP32-S3, which `gogo firmware
 * -component` sets; always in gogovm): a GOGO_BOARD_PORT in a build
 * without it fails to link, naming gogo_board_ports_enabled.
 *
 * The entries go in a linker section the board walks (no code runs to
 * register them): ESP-IDF's linker fragment keeps it, so the component
 * must be linked whole (idf_component_register(... WHOLE_ARCHIVE)), or
 * the linker drops a file nothing else refers to. */
typedef struct gogo_board_port {
    uint32_t size;                    /* sizeof(gogo_board_port) as the driver was compiled */
    uint32_t reserved;                /* 0 */
    const char *name;                 /* the requirement name: NUL-terminated, at most GOGO_PORT_NAME_MAX bytes */
    const gogo_port_driver *driver;
    void *ctx;                        /* given to every callback */
    /* Fields are appended here, never inserted. */
} gogo_board_port;

/* Defined by a board's host-port layer only (see above). */
extern const char gogo_board_ports_enabled;

#if defined(__APPLE__)
#define GOGO_BOARD_PORT_SECTION "__DATA,gogo_bport"
#elif defined(ESP_PLATFORM)
#define GOGO_BOARD_PORT_SECTION ".gogo_board_port"
#else
#define GOGO_BOARD_PORT_SECTION "gogo_board_port"
#endif
/* The section holds pointers, laid end to end: AddressSanitizer must not
 * pad them (GCC leaves globals with a section alone). */
#if defined(__clang__)
#define GOGO_BOARD_PORT_ATTR __attribute__((used, section(GOGO_BOARD_PORT_SECTION), no_sanitize("address")))
#else
#define GOGO_BOARD_PORT_ATTR __attribute__((used, section(GOGO_BOARD_PORT_SECTION)))
#endif
#define GOGO_BOARD_PORT_CAT2(a, b) a##b
#define GOGO_BOARD_PORT_CAT(a, b) GOGO_BOARD_PORT_CAT2(a, b)
/* At file scope, once per line. */
#define GOGO_BOARD_PORT(name, driver, ctx)                                                                  \
    static const gogo_board_port GOGO_BOARD_PORT_CAT(gogo_board_port_, __LINE__) = {                        \
        sizeof(gogo_board_port), 0, (name), (driver), (ctx) };                                              \
    static const void *const GOGO_BOARD_PORT_CAT(gogo_board_port_layer_, __LINE__) __attribute__((used)) =  \
        &gogo_board_ports_enabled;                                                                          \
    static const gogo_board_port *const GOGO_BOARD_PORT_CAT(gogo_board_port_ref_, __LINE__)                \
        GOGO_BOARD_PORT_ATTR = &GOGO_BOARD_PORT_CAT(gogo_board_port_, __LINE__)

/* ---- posting from other threads ----
 *
 * A gogo_poster is a handle any thread may use to post to the VM. It
 * holds a reference: the VM's memory (not its program, which gogo_free
 * stops) stays until the owner's gogo_free and every gogo_poster_free,
 * whichever comes last; then the host's destroy hook runs. After
 * gogo_free a poster's posts return GOGO_ECLOSED. Posts from one thread
 * are delivered in the order it made them. */
typedef struct gogo_poster gogo_poster;

/* A new poster for vm (a small block from the host's alloc), or NULL if
 * vm is NULL or memory runs out. Where gogo_free may be called: the
 * owner thread, or, while gogo_run_thread runs the VM, the thread that
 * started it. Hand it to another thread before that thread posts; free
 * it there (or anywhere) when done, once. */
GOGO_API gogo_poster *gogo_poster_new(gogo_vm *vm);

/* Any thread: gogo_post, as the owner would. */
GOGO_API int gogo_poster_post(gogo_poster *p, const char *name, size_t name_len, int32_t kind, const void *data,
                              size_t len);

/* Any thread: gogo_port_emit through the inbox: queued, wakes the host,
 * delivered at the VM's next step. Returns as gogo_post does (GOGO_ECLOSED
 * once posts are closed). A post the port can no longer take when it is
 * delivered (closed since, its owner exiting, over quota) is dropped and
 * counted in posts_dropped. */
GOGO_API GOGO_EXPERIMENTAL int gogo_poster_port_post(gogo_poster *p, gogo_port port, int32_t kind, const void *data, size_t len);

/* Any thread: gogo_close_posts. */
GOGO_API void gogo_poster_close_posts(gogo_poster *p);

/* Any thread: drops the poster's reference and gives its block back; p
 * is invalid after. NULL is ignored. Using a poster after its free,
 * a second free included, is undefined behaviour; a GOGO_DEBUG build
 * often catches it (an assertion on the poster's state word, while its
 * block has not been reused). */
GOGO_API void gogo_poster_free(gogo_poster *p);

/* Counters (GOGO_EXPERIMENTAL: the fields may change). On the owning
 * thread: with gogo_run_thread, after gogo_join (or from a hook). The
 * caller sets out->size (GOGO_STATS_INIT); the library fills the fields
 * it knows within it and sets out->size to how far that is (as
 * gogo_image_info: a field past it is not this library's). 0, or
 * GOGO_EINVAL. */
typedef struct gogo_stats {
    uint32_t size;             /* in: the caller's sizeof(gogo_stats); out: the bytes filled */
    uint32_t reserved;
    uint64_t posts_delivered;  /* posts queued on their receiver */
    uint64_t posts_dropped;    /* posts not delivered: unknown name, over quota, out of memory,
                                * or the VM finished first */
    uint64_t mem_used;         /* the VM allocator's bytes in use */
    uint64_t mem_peak;         /* its highest use */
    /* Fields are appended here, never inserted. */
} gogo_stats;
#define GOGO_STATS_INIT { .size = sizeof(gogo_stats) }
GOGO_API GOGO_EXPERIMENTAL int gogo_get_stats(gogo_vm *vm, gogo_stats *out);

/* Closes posts (gogo_close_posts) and the program's ports and returns
 * every byte the VM took from its host (pending posts included); a VM running in gogo_run_thread is
 * stopped and joined first. Never from a hook. Drops the owner's
 * reference: vm is invalid after it (posters keep their own). NULL is
 * ignored. */
GOGO_API void gogo_free(gogo_vm *vm);

#ifdef __cplusplus
}
#endif

#endif
