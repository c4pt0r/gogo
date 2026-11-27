/* The VM's run-time parameters (params.h). */
#include "params.h"
#include "actor.h"
#include "alloc.h"
#include "port.h"
#include <string.h>

void vm_params_default(VmParams *p) {
    memset(p, 0, sizeof *p);
    p->actor_memory = GOGO_DEFAULT_MAX_WORDS * 4u;
    p->max_actors = GOGO_TABLE_CAP;
    p->max_ports = GOGO_MAX_PORTS;
    p->max_links = GOGO_MAX_LINKS;
    p->code_mem = GOGO_CODE_MEM_DEFAULT;
    p->bytes_handles = GOGO_BYTES_HANDLES;
    p->bytes_per_actor = GOGO_BYTES_PER_ACTOR;
}

/* ---- text ---- */

static char *put_s(char *p, const char *s) {
    size_t n = strlen(s);
    memcpy(p, s, n);
    return p + n;
}

static char *put_u(char *p, uint64_t v) {
    char d[20];
    int n = 0;
    do d[n++] = (char)('0' + v % 10u); while ((v /= 10u) != 0);
    while (n) *p++ = d[--n];
    return p;
}

char *vm_params_put_size(char *p, uint64_t v) {
    for (uint32_t sh = 30; v && sh; sh -= 10u) {
        if (!(v & (((uint64_t)1 << sh) - 1u))) {
            p = put_u(p, v >> sh);
            *p++ = sh == 30u ? 'G' : sh == 20u ? 'M' : 'K';
            return p;
        }
    }
    return put_u(p, v);
}

/* "gogo: <k><mid><v> ...<tail>\n" on stream 2. */
static void say(const gogo_host *h, void *ctx, const char *k, const char *mid, const char *tail) {
    char m[96], *p = put_s(put_s(put_s(m, "gogo: "), k), mid);
    p = put_s(p, tail);
    *p++ = '\n';
    if (h && h->write) h->write(ctx, 2, m, (size_t)(p - m));
}

/* ---- keys ---- */

enum { P_INT, P_SIZE, P_PAIR };

/* Key k's value into v: P_INT decimal digits, P_SIZE digits with an
 * optional K, M or G (either case), P_PAIR "N" or "N/M" (v[1]: M, or 0).
 * Each number at most max. 0 if unset, 1 if set and good, -1 if set and
 * bad (said on the console). */
static int key_get(const gogo_host *h, void *ctx, const char *k, int kind, uint64_t max, uint64_t v[2]) {
    char b[32];
    if (!h || !h->config_get) return 0;
    int32_t n = h->config_get(ctx, (const uint8_t *)k, (uint32_t)strlen(k), (uint8_t *)b, sizeof b);
    if (n < 0) return 0;
    int32_t i = 0;
    bool ok = n <= (int32_t)sizeof b;
    v[1] = 0;
    for (int part = 0; part < 2 && ok; part++) {
        uint64_t x = 0;
        int32_t d0 = i;
        uint32_t d;
        while (i < n && i - d0 < 16 && (d = (uint8_t)b[i] - (uint32_t)'0') <= 9u) {
            x = x * 10u + d;
            i++;
        }
        ok = i > d0;
        if (ok && kind == P_SIZE && i + 1 == n) {
            uint32_t c = (uint8_t)(b[i] | 0x20), sh = c == 'k' ? 10u : c == 'm' ? 20u : c == 'g' ? 30u : 0u;
            ok = sh && x <= max >> sh;
            x <<= sh;
            i++;
        }
        ok = ok && x <= max;
        v[part] = x;
        if (kind != P_PAIR || i == n || b[i] != '/') break;
        i++;
    }
    if (!ok || i != n) {
        say(h, ctx, "bad ", k, "; using the default");
        return -1;
    }
    return 1;
}

/* A field: set's if nonzero, else the key's if set, good and (zero_ok or)
 * nonzero, else the default. */
static uint64_t pick(const gogo_host *h, void *ctx, const char *k, int kind, uint64_t max, uint64_t set,
                     uint64_t def, bool zero_ok) {
    uint64_t v[2];
    if (set) return set;
    return key_get(h, ctx, k, kind, max, v) > 0 && (v[0] || zero_ok) ? v[0] : def;
}

/* v within [lo, hi], else the nearer end, said:
 * "gogo: vm.actor_memory 2M is over this VM's 1G; using 1G". */
static uint64_t clamp(const gogo_host *h, void *ctx, const char *k, uint64_t v, uint64_t lo, uint64_t hi, bool size) {
    if (v >= lo && v <= hi) return v;
    uint64_t to = v < lo ? lo : hi;
    char a[24], c[24], t[40];
    *(size ? vm_params_put_size(a, v) : put_u(a, v)) = 0;
    *(size ? vm_params_put_size(c, to) : put_u(c, to)) = 0;
    *put_s(put_s(t, "; using "), c) = 0;
    char m[64];
    *put_s(put_s(put_s(put_s(m, " "), a), v < lo ? " is under " : " is over this VM's "), c) = 0;
    say(h, ctx, k, m, t);
    return to;
}

void vm_params_resolve(const gogo_host *h, void *ctx, const VmParams *set, VmParams *p) {
    static const VmParams none;
    const VmParams def = *p;
    uint64_t v[2] = { 0, 0 };
    if (!set) set = &none;
    uint64_t am = pick(h, ctx, "vm.actor_memory", P_SIZE, (uint64_t)1 << 40, set->actor_memory, def.actor_memory, false);
    if (am > GOGO_ACTOR_MAX_WORDS * 4u && am - GOGO_ACTOR_MAX_WORDS * 4u < 1024u) am = GOGO_ACTOR_MAX_WORDS * 4u;   /* a board's "256K" */
    p->actor_memory = (uint32_t)clamp(h, ctx, "vm.actor_memory", am, 1024u, GOGO_ACTOR_MAX_WORDS * 4u, true);
    p->memory = pick(h, ctx, "vm.memory", P_SIZE, (uint64_t)1 << 48, set->memory, def.memory, true);
    if (GOGO_ARENA_BYTES) p->memory = clamp(h, ctx, "vm.memory", p->memory, 0, GOGO_ARENA_BYTES, true);
    p->max_actors = (uint32_t)clamp(h, ctx, "vm.max_actors",
                                    pick(h, ctx, "vm.max_actors", P_INT, 0xFFFFFFFFu, set->max_actors,
                                         def.max_actors, false),
                                    1u, PORT_SLOT_TOP - GOGO_MAX_PORTS, false);
    /* Ports: a value given that does not fit (with the actors, in the
     * 65536 PID slots) is refused; the default then, or what the actors
     * leave. */
    p->max_ports = (uint32_t)pick(h, ctx, "vm.max_ports", P_INT, 0xFFFFFFFFu, set->max_ports, def.max_ports, false);
    if (p->max_ports > PORT_MAX_CEILING || p->max_actors > PORT_SLOT_TOP - p->max_ports) {
        say(h, ctx, "vm.max_ports refused (at most 2048, and actors + ports at most 65536)", "", "");
        p->max_ports = def.max_ports;
    }
    if (p->max_actors > PORT_SLOT_TOP - p->max_ports) p->max_ports = PORT_SLOT_TOP - p->max_actors;
    p->max_links = (uint32_t)clamp(h, ctx, "node.max_links",
                                   pick(h, ctx, "node.max_links", P_INT, 0xFFFFFFFFu, set->max_links, def.max_links,
                                        false),
                                   0, PORT_MAX_CEILING, false);
    p->code_mem = (uint32_t)pick(h, ctx, "code.mem", P_SIZE, 0xFFFFFFFFu, set->code_mem, def.code_mem, true);
    /* Bytes handles: N, or N/M (per actor; else the default's, at most N). */
    if (!set->bytes_handles && key_get(h, ctx, "vm.handles", P_PAIR, 0xFFFFFFFFu, v) > 0 && v[0]) {
        p->bytes_handles = (uint32_t)v[0];
        p->bytes_per_actor = (uint32_t)v[1];
    } else if (set->bytes_handles) {
        p->bytes_handles = set->bytes_handles;
        p->bytes_per_actor = set->bytes_per_actor;
    }
    p->bytes_handles = (uint32_t)clamp(h, ctx, "vm.handles", p->bytes_handles, 1u, GOGO_BYTES_HANDLES_MAX, false);
    if (!p->bytes_per_actor) p->bytes_per_actor = def.bytes_per_actor < p->bytes_handles ? def.bytes_per_actor : p->bytes_handles;
    p->bytes_per_actor =
        (uint32_t)clamp(h, ctx, "vm.handles per actor", p->bytes_per_actor, 1u, p->bytes_handles, false);
}

uint32_t vm_node_agent_bytes(const Vm *vm) {
    uint64_t want = (uint64_t)(vm->max_links ? vm->max_links : GOGO_MAX_LINKS) * NODE_AGENT_LINK_BYTES + NODE_AGENT_BASE;
    uint64_t am = (uint64_t)vm->actor_words * 4u;
    if (want < am) want = am;
    if (want < 262144u) want = 262144u;
    return want > GOGO_ACTOR_MAX_WORDS * 4u ? GOGO_ACTOR_MAX_WORDS * 4u : (uint32_t)want;
}

int vm_params_apply(Vm *vm, const VmParams *p) {
    vm->actor_words = (gogo_words_t)(p->actor_memory / 4u);
    va_set_limit(vm, p->memory > (size_t)-1 ? (size_t)-1 : (size_t)p->memory);
    vm->max_links = (uint16_t)p->max_links;
    vm->mod_cap = p->code_mem;
    vm->bytes_cap = (uint16_t)p->bytes_handles;
    vm->bytes_per_actor = (uint16_t)p->bytes_per_actor;
    return vm_set_max_ports(vm, p->max_ports);
}
