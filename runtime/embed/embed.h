/* The embedding layer's internals (runtime/embed): what a gogo_vm holds
 * around its Vm. Linked into libgogo and its tests only, never into a
 * board's firmware. */
#ifndef GOGO_EMBED_H
#define GOGO_EMBED_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/gogo.h"
#include "actor.h"
#include "inbox.h"

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

/* gogo_vm.state */
enum { EMBED_LIVE = 0, EMBED_DEAD = 1 };

struct EmbedThread;   /* host_posix.c: gogo_run_thread's thread */

struct gogo_vm {
    Vm vm;                       /* its host is .fwd, with ctx this gogo_vm */
    const gogo_host *uh;         /* the host's own table and ctx */
    void *uctx;
    gogo_host fwd;               /* forwarders to uh (NULL where uh has NULL) */
    gogo_config cfg;
    EmbedBlk *blocks;            /* live alloc blocks */
    EmbedBlk *code_blocks;       /* live code_alloc blocks */
    Inbox inbox;
    void *image_copy;            /* GOGO_LOAD_COPY: the copy (from uh->alloc) */
    size_t image_bytes;
    struct EmbedThread *thread;  /* gogo_run_thread, until gogo_join */
    /* Atomics (__atomic builtins): state, the shell's references (the
     * owner's plus gogo_retain's) and gogo_post calls in progress. */
    int state;
    int refs;
    int posting;
    bool loaded;                 /* gogo_load succeeded */
    bool stepping;               /* inside gogo_step (re-entry guard) */
    bool own_ctx;                /* uctx is a POSIX context the VM made */
};

/* gogo_run's loop asks for a stop between steps once state is EMBED_DEAD. */
static inline bool embed_dead(const gogo_vm *g) {
    return __atomic_load_n(&g->state, __ATOMIC_ACQUIRE) != EMBED_LIVE;
}

/* host_posix.c: stops and joins a gogo_run_thread thread (gogo_free). */
void embed_thread_stop(gogo_vm *g);

#endif
