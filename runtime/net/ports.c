/* The stack's instance (NetState, the board's storage), its port-layer
 * source (RX and link slots, Up/Down reports), the "netif" port and
 * net.Subscribe. */
#include "net_int.h"
#include "heap.h"
#include "alloc.h"
#include "opcodes.h"
#include "host.h"
#include "signals.h"
#include <string.h>

/* Fixed state within budget, on every target. */
typedef char net_fixed_state_fits[sizeof(NetState) <= NET_FIXED_LIMIT ? 1 : -1];
typedef char net_udp_pcb_fits[sizeof(UdpPcb) <= 32u ? 1 : -1];
typedef char net_tcp_pcb_fits[sizeof(TcpPcb) <= 64u ? 1 : -1];

uint32_t net_fixed_bytes(void) { return (uint32_t)sizeof(NetState); }
const NetStats *net_stats(const NetState *ns) { return &ns->st; }

uint32_t net_subs_used(const NetState *ns) {
    const volatile term *subs = ns->subs;
    uint32_t n = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) n += subs[i] != 0;
    return n;
}

/* ---- message blocks ---- */

term net_put_int(uint32_t *d, uint32_t *at, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    d[*at] = hdr_make(2, K_INT);
    d[*at + 1u] = (uint32_t)v;
    term t = term_from_ref(*at);
    *at += 2u;
    return t;
}

Msg *net_msg_new(Vm *vm, uint32_t words) {
    if (words > 0xFFFFu) return NULL;
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    return m;
}

bool net_room(Vm *vm, Actor *o, uint32_t words, uint32_t reserve) {
    for (int k = 0; k < 2; k++) {
        if ((uint32_t)o->mem_words + actor_charged_words(o) + words + reserve <= actor_max_words(o)) return true;
        if (k || !heap_reclaim_waiting(vm, o)) break;
    }
    return false;
}

bool net_deliver(Vm *vm, Actor *o, Msg *m, uint32_t reserve) {
    for (int k = 0; k < 2; k++) {
        if ((uint32_t)o->mem_words + actor_charged_words(o) + msg_charge_words(vm, m) + reserve <=
                actor_max_words(o) && msg_try_enqueue(vm, o, m)) return true;
        if (k || !heap_reclaim_waiting(vm, o)) break;
    }
    return false;
}

/* ---- Up / Down ---- */

/* Retry interval for an Up/Down that could not be allocated. */
#define NOTIFY_RETRY_MS 50u

static bool due(uint32_t t, uint32_t now) { return (int32_t)(now - t) >= 0; }

/* Up{IP, Gateway, DNS} or Down{} to a, never killing it;
 * false if the message could not be allocated or a has no room for it
 * (the caller owes it and retries). */
static bool notify(Vm *vm, Actor *a, bool up) {
    const NetState *ns = vm->net;
    int32_t v[3] = { (int32_t)ns->up_ip, (int32_t)ns->up_gw, (int32_t)ns->up_dns };
    uint32_t words = 2u;
    if (up) words = 5u + net_int_words(v[0]) + net_int_words(v[1]) + net_int_words(v[2]);
    Msg *m = net_msg_new(vm, words);
    if (!m) return false;
    uint32_t at = 5;
    m->data[0] = hdr_make(up ? 5u : 2u, K_STRUCT);
    m->data[1] = up ? TYPE_NET_UP : TYPE_NET_DOWN;
    if (up) {
        for (uint32_t i = 0; i < 3; i++) m->data[2 + i] = net_put_int(m->data, &at, v[i]);
    }
    if (net_deliver(vm, a, m, 0)) return true;   /* a waiting a's slack collected first */
    msg_free(vm, m);
    return false;
}

/* The live subscriber in slot i, or NULL (a dead one's slot is freed). */
static Actor *sub_at(Vm *vm, uint32_t i) {
    NetState *ns = vm->net;
    if (!ns->subs[i]) return NULL;
    Actor *a = actor_lookup(vm, ns->subs[i]);
    if (!a || a->state == A_EXITED || (a->flags & AF_EXIT_PENDING)) {
        ns->subs[i] = 0;
        return NULL;
    }
    return a;
}

/* Sends subscriber i the current state, or owes it (retried by the
 * source's drain). */
static void notify_sub(Vm *vm, uint32_t i, Actor *a) {
    NetState *ns = vm->net;
    uint8_t bit = (uint8_t)(1u << i);
    if (notify(vm, a, ns->is_up)) {
        ns->sub_owed &= (uint8_t)~bit;
        return;
    }
    if (!ns->sub_owed) ns->sub_retry_at = (uint32_t)vm_now(ns->vm) + NOTIFY_RETRY_MS;
    ns->sub_owed |= bit;
}

static bool owed_due(const NetState *ns) {
    return ns->sub_owed && due(ns->sub_retry_at, (uint32_t)vm_now(ns->vm));
}

static void retry_owed(Vm *vm) {
    NetState *ns = vm->net;
    uint8_t owed = ns->sub_owed;
    ns->sub_owed = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        if (!(owed & (1u << i))) continue;
        Actor *a = sub_at(vm, i);
        if (a) notify_sub(vm, i, a);
    }
}

/* Reports a change of the interface state to every subscriber. */
static void report(Vm *vm) {
    NetState *ns = vm->net;
    ns->dirty = 0;
    bool up = ns->link_up && ip_configured(ns);
    if (up) {
        if (ns->is_up && ns->up_ip == ns->ip && ns->up_gw == ns->gw && ns->up_dns == ns->dns) return;
        ns->up_ip = ns->ip;
        ns->up_gw = ns->gw;
        ns->up_dns = ns->dns;
    } else if (!ns->is_up) {
        return;
    }
    ns->is_up = up;
    if (!up) tcp_down(vm);
    ns->nif->report(ns->nif->ctx, up, up ? ns->up_ip : 0u, up ? ns->up_gw : 0u, up ? ns->up_dns : 0u);
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        Actor *a = sub_at(vm, i);
        if (a) notify_sub(vm, i, a);
    }
}

/* Pre-resolves the gateway, so the first packet routed through it is not
 * dropped for an ARP miss. */
static void resolve_gateway(NetState *ns) {
    if (ns->link_up && ns->gw && ip_on_link(ns, ns->gw)) arp_request(ns, ns->gw);
}

static bool s_subscribe(Vm *vm, void *ctx, Actor *a) {
    NetState *ns = ctx;
    uint32_t free = GOGO_NET_MAX_SUBS;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        if (ns->subs[i] == a->id) return true;
        if (!sub_at(vm, i) && free == GOGO_NET_MAX_SUBS) free = i;
    }
    if (free == GOGO_NET_MAX_SUBS) return false;
    ns->subs[free] = a->id;
    ns->sub_owed &= (uint8_t)~(1u << free);
    if (ns->is_up) notify_sub(vm, free, a);
    return true;
}

/* ---- the port-layer source ---- */

static bool s_pending(const Vm *vm, void *ctx) {
    const NetState *ns = ctx;
    (void)vm;
    return ns->rx_ready || ns->link_pending || ns->dirty || owed_due(ns) || tcp_due(ns) ||
           (ns->upper && ns->upper->pending(ns));
}

static void s_drain(Vm *vm, void *ctx, uint64_t now) {
    NetState *ns = ctx;
    (void)now;
    if (ns->link_pending) {
        ns->link_pending = 0;
        vm_barrier();          /* flag cleared before the state is read */
        bool up = ns->link_state != 0;
        vm_barrier();          /* link_state read before down_seq */
        uint32_t seq = ns->down_seq;
        if (seq != ns->down_seen) {
            vm_barrier();
            up = ns->link_state != 0;  /* re-read: the state of any Down seen is visible */
        }
        if (seq != ns->down_seen) {    /* a Down since the last drain, even if the link is up again */
            ns->down_seen = seq;
            if (up && ns->link_up) {
                ns->link_up = 0;
                report(vm);
            }
        }
        if (up && !ns->link_up) {
            ns->link_up = 1;
            resolve_gateway(ns);
        }
        ns->link_up = up;
        ns->dirty = 1;
    }
    if (ns->rx_ready) {
        vm_barrier();          /* flag read before the frame */
        net_rx_frame(ns, ns->rx, ns->rx_len);
        vm_barrier();          /* frame consumed before the slot is freed */
        ns->rx_ready = 0;
    }
    if (ns->dirty) report(vm);
    if (owed_due(ns)) retry_owed(vm);
    tcp_poll(vm);
    if (ns->upper && ns->upper->pending(ns)) ns->upper->drain(vm);
}

/* Orphan connections finish their close even with no actor left. */
static bool s_any_live(const Vm *vm, void *ctx) { (void)vm; return tcp_any_orphan(ctx); }

static bool s_next_deadline(const Vm *vm, void *ctx, uint64_t *deadline) {
    const NetState *ns = ctx;
    (void)vm;
    bool have = tcp_next_deadline(ns, deadline);
    uint64_t d;
    if (ns->upper && ns->upper->next_deadline && ns->upper->next_deadline(ns, &d) && (!have || d < *deadline)) {
        *deadline = d;
        have = true;
    }
    if (ns->sub_owed) {
        uint64_t now = vm_now(ns->vm);
        int32_t left = (int32_t)(ns->sub_retry_at - (uint32_t)now);
        d = left > 0 ? now + (uint32_t)left : now;
        if (!have || d < *deadline) *deadline = d;
        have = true;
    }
    return have;
}

static const PortSource net_source = { s_pending, s_drain, s_any_live, s_next_deadline, s_subscribe };

void net_install(Vm *vm, NetState *ns) {
    memset(ns, 0, sizeof *ns);
    udp_reset(ns);
    tcp_reset(ns);
    const NetIf *nif = vm->board->netif;
    ns->vm = vm;
    ns->nif = nif;
    vm->net = ns;
    bool up = nif && nif->mac(nif->ctx, ns->mac);
    /* The Wi-Fi task (net_rx_offer, net_link_event) reads `attached` and then
     * ns->vm: publish vm, nif and the MAC before it. */
    vm_barrier();
    ns->attached = up;
    ns->link_up = up && nif->link_up(nif->ctx);
    (void)port_add_source(vm, &net_source, ns);
}

void net_uninstall(Vm *vm) {
    NetState *ns = vm->net;
    if (!ns) return;
    /* Its ports first: their close needs vm->net (the tls port frees its
     * connection, TCP resets). */
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) {
        const Port *p = &vm->ports[i];
        const PortDriver *d = p->drv;
        if (d && (d == &net_netif_driver || d == &net_udp_driver || d == &net_tcp_driver ||
                  d == &net_tcp_listen_driver || (ns->upper && d == ns->upper->drv))) port_close(vm, p->id);
    }
    port_remove_source(vm, &net_source);
    ns->attached = 0;
    vm->net = NULL;
}

/* ---- the "netif" port ---- */

static int n_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    NetState *ns = vm->net;
    (void)index; (void)cfg;
    if (!ns || !ns->attached || n != 0u || ns->netif) return -1;
    ns->netif = p;
    return 0;
}

/* The address goes with the port: its owner's lease ends with it. */
static void n_close(struct Vm *vm, Port *p) {
    NetState *ns = vm->net;
    if (ns->netif != p) return;
    ns->netif = NULL;
    ns->ip = ns->mask = ns->gw = ns->dns = 0;
    ns->dirty = 1;
}

static bool n_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return true; }

/* SetAddr{IP, Mask, Gateway, DNS}, from the owner only. */
static void n_command(Vm *vm, const Actor *from, Port *p, term msg) {
    NetState *ns = vm->net;
    const uint32_t *o = obj_of(vm, from, msg);
    int32_t f[4];
    if (from->id != p->owner || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_NET_SETADDR ||
        hdr_words(o[0]) != 6u) return;
    for (uint32_t i = 0; i < 4; i++) {
        if (!int_val(vm, from, o[2 + i], &f[i])) return;
    }
    if (ns->ip && (uint32_t)f[0] != ns->ip) tcp_down(vm);   /* the address changes */
    ns->ip = (uint32_t)f[0];
    ns->mask = ns->ip ? (uint32_t)f[1] : 0u;
    ns->gw = ns->ip ? (uint32_t)f[2] : 0u;
    ns->dns = ns->ip ? (uint32_t)f[3] : 0u;
    resolve_gateway(ns);
    report(vm);
}

const PortDriver net_netif_driver = {
    PORT_NAME_NETIF, TYPE_NET_IFCONFIG, TYPE_NET_UP, n_open, NULL, n_close, n_can_event, NULL, n_command, NULL
};
