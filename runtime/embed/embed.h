/* The embedding layer's internals (runtime/embed): what a gogo_vm holds
 * around its Vm. Linked into libgogo and its tests only, never into a
 * board's firmware. */
#ifndef GOGO_EMBED_H
#define GOGO_EMBED_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/gogo_posix.h"   /* and gogo.h */
#include "actor.h"
#include "params.h"
#include "ports.h"
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

struct EmbedThread;   /* host_posix.c: gogo_run_thread's thread */

/* The library's shell around a Vm. ports comes first: the gogo_vm handle
 * is also the host-port layer's (ports.h embed_ports). */
struct gogo_vm {
    EmbedPorts ports;            /* host ports, posts and posters; its vm is &vm, its uh &uh */
    Vm vm;                       /* its host is .fwd, with ctx this gogo_vm */
    gogo_host uh;                /* the host's own table (gogo_new's copy) and ctx */
    void *uctx;
    gogo_host fwd;               /* forwarders to uh (NULL where uh has NULL) */
    gogo_config cfg;
    VmParams params;             /* resolved once by gogo_new (params.h): cfg, the host's keys, defaults */
    char ptext[PARAM_COUNT][24]; /* cfg's parameters as text for the config_get overlay ("": not set) */
    EmbedBlk *blocks;            /* live alloc blocks */
    EmbedBlk *code_blocks;       /* live code_alloc blocks */
    VfsState vfs;                /* its mounts (none without fs_root) and open files */
    FsDir *fs;                   /* cfg.fs_root as /flash (from uh.alloc), or NULL */
    void *image_copy;            /* GOGO_LOAD_COPY: the copy (from uh.alloc) */
    size_t image_bytes;
    struct EmbedThread *thread;  /* gogo_run_thread, until gogo_join */
};

/* gogo_run's loop stops between steps once the VM is being freed. */
static inline bool embed_dead(gogo_vm *g) { return embed_state(&g->ports) == EMBED_DEAD; }

/* host_posix.c: stops and joins a gogo_run_thread thread (gogo_free). */
void embed_thread_stop(gogo_vm *g);

#endif
