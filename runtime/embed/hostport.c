/* Host ports (gogo_register_port): ports whose driver is the embedding
 * program's. Each registration is a HostPort, a PortDriver with the host's
 * callbacks behind it; the Vm finds them by name through its driver list
 * (Vm.drivers: the board's drivers, then the host's), so the VM core needs
 * nothing of its own for them. The cfg is a port.Host{Arg, Data} and the
 * commands are port.Post{From, Kind, Data} (From ignored), both read in
 * place; events go to the owner as port.Post messages (From: the port), their Data inline when short, else in an
 * off-heap Bytes the port makes for its owner (as Buffer.Freeze would). */
#include "embed.h"
#include "interp.h"
#include "heap.h"
#include "bytes.h"
#include "msg.h"
#include "opcodes.h"
#include <string.h>

/* A gogo_port (and an inbox node's port): index and generation, never 0. */
#define PORT_ENC(i, g) (0x80000000u | (uint32_t)(i) << 16 | (uint32_t)(g))

static gogo_port port_val(const Vm *vm, const Port *p) { return PORT_ENC(p - vm->ports, p->gen); }

static const HostPort *host_of(const Port *p) { return (const HostPort *)(const void *)p->drv; }

/* Queues the events held during an open on the owner (it succeeded), or
 * frees them (refused). */
static void flush_held(gogo_vm *g, Actor *owner, bool ok) {
    Msg *m = g->held_head;
    g->held_head = g->held_tail = NULL;
    while (m) {
        Msg *next = m->next;
        m->next = NULL;
        if (ok) (void)embed_enqueue_post(&g->vm, owner, m);
        else msg_free(&g->vm, m);
        m = next;
    }
}

static int hp_open(Vm *vm, const Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)index;
    gogo_vm *g = vm->host_ctx;
    const HostPort *hp = host_of(p);
    const uint32_t *o = obj_of(vm, owner, cfg);   /* a port.Host: port_open checked */
    int32_t arg;
    const uint8_t *d;
    uint32_t len;
    if (hdr_words(o[0]) != 4u || !int_val(vm, owner, o[2], &arg) || !bytes_view(vm, owner, o[3], &d, &len))
        return -1;
    /* The port's Closed is made now, so gogo_port_close can always send
     * it (a stream port's terminal Closed is never lost either). */
    Msg **closed = &g->closed_msg[p - vm->ports];
    if (!(*closed = embed_closed_msg(vm, p->id))) return -1;
    int r = 0;
    if (hp->pub.open) {
        g->opening = p;
        r = hp->pub.open(hp->ctx, g, port_val(vm, p), arg, d, len) ? -1 : 0;   /* any nonzero refuses */
        g->opening = NULL;
        /* A host port has no charge or room: open_in accepts it when r is 0. */
        flush_held(g, actor_lookup(vm, owner->id), r == 0);
    }
    if (r) {
        msg_free(vm, *closed);
        *closed = NULL;
    }
    return r;
}

static void hp_command(Vm *vm, const Actor *from, Port *p, term msg) {
    const HostPort *hp = host_of(p);
    const uint32_t *o = obj_of(vm, from, msg);
    int32_t kind;
    const uint8_t *d;
    uint32_t len;
    if (!hp->pub.command || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_POST || hdr_words(o[0]) != 5u ||
        !int_val(vm, from, o[3], &kind) || !bytes_view(vm, from, o[4], &d, &len))
        return;
    hp->pub.command(hp->ctx, vm->host_ctx, port_val(vm, p), kind, d, len);
}

static void hp_close(Vm *vm, Port *p) {
    gogo_vm *g = vm->host_ctx;
    const HostPort *hp = host_of(p);
    Msg **closed = &g->closed_msg[p - vm->ports];
    if (*closed) msg_free(vm, *closed);   /* not sent: an ordinary close */
    *closed = NULL;
    if (!hp->pub.close) return;
    g->closing = p;
    hp->pub.close(hp->ctx, g, port_val(vm, p));
    g->closing = NULL;
}

/* Events may still come while posts are open (gogo_poster_port_post). */
static bool hp_live(const Vm *vm, const Port *p) {
    (void)p;
    return embed_state(vm->host_ctx) == EMBED_LIVE;
}

/* The Vm's driver list: the board's, then the host ports, each only if
 * the grant has its name. */
void embed_ports_install(gogo_vm *g) {
    if (!g->n_host_ports && !g->cfg.grant) return;   /* vm_init left the board's list */
    const PortDriver *const *b = g->vm.board->ports;
    uint32_t n = 0;
    for (; b && *b && n < EMBED_BOARD_PORTS_MAX; b++)
        if (embed_granted(g, (*b)->name)) g->port_list[n++] = *b;
    for (uint32_t k = 0; k < g->n_host_ports; k++)
        if (embed_granted(g, g->host_ports[k].name)) g->port_list[n++] = &g->host_ports[k].base;
    g->port_list[n] = NULL;
    port_set_drivers(&g->vm, g->port_list);
}

/* The runtime's own driver names (port_names.h), reserved on every board:
 * an image that requires one is refused at load where the board lacks it,
 * rather than loading against a host port whose cfg it cannot use. */
static const char *const reserved_names[] = { PORT_RESERVED_NAMES, NULL };

/* A host port may not take a reserved name, the name of one of the
 * board's drivers (granted or not), so a program that opens one never gets
 * the host's port instead, or that of another host port. */
static bool name_taken(const gogo_vm *g, const char *name) {
    for (const char *const *r = reserved_names; *r; r++)
        if (!strcmp(*r, name)) return true;
    for (const PortDriver *const *b = g->vm.board->ports; b && *b; b++)
        if (!strcmp((*b)->name, name)) return true;
    for (uint32_t k = 0; k < g->n_host_ports; k++)
        if (!strcmp(g->host_ports[k].name, name)) return true;
    return false;
}

static uint32_t board_count(const gogo_vm *g) {
    uint32_t n = 0;
    for (const PortDriver *const *b = g->vm.board->ports; b && *b; b++) n++;
    return n;
}

int gogo_register_port(gogo_vm *g, const char *name, size_t nl, const gogo_port_driver *drv, void *ctx) {
    if (!g || !name || !nl || memchr(name, 0, nl) || !drv || drv->size < 2u * sizeof(uint32_t)) return GOGO_EINVAL;
    if (g->stepping) return GOGO_EBUSY;
    if (g->loaded) return GOGO_ESTATE;    /* registered ports count as the image's requirements */
    if (nl > GOGO_PORT_NAME_MAX) return GOGO_E2BIG;
    char z[GOGO_PORT_NAME_MAX + 1u];      /* the name as a C string, as drivers' names are */
    memcpy(z, name, nl);
    z[nl] = 0;
    if (name_taken(g, z)) return GOGO_EEXIST;
    if (g->n_host_ports == EMBED_HOST_PORTS || board_count(g) > EMBED_BOARD_PORTS_MAX) return GOGO_EFULL;
    HostPort *hp = &g->host_ports[g->n_host_ports++];
    memset(hp, 0, sizeof *hp);
    memcpy(hp->name, z, nl + 1u);
    memcpy(&hp->pub, drv, drv->size < sizeof hp->pub ? drv->size : sizeof hp->pub);   /* the rest stays NULL */
    hp->ctx = ctx;
    hp->base.name = hp->name;
    hp->base.cfg_type = TYPE_HOST;
    hp->base.event_type = TYPE_POST;
    hp->base.close = hp_close;
    hp->base.can_event = hp_live;
    hp->base.command_term = hp_command;
    hp->base.open_term = hp_open;
    embed_ports_install(g);
    return GOGO_OK;
}

bool embed_port_valid(gogo_port port) { return (port & 0x80000000u) && (port >> 16 & 0x7FFFu) < GOGO_MAX_PORTS; }

/* The open host port a gogo_port names, or NULL. */
static Port *host_port(gogo_vm *g, gogo_port port) {
    if (!embed_port_valid(port)) return NULL;
    Port *p = &g->vm.ports[port >> 16 & 0x7FFFu];
    for (uint32_t k = 0; k < g->n_host_ports; k++)
        if (p->drv == &g->host_ports[k].base) return p->gen == (port & 0xFFFFu) ? p : NULL;
    return NULL;
}

int embed_port_send(gogo_vm *g, uint32_t port, int32_t kind, const uint8_t *data, uint32_t len) {
    Vm *vm = &g->vm;
    Port *p = host_port(g, port);
    Actor *o = p && p != g->closing ? port_owner(vm, p) : NULL;
    if (!o) return GOGO_ECLOSED;
    Msg *m;
    if (len <= GOGO_BYTES_INLINE) {
        m = embed_post_msg(vm, p->id, kind, data, len, T_NIL);
    } else {
        /* Off-heap: the port makes the Bytes for its owner, the message
         * takes its own reference, and the port lets go before the
         * enqueue, so the owner's quota counts the bytes once (as the
         * mailbox's). A full holder list or limit may be garbage: collect
         * a waiting owner once, as a receive would. */
        uint8_t *w;
        term h = port_bytes_new(vm, p, len, &w);
        if (h == T_NIL && heap_reclaim_waiting(vm, o)) h = port_bytes_new(vm, p, len, &w);
        if (h == T_NIL) return GOGO_EFULL;
        memcpy(w, data, len);
        m = embed_post_msg(vm, p->id, kind, NULL, len, h);
        port_bytes_release(vm, p, h);
    }
    if (m && p == g->opening) {          /* held until the open's outcome */
        if (g->held_tail) g->held_tail->next = m;
        else g->held_head = m;
        g->held_tail = m;
        m->next = NULL;
        return 0;
    }
    return embed_enqueue_post(vm, o, m);
}

int gogo_port_emit(gogo_vm *g, gogo_port port, int32_t kind, const void *data, size_t len) {
    if (!g || (len && !data) || !embed_port_valid(port)) return GOGO_EINVAL;
    if (len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return embed_port_send(g, port, kind, data, (uint32_t)len);
}

/* Posts queued for the port first (delivered in order, as the next step
 * would), then Closed{From, Reason} to the owner from the message made
 * at the open, outside its quota (as an exit notice: it cannot be
 * dropped), then the ordinary close (the driver's close runs, the owner's
 * charges go back, the PID goes stale: the program's own port.Close of it
 * later does nothing). */
int gogo_port_close(gogo_vm *g, gogo_port port, int32_t reason) {
    if (!g || !embed_port_valid(port) || reason <= 0 || reason > EMBED_REASON_MAX) return GOGO_EINVAL;
    Vm *vm = &g->vm;
    Port *p = host_port(g, port);
    if (p && p == g->opening) return GOGO_EBUSY;
    if (!p || p == g->closing || !port_owner(vm, p)) return GOGO_ECLOSED;
    embed_inbox_drain(g);
    Actor *o = port_owner(vm, p);    /* the drain may have ended it (a mailbox overflow) */
    if (o) {
        Msg *m = g->closed_msg[p - vm->ports];
        g->closed_msg[p - vm->ports] = NULL;
        m->data[3] = term_from_int(reason);
        msg_enqueue(vm, o, m, false);
    }
    bool was = g->stepping;
    g->stepping = true;              /* the driver's close is a hook */
    port_close(vm, p->id);
    g->stepping = was;
    return GOGO_OK;
}
