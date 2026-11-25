/* The stack's state, its port-layer source (RX and link slots, Up/Down
 * reports), the "netif" port and net.Subscribe. */
#include "net_int.h"
#include "heap.h"
#include "alloc.h"
#include "opcodes.h"
#include "platform.h"
#include "signals.h"
#include <string.h>

NetState net_s;

/* Fixed state within budget, on every target. */
typedef char net_fixed_state_fits[sizeof(NetState) <= NET_FIXED_LIMIT ? 1 : -1];
typedef char net_udp_pcb_fits[sizeof(UdpPcb) <= 32u ? 1 : -1];
typedef char net_tcp_pcb_fits[sizeof(TcpPcb) <= 64u ? 1 : -1];

uint32_t net_fixed_bytes(void) { return (uint32_t)sizeof(NetState); }
const NetStats *net_stats(void) { return &net_s.st; }

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
    int32_t v[3] = { (int32_t)net_s.up_ip, (int32_t)net_s.up_gw, (int32_t)net_s.up_dns };
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
    if (!net_s.subs[i]) return NULL;
    Actor *a = actor_lookup(vm, net_s.subs[i]);
    if (!a || a->state == A_EXITED || (a->flags & AF_EXIT_PENDING)) {
        net_s.subs[i] = 0;
        return NULL;
    }
    return a;
}

/* Sends subscriber i the current state, or owes it (retried by the
 * source's drain). */
static void notify_sub(Vm *vm, uint32_t i, Actor *a) {
    uint8_t bit = (uint8_t)(1u << i);
    if (notify(vm, a, net_s.is_up)) {
        net_s.sub_owed &= (uint8_t)~bit;
        return;
    }
    if (!net_s.sub_owed) net_s.sub_retry_at = (uint32_t)platform_now_ms() + NOTIFY_RETRY_MS;
    net_s.sub_owed |= bit;
}

static bool owed_due(void) {
    return net_s.sub_owed && due(net_s.sub_retry_at, (uint32_t)platform_now_ms());
}

static void retry_owed(Vm *vm) {
    uint8_t owed = net_s.sub_owed;
    net_s.sub_owed = 0;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        if (!(owed & (1u << i))) continue;
        Actor *a = sub_at(vm, i);
        if (a) notify_sub(vm, i, a);
    }
}

/* Reports a change of the interface state to every subscriber. */
static void report(Vm *vm) {
    net_s.dirty = 0;
    bool up = net_s.link_up && ip_configured();
    if (up) {
        if (net_s.is_up && net_s.up_ip == net_s.ip && net_s.up_gw == net_s.gw && net_s.up_dns == net_s.dns) return;
        net_s.up_ip = net_s.ip;
        net_s.up_gw = net_s.gw;
        net_s.up_dns = net_s.dns;
    } else if (!net_s.is_up) {
        return;
    }
    net_s.is_up = up;
    if (!up) tcp_down(vm);
    platform_net_report(up, up ? net_s.up_ip : 0u, up ? net_s.up_gw : 0u, up ? net_s.up_dns : 0u);
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        Actor *a = sub_at(vm, i);
        if (a) notify_sub(vm, i, a);
    }
}

/* Pre-resolves the gateway, so the first packet routed through it is not
 * dropped for an ARP miss. */
static void resolve_gateway(void) {
    if (net_s.link_up && net_s.gw && ip_on_link(net_s.gw)) arp_request(net_s.gw);
}

static bool s_subscribe(Vm *vm, Actor *a) {
    uint32_t free = GOGO_NET_MAX_SUBS;
    for (uint32_t i = 0; i < GOGO_NET_MAX_SUBS; i++) {
        if (net_s.subs[i] == a->id) return true;
        if (!sub_at(vm, i) && free == GOGO_NET_MAX_SUBS) free = i;
    }
    if (free == GOGO_NET_MAX_SUBS) return false;
    net_s.subs[free] = a->id;
    net_s.sub_owed &= (uint8_t)~(1u << free);
    if (net_s.is_up) notify_sub(vm, free, a);
    return true;
}

/* ---- the port-layer source ---- */

static bool s_pending(void) {
    return net_s.rx_ready || net_s.link_pending || net_s.dirty || owed_due() || tcp_due() ||
           (net_s.upper && net_s.upper->pending());
}

static void s_drain(Vm *vm, uint64_t now) {
    (void)now;
    if (net_s.link_pending) {
        net_s.link_pending = 0;
        platform_barrier();          /* flag cleared before the state is read */
        bool up = net_s.link_state != 0;
        platform_barrier();          /* link_state read before down_seq */
        uint32_t seq = net_s.down_seq;
        if (seq != net_s.down_seen) {
            platform_barrier();
            up = net_s.link_state != 0;  /* re-read: the state of any Down seen is visible */
        }
        if (seq != net_s.down_seen) {    /* a Down since the last drain, even if the link is up again */
            net_s.down_seen = seq;
            if (up && net_s.link_up) {
                net_s.link_up = 0;
                report(vm);
            }
        }
        if (up && !net_s.link_up) {
            net_s.link_up = 1;
            resolve_gateway();
        }
        net_s.link_up = up;
        net_s.dirty = 1;
    }
    if (net_s.rx_ready) {
        platform_barrier();          /* flag read before the frame */
        net_rx_frame(vm, net_s.rx, net_s.rx_len);
        platform_barrier();          /* frame consumed before the slot is freed */
        net_s.rx_ready = 0;
    }
    if (net_s.dirty) report(vm);
    if (owed_due()) retry_owed(vm);
    tcp_poll(vm);
    if (net_s.upper && net_s.upper->pending()) net_s.upper->drain(vm);
}

/* Orphan connections finish their close even with no actor left. */
static bool s_any_live(const Vm *vm) { (void)vm; return tcp_any_orphan(); }

static bool s_next_deadline(const Vm *vm, uint64_t *deadline) {
    (void)vm;
    bool have = tcp_next_deadline(deadline);
    uint64_t d;
    if (net_s.upper && net_s.upper->next_deadline && net_s.upper->next_deadline(&d) && (!have || d < *deadline)) {
        *deadline = d;
        have = true;
    }
    if (net_s.sub_owed) {
        uint64_t now = platform_now_ms();
        int32_t left = (int32_t)(net_s.sub_retry_at - (uint32_t)now);
        d = left > 0 ? now + (uint32_t)left : now;
        if (!have || d < *deadline) *deadline = d;
        have = true;
    }
    return have;
}

static const PortSource net_source = { s_pending, s_drain, s_any_live, s_next_deadline, s_subscribe };

void net_attach(void) {
    memset(&net_s, 0, sizeof net_s);
    udp_reset();
    tcp_reset();
    net_s.attached = platform_net_mac(net_s.mac);
    net_s.link_up = net_s.attached && platform_net_link_up();
    (void)port_add_source(&net_source);
}

void net_detach(void) {
    port_remove_source(&net_source);
    net_s.attached = 0;
}

/* ---- the "netif" port ---- */

static int n_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)index; (void)cfg;
    if (!net_s.attached || n != 0u || net_s.netif) return -1;
    net_s.netif = p;
    return 0;
}

/* The address goes with the port: its owner's lease ends with it. */
static void n_close(struct Vm *vm, Port *p) {
    (void)vm;
    if (net_s.netif != p) return;
    net_s.netif = NULL;
    net_s.ip = net_s.mask = net_s.gw = net_s.dns = 0;
    net_s.dirty = 1;
}

static bool n_can_event(const Port *p) { (void)p; return true; }

/* SetAddr{IP, Mask, Gateway, DNS}, from the owner only. */
static void n_command(Vm *vm, const Actor *from, Port *p, term msg) {
    const uint32_t *o = obj_of(vm, from, msg);
    int32_t f[4];
    if (from->id != p->owner || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_NET_SETADDR ||
        hdr_words(o[0]) != 6u) return;
    for (uint32_t i = 0; i < 4; i++) {
        if (!int_val(vm, from, o[2 + i], &f[i])) return;
    }
    if (net_s.ip && (uint32_t)f[0] != net_s.ip) tcp_down(vm);   /* the address changes */
    net_s.ip = (uint32_t)f[0];
    net_s.mask = net_s.ip ? (uint32_t)f[1] : 0u;
    net_s.gw = net_s.ip ? (uint32_t)f[2] : 0u;
    net_s.dns = net_s.ip ? (uint32_t)f[3] : 0u;
    resolve_gateway();
    report(vm);
}

const PortDriver net_netif_driver = {
    "netif", TYPE_NET_IFCONFIG, TYPE_NET_UP, n_open, NULL, n_close, n_can_event, NULL, n_command, NULL
};
