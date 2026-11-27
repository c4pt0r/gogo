/* The host-port layer's internals (runtime/embed): host ports (hostport.c),
 * the post inbox (inbox.c) and posters (poster.c), in an EmbedPorts that
 * a VM's shell holds beside its Vm. Two shells have one: libgogo's
 * gogo_vm (embed.h), whose first member it is, and a board's, made by
 * runtime/board/board_ports.c for the ports a firmware or gogovm build
 * registers with GOGO_BOARD_PORT. Either way the gogo_vm handle the
 * public calls take is the EmbedPorts (embed_ports), and nothing here
 * needs the rest of libgogo. Pure C99 with the __atomic builtins. */
#ifndef GOGO_EMBED_PORTS_H
#define GOGO_EMBED_PORTS_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/gogo.h"
#include "actor.h"
#include "inbox.h"

/* EmbedPorts.state, only ever growing: LIVE takes posts; CLOSED
 * (gogo_close_posts) refuses them and no longer counts registered names
 * as liveness; DEAD (gogo_free) also stops gogo_run. */
enum { EMBED_LIVE = 0, EMBED_CLOSED = 1, EMBED_DEAD = 2 };

/* Most host ports per VM (gogo_register_port's GOGO_EFULL beyond); a
 * board build may give its own (board ports are host ports). */
#ifndef EMBED_HOST_PORTS
#define EMBED_HOST_PORTS 4
#endif

/* The most board drivers the driver list takes in front of the host
 * ports (libgogo's board has none; a board with host ports gives its
 * own count). */
#ifndef EMBED_BOARD_PORTS_MAX
#define EMBED_BOARD_PORTS_MAX 8
#endif
#define EMBED_PORT_LIST (EMBED_BOARD_PORTS_MAX + EMBED_HOST_PORTS + 1)

typedef struct EmbedPorts EmbedPorts;

/* A registered host port (hostport.c): base first, so a Port's drv points
 * back to its HostPort, and the HostPort to its EmbedPorts. */
typedef struct HostPort {
    PortDriver base;
    gogo_port_driver pub;
    void *ctx;
    EmbedPorts *ep;
    char name[GOGO_PORT_NAME_MAX + 1u];
} HostPort;

struct EmbedPorts {
    Vm *vm;                      /* the VM the ports are in */
    /* The host table posts and posters allocate from and wake through
     * (alloc, free and wake callable from any thread), with its ctx. */
    const gogo_host *uh;
    void *uctx;
    const char *const *grant;    /* names the program may see (NULL: all) */
    /* Called when the last reference goes (unref): libgogo frees its
     * gogo_vm; NULL: the shell is never freed (a board's). */
    void (*last_ref)(EmbedPorts *ep);
    Inbox inbox;
    HostPort host_ports[EMBED_HOST_PORTS];
    const PortDriver *port_list[EMBED_PORT_LIST];
    uint8_t n_host_ports;
    /* The inbox keeps the VM alive while some actor holds a name a post
     * can reach (libgogo); a board's does only while a post is queued. */
    bool names_live;
    bool loaded;                 /* an image is loaded: no more registrations */
    bool stepping;               /* inside a step or hook (re-entry guard) */
    /* The host port whose open or close callback is running (hostport.c):
     * events emitted during its open wait in held (oldest first) until the
     * open succeeds; during its close, emit is refused. */
    Port *opening, *closing;
    /* Each open host port's Closed message, made at the open so
     * gogo_port_close never fails to send it (by port index), or NULL; the
     * array (closed_n entries: the port table's size when a host port last
     * needed more) grows as host ports open. */
    Msg **closed_msg;
    uint16_t closed_n;
    Msg *held_head, *held_tail;
    /* Atomics (__atomic builtins): state, the shell's references (the
     * owner's plus one per gogo_poster) and posts in progress. */
    int state;
    int refs;
    int posting;
};

/* The public handle is the EmbedPorts (libgogo's gogo_vm begins with it). */
static inline EmbedPorts *embed_ports(gogo_vm *g) { return (EmbedPorts *)(void *)g; }
static inline gogo_vm *embed_handle(EmbedPorts *ep) { return (gogo_vm *)(void *)ep; }

static inline int embed_state(const EmbedPorts *ep) { return __atomic_load_n(&ep->state, __ATOMIC_ACQUIRE); }

/* Raises the state to at least s (never lowers it); seq_cst, as the post
 * handshake needs. Returns the state before (poster.c). */
int embed_set_state(EmbedPorts *ep, int s);

/* poster.c: drops a reference; the last one calls last_ref. */
void embed_unref(EmbedPorts *ep);

/* True if the grant has name (or there is no grant: all). */
bool embed_granted(const EmbedPorts *ep, const char *name);

/* hostport.c: points the Vm at the driver list (after vm_init). */
void embed_ports_install(EmbedPorts *ep);

/* hostport.c: whether name is a runtime driver's (port_names.h) or one of
 * board's drivers (NULL-terminated, or NULL): a host port may not take it. */
bool embed_name_reserved(const PortDriver *const *board, const char *name);

/* hostport.c: sends host port `port` (an inbox node's encoding) a post as
 * gogo_port_emit does: 0, or a GOGO_E* error. VM thread. */
int embed_port_send(EmbedPorts *ep, uint32_t port, int32_t kind, const uint8_t *data, uint32_t len);
/* gogo_port_close's largest reason: an immediate int. */
#define EMBED_REASON_MAX TERM_INT_MAX

/* hostport.c: whether port can name a port slot at all (nonzero, its
 * index in range). */
bool embed_port_valid(gogo_port port);

/* gogo_port_driver's first size (v0.2), which never changes. */
#define EMBED_DRIVER_SIZE_V1 (offsetof(gogo_port_driver, close) + sizeof(void (*)(void)))

/* *dst = the host's struct src of the given size: its first min(size,
 * cap) bytes, the rest zero; false (dst zeroed) if size < min (hostport.c). */
bool embed_copy_sized(void *dst, size_t cap, const void *src, uint32_t size, size_t min);

#endif
