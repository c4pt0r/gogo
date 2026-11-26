/* The embedding layer's internals (runtime/embed): what a gogo_vm holds
 * around its Vm. Linked into libgogo and its tests only, never into a
 * board's firmware. */
#ifndef GOGO_EMBED_H
#define GOGO_EMBED_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/gogo_posix.h"   /* and gogo.h */
#include "actor.h"
#include "inbox.h"
#include "vfs_int.h"
#include "fs_dir.h"

/* A tracked block of host memory: every block the Vm takes through its
 * alloc / code_alloc hooks is linked into a list (the header sits in front
 * of the block, which keeps malloc's alignment), so gogo_free can return
 * whatever is still live without walking the VM's structures. */
typedef struct EmbedBlk {
    struct EmbedBlk *prev, *next;
    size_t n;                    /* the whole allocation, header included */
} EmbedBlk;
#define EMBED_BLK_HDR 32u
typedef char embed_blk_fits[sizeof(EmbedBlk) <= EMBED_BLK_HDR ? 1 : -1];

/* gogo_vm.state, only ever growing: LIVE takes posts; CLOSED
 * (gogo_close_posts) refuses them and no longer counts registered names
 * as liveness; DEAD (gogo_free) also stops gogo_run. */
enum { EMBED_LIVE = 0, EMBED_CLOSED = 1, EMBED_DEAD = 2 };

struct EmbedThread;   /* host_posix.c: gogo_run_thread's thread */

/* Most host ports per VM (gogo_register_port's GOGO_EFULL beyond). */
#define EMBED_HOST_PORTS 4

/* A registered host port (hostport.c): base first, so a Port's drv points
 * back to its HostPort. */
typedef struct HostPort {
    PortDriver base;
    gogo_port_driver pub;
    void *ctx;
    char name[GOGO_PORT_NAME_MAX + 1u];
} HostPort;

/* The driver list a Vm with host ports looks names up in (Vm.drivers):
 * the board's drivers first, then the host's, NULL-terminated. */
#define EMBED_BOARD_PORTS_MAX 8
#define EMBED_PORT_LIST (EMBED_BOARD_PORTS_MAX + EMBED_HOST_PORTS + 1)

struct gogo_vm {
    Vm vm;                       /* its host is .fwd, with ctx this gogo_vm */
    gogo_host uh;                /* the host's own table (gogo_new's copy) and ctx */
    void *uctx;
    gogo_host fwd;               /* forwarders to uh (NULL where uh has NULL) */
    gogo_config cfg;
    EmbedBlk *blocks;            /* live alloc blocks */
    EmbedBlk *code_blocks;       /* live code_alloc blocks */
    Inbox inbox;
    VfsState vfs;                /* its mounts (none without fs_root) and open files */
    FsDir *fs;                   /* cfg.fs_root as /flash (from uh.alloc), or NULL */
    void *image_copy;            /* GOGO_LOAD_COPY: the copy (from uh.alloc) */
    size_t image_bytes;
    struct EmbedThread *thread;  /* gogo_run_thread, until gogo_join */
    HostPort host_ports[EMBED_HOST_PORTS];
    const PortDriver *port_list[EMBED_PORT_LIST];
    uint8_t n_host_ports;
    /* The host port whose open or close callback is running (hostport.c):
     * events emitted during its open wait in held (oldest first) until the
     * open succeeds; during its close, emit is refused. */
    Port *opening, *closing;
    /* Each open host port's Closed message, made at the open so
     * gogo_port_close never fails to send it (by port index), or NULL; the
     * array (vm.port_max entries) is made at the first host port's open. */
    Msg **closed_msg;
    Msg *held_head, *held_tail;
    /* Atomics (__atomic builtins): state, the shell's references (the
     * owner's plus one per gogo_poster) and posts in progress. */
    int state;
    int refs;
    int posting;
    bool loaded;                 /* gogo_load succeeded */
    bool stepping;               /* inside gogo_step (re-entry guard) */
};

/* *dst = the host's struct src of the given size: its first min(size,
 * cap) bytes, the rest zero; false (dst zeroed) if size < min (gogo.c). */
bool embed_copy_sized(void *dst, size_t cap, const void *src, uint32_t size, size_t min);

/* True if the config's grant has name (or there is no grant: all). */
bool embed_granted(const gogo_vm *g, const char *name);

static inline int embed_state(const gogo_vm *g) { return __atomic_load_n(&g->state, __ATOMIC_ACQUIRE); }

/* gogo_run's loop stops between steps once the VM is being freed. */
static inline bool embed_dead(const gogo_vm *g) { return embed_state(g) == EMBED_DEAD; }

/* Raises the state to at least s (never lowers it); seq_cst, as the post
 * handshake needs. Returns the state before. */
int embed_set_state(gogo_vm *g, int s);

/* hostport.c: points the Vm at the driver list (vm_setup; after vm_init). */
void embed_ports_install(gogo_vm *g);

/* hostport.c: sends host port `port` (an inbox node's encoding) a post as
 * gogo_port_emit does: 0, or a GOGO_E* error. VM thread. */
int embed_port_send(gogo_vm *g, uint32_t port, int32_t kind, const uint8_t *data, uint32_t len);
/* gogo_port_close's largest reason: an immediate int. */
#define EMBED_REASON_MAX TERM_INT_MAX

/* hostport.c: whether port can name a port slot at all (nonzero, its
 * index in range). */
bool embed_port_valid(gogo_port port);

/* host_posix.c: stops and joins a gogo_run_thread thread (gogo_free). */
void embed_thread_stop(gogo_vm *g);

#endif
