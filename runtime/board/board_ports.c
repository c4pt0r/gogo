/* Board ports (board_ports.h): the GOGO_BOARD_PORT entries of a build,
 * registered at boot with the host-port layer. */
#include "board_ports.h"
#include "ports.h"
#include <string.h>
#include <stdio.h>

/* What GOGO_BOARD_PORT refers to, so a registration in a build without
 * this layer fails to link instead of being silently left out. */
const char gogo_board_ports_enabled = 1;

/* This layer's own entry (NULL, skipped): the section exists in every
 * build, so its bounds are always defined. */
static const gogo_board_port *const no_port GOGO_BOARD_PORT_ATTR = NULL;

#if defined(__APPLE__)
extern const gogo_board_port *const bp_start[] __asm__("section$start$__DATA$gogo_bport");
extern const gogo_board_port *const bp_stop[] __asm__("section$end$__DATA$gogo_bport");
#elif defined(ESP_PLATFORM)
/* linker.lf: SURROUND(gogo_board_ports) */
extern const gogo_board_port *const _gogo_board_ports_start[];
extern const gogo_board_port *const _gogo_board_ports_end[];
#define bp_start _gogo_board_ports_start
#define bp_stop _gogo_board_ports_end
#else
extern const gogo_board_port *const __start_gogo_board_port[];
extern const gogo_board_port *const __stop_gogo_board_port[];
#define bp_start __start_gogo_board_port
#define bp_stop __stop_gogo_board_port
#endif

/* gogo_board_port's first size, which never changes. */
#define ENTRY_SIZE_V1 (offsetof(gogo_board_port, ctx) + sizeof(void *))

static size_t entries(void) {
    (void)no_port;
    return (size_t)(bp_stop - bp_start);
}

static const char *name_of(size_t i) { return bp_start[i]->name ? bp_start[i]->name : ""; }

/* True if entry i comes before entry j: by name, then in link order. */
static bool before(size_t i, size_t j) {
    int c = strcmp(name_of(i), name_of(j));
    return c < 0 || (c == 0 && i < j);
}

/* The entry after entry prev (SIZE_MAX: the first) in registration
 * order, or SIZE_MAX after the last. A handful of entries: a scan each. */
static size_t next_entry(size_t prev) {
    size_t n = entries(), best = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        if (!bp_start[i] || (prev != SIZE_MAX && !before(prev, i))) continue;
        if (best == SIZE_MAX || before(i, best)) best = i;
    }
    return best;
}

/* A verdict of its own: another entry has the same name. */
#define BP_TWICE 1

/* True if an entry other than i has i's name. */
static bool named_twice(size_t i) {
    size_t n = entries();
    if (!bp_start[i]->name) return false;
    for (size_t k = 0; k < n; k++)
        if (k != i && bp_start[k] && bp_start[k]->name && !strcmp(bp_start[k]->name, bp_start[i]->name)) return true;
    return false;
}

/* Whether gogo_register_port takes entry e on board b, with the names
 * accepted before it: 0, or the GOGO_E* it returns, or BP_TWICE (no
 * entry of a name two have is taken: which one would win is the link
 * order's, which no driver controls). */
static int verdict(const VmBoard *b, const gogo_board_port *e, const char *const *acc, uint32_t na) {
    const gogo_port_driver *d = e->driver;
    if (e->size < ENTRY_SIZE_V1 || e->reserved || !e->name || !e->name[0] || !d || d->size < EMBED_DRIVER_SIZE_V1 ||
        d->reserved)
        return GOGO_EINVAL;
    if (strlen(e->name) > GOGO_PORT_NAME_MAX) return GOGO_E2BIG;
    if (embed_name_reserved(b->ports, e->name)) return GOGO_EEXIST;
    for (uint32_t k = 0; k < na; k++)
        if (!strcmp(acc[k], e->name)) return GOGO_EEXIST;
    uint32_t nb = 0;
    for (const PortDriver *const *p = b->ports; p && *p; p++) nb++;
    if (na == EMBED_HOST_PORTS || nb > EMBED_BOARD_PORTS_MAX) return GOGO_EFULL;
    return 0;
}

typedef void (*EachFn)(void *ctx, const gogo_board_port *e, int v);

/* Calls fn for every entry in registration order with its verdict. */
static void each(const VmBoard *b, EachFn fn, void *ctx) {
    const char *acc[EMBED_HOST_PORTS];
    uint32_t na = 0;
    for (size_t i = next_entry(SIZE_MAX); i != SIZE_MAX; i = next_entry(i)) {
        const gogo_board_port *e = bp_start[i];
        int v = verdict(b, e, acc, na);
        if (v == 0 && named_twice(i)) v = BP_TWICE;
        if (v == 0) acc[na++] = e->name;
        fn(ctx, e, v);
    }
}

typedef struct Names {
    const char **out;
    uint32_t cap, n;
} Names;

static void put_name(void *ctx, const gogo_board_port *e, int v) {
    Names *s = ctx;
    if (v) return;
    if (s->n < s->cap) s->out[s->n] = e->name;
    s->n++;
}

uint32_t board_ports_names(const VmBoard *b, const char **out, uint32_t cap) {
    Names s = { out, cap, 0 };
    each(b, put_name, &s);
    return s.n;
}

struct EmbedPorts *board_ports_new(const gogo_host *h, const gogo_host *ph) {
    if (next_entry(SIZE_MAX) == SIZE_MAX) return NULL;
    EmbedPorts *ep = ph->alloc(NULL, sizeof *ep);
    if (!ep) {
        static const char m[] = "gogo: out of memory for the board ports; none registered\n";
        h->write(NULL, 2, m, sizeof m - 1u);
        return NULL;
    }
    memset(ep, 0, sizeof *ep);
    ep->uh = ph;
    ep->refs = 1;                /* the board's, never dropped */
    ep->board = true;
    return ep;
}

typedef struct Reg {
    EmbedPorts *ep;
    const gogo_host *h;
} Reg;

/* Registers e (v: its verdict) or says why not. */
static void reg_one(void *ctx, const gogo_board_port *e, int v) {
    Reg *r = ctx;
    if (v == 0) v = gogo_register_port(embed_handle(r->ep), e->name, strlen(e->name), e->driver, e->ctx);
    if (v == 0) return;
    char m[200];
    const char *why = v == BP_TWICE   ? "two board ports have this name (none of them is registered)"
                      : v == GOGO_EEXIST ? "the name is taken (by a driver of the runtime or the board)"
                      : v == GOGO_E2BIG ? "the name is too long"
                      : v == GOGO_EFULL ? "too many board ports"
                      : v == GOGO_EINVAL ? "a bad entry (its size, name or driver)"
                                         : "refused";
    int n = snprintf(m, sizeof m, "gogo: board port \"%.40s\" not registered: %s\n", e->name ? e->name : "", why);
    if (n > 0) r->h->write(NULL, 2, m, (size_t)n < sizeof m ? (size_t)n : sizeof m - 1u);
}

int board_ports_attach(struct EmbedPorts *ep, struct Vm *vm, const gogo_host *h) {
    ep->vm = vm;
    ep->closed_msg = NULL;       /* vm_init starts the allocator over */
    ep->closed_n = 0;
    if (!ep->registered) {
        ep->registered = true;
        Reg r = { ep, h };
        each(vm->board, reg_one, &r);
    }
    if (!ep->n_host_ports) return 0;   /* none taken: the board as it is */
    embed_ports_install(ep);
    return port_add_source(vm, &inbox_source, ep) ? 0 : -1;
}

void board_ports_loaded(struct EmbedPorts *ep) { ep->loaded = true; }

void board_ports_end(struct EmbedPorts *ep) {
    embed_set_state(ep, EMBED_CLOSED);
    inbox_clear(&ep->inbox, ep->uh, ep->uctx);
}
