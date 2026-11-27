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

static void say(const gogo_host *h, void *ctx, const char *s, size_t n) {
    if (h && h->write) h->write(ctx, 2, s, n);
}

/* ---- keys ---- */

enum { P_INT, P_SIZE };

/* Key k's value as a number into *v: P_INT decimal digits, P_SIZE digits
 * with an optional K, M or G (either case). 0 if unset, 1 if set and good,
 * -1 if set and bad (said on the console). */
static int key_num(const gogo_host *h, void *ctx, const char *k, int kind, uint64_t *v) {
    char b[24];
    if (!h || !h->config_get) return 0;
    int32_t n = h->config_get(ctx, (const uint8_t *)k, (uint32_t)strlen(k), (uint8_t *)b, sizeof b);
    if (n < 0) return 0;
    uint64_t x = 0;
    int32_t i = 0;
    uint32_t d;
    if (n <= 20) {   /* longer than any good value: bad */
        while (i < n && i < 12 && (d = (uint8_t)b[i] - (uint32_t)'0') <= 9u) { x = x * 10u + d; i++; }
        if (kind == P_SIZE && i > 0 && i + 1 == n) {
            uint32_t c = (uint8_t)(b[i] | 0x20), sh = c == 'k' ? 10u : c == 'm' ? 20u : c == 'g' ? 30u : 0u;
            x <<= sh;
            i += sh != 0;
        }
    }
    if (n > 20 || i == 0 || i != n || x > 0xFFFFFFFFu) {   /* 12 digits << 30: no overflow before this check */
        char m[64], *p = put_s(m, "gogo: bad ");
        p = put_s(p, k);
        p = put_s(p, "; using the default\n");
        say(h, ctx, m, (size_t)(p - m));
        return -1;
    }
    *v = x;
    return 1;
}

/* One uint32 field: set's if nonzero, else the key's if good. */
static uint32_t pick32(const gogo_host *h, void *ctx, const char *k, int kind, uint32_t set, uint32_t def) {
    uint64_t v;
    if (set) return set;
    return key_num(h, ctx, k, kind, &v) > 0 ? (uint32_t)v : def;
}

void vm_params_resolve(const gogo_host *h, void *ctx, const VmParams *set, VmParams *p) {
    static const VmParams none;
    const VmParams def = *p;
    if (!set) set = &none;
    p->max_ports = pick32(h, ctx, "vm.max_ports", P_INT, set->max_ports, def.max_ports);
    p->max_links = pick32(h, ctx, "node.max_links", P_INT, set->max_links, def.max_links);
    p->code_mem = pick32(h, ctx, "code.mem", P_SIZE, set->code_mem, def.code_mem);
    if (!p->max_ports) p->max_ports = def.max_ports;   /* 0: the default */
    if ( p->max_ports > PORT_MAX_CEILING || p->max_actors > PORT_SLOT_TOP - p->max_ports) {
        static const char m[] = "gogo: vm.max_ports refused (at most 2048, and actors + ports at most 65536)\n";
        say(h, ctx, m, sizeof m - 1u);
        p->max_ports = def.max_ports;
    }
    if (p->max_links > PORT_MAX_CEILING) {
        static const char m[] = "gogo: node.max_links is over 2048; using 2048\n";
        say(h, ctx, m, sizeof m - 1u);
        p->max_links = PORT_MAX_CEILING;
    }
}

int vm_params_apply(Vm *vm, const VmParams *p) {
    vm->max_links = (uint16_t)p->max_links;
    vm->mod_cap = p->code_mem;
    return vm_set_max_ports(vm, p->max_ports);
}
