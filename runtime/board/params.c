/* The VM's run-time parameters (params.h): one table, one loop. */
#include "params.h"
#include "actor.h"
#include "alloc.h"
#include "port.h"
#include <string.h>

enum { F_SIZE = 1, F_ZERO = 2, F_WIDE = 4, F_PAIR = 8 };

/* A row per key, in PARAM_* order: the field, how its value reads, its
 * range (hi 0: none). */
typedef struct Row {
    char key[16];
    uint8_t off, flags;
    uint32_t lo, hi;
} Row;

#define ROW(k, f, fl, lo, hi) { k, (uint8_t)offsetof(VmParams, f), fl, lo, hi }
static const Row rows[PARAM_COUNT] = {
    ROW("vm.actor_memory", actor_memory, F_SIZE, 1024u, GOGO_ACTOR_MAX_WORDS * 4u),
    ROW("vm.memory", memory, F_SIZE | F_ZERO | F_WIDE, 0, GOGO_ARENA_BYTES),
    ROW("vm.max_actors", max_actors, 0, 1u, PORT_SLOT_TOP - GOGO_MAX_PORTS),
    ROW("vm.max_ports", max_ports, 0, 1u, PORT_MAX_CEILING),
    ROW("node.max_links", max_links, 0, 0, PORT_MAX_CEILING),
    ROW("code.mem", code_mem, F_SIZE | F_ZERO, 0, 0xFFFFFFFFu),
    ROW("vm.handles", bytes_handles, F_PAIR, 1u, GOGO_BYTES_HANDLES_MAX),
};

void vm_params_default(VmParams *p) {
    static const VmParams d = { 0, GOGO_DEFAULT_MAX_WORDS * 4u, GOGO_TABLE_CAP, GOGO_MAX_PORTS, GOGO_MAX_LINKS,
                                GOGO_CODE_MEM_DEFAULT, GOGO_BYTES_HANDLES, GOGO_BYTES_PER_ACTOR };
    *p = d;
}

/* ---- text ---- */

static char *put_s(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

/* v in decimal, or with a K, M or G when it divides evenly (size). */
static char *put_num(char *p, params_num v, bool size) {
    char d[20], suf = 0;
    int n = 0;
    for (uint32_t sh = 30; size && v && sh; sh -= 10u) {
        if (!(v & (((params_num)1 << sh) - 1u))) {
            v >>= sh;
            suf = "KMG"[sh / 10u - 1u];
            break;
        }
    }
    do d[n++] = (char)('0' + v % 10u); while ((v /= 10u) != 0);
    while (n) *p++ = d[--n];
    if (suf) *p++ = suf;
    return p;
}

/* The longest part clamp builds: " " + a value as typed (at most 31: the
 * key buffer) + " is over this VM's " (19) + a number (at most 20) +
 * "; using " (8) + a number (20) + NUL. */
#define CLAMP_MAX (1 + 31 + 19 + 20 + 8 + 20 + 1)

/* "gogo: <a><b><c>\n" on stream 2. The longest line: "gogo: " (6), a
 * of at most 20 ("vm.handles per actor"), b of at most CLAMP_MAX - 1, c of
 * at most 32 ("; using the default", ": actors + ports at most 65536"),
 * the newline: under 160. */
static void say(const gogo_host *h, void *ctx, const char *a, const char *b, const char *c) {
    char m[6 + 20 + CLAMP_MAX + 32 + 1], *p = put_s(put_s(put_s(put_s(m, "gogo: "), a), b), c);
    *p++ = '\n';
    if (h && h->write) h->write(ctx, 2, m, (size_t)(p - m));
}

/* v (t: as typed) within [lo, hi] (hi 0: no top), else the nearer end,
 * said: "gogo: vm.actor_memory 2G is over this VM's 1G; using 1G". */
static params_num clamp(const gogo_host *h, void *ctx, const char *k, const char *t, params_num v, params_num lo,
                        params_num hi, bool size) {
    if (v >= lo && (!hi || v <= hi)) return v;
    params_num to = v < lo ? lo : hi;
    char m[CLAMP_MAX], *p = put_s(put_s(put_s(m, " "), t), v < lo ? " is under " : " is over this VM's ");
    p = put_s(put_num(p, to, size), "; using ");
    *put_num(p, to, size) = 0;
    say(h, ctx, k, m, "");
    return to;
}

uint32_t params_parse(const char *s, uint32_t n, bool size, params_num *out) {
    params_num x = 0;
    uint32_t i = 0, d;
    while (i < n && (d = (uint8_t)s[i] - (uint32_t)'0') <= 9u) {
        x = x > (PARAMS_NUM_TOP - d) / 10u ? PARAMS_NUM_TOP : x * 10u + d;
        i++;
    }
    if (i && size && i < n) {
        uint32_t c = (uint8_t)(s[i] | 0x20), sh = c == 'k' ? 10u : c == 'm' ? 20u : c == 'g' ? 30u : 0u;
        if (sh) {
            x = x > PARAMS_NUM_TOP >> sh ? PARAMS_NUM_TOP : x << sh;
            i++;
        }
    }
    *out = x;
    return i;
}

/* ---- resolving ---- */

void vm_params_resolve(const gogo_host *h, void *ctx, VmParams *p) {
    uint32_t def_per = p->bytes_per_actor, per = 0;
    bool ports_given = false;
    char b[32];
    for (uint32_t r = 0; r < PARAM_COUNT; r++) {
        const Row *row = &rows[r];
        uint32_t fl = row->flags, i, j = 1;
        uint32_t *f = (uint32_t *)(void *)((char *)p + row->off);
        params_num v = 0, m = 0;
        int32_t n = h && h->config_get ? h->config_get(ctx, (const uint8_t *)row->key, (uint32_t)strlen(row->key),
                                                       (uint8_t *)b, sizeof b - 1u)
                                       : -1;
        if (n < 0) continue;                        /* unset: the default stays */
        uint32_t len = n < (int32_t)sizeof b ? (uint32_t)n : 0u;
        b[len] = 0;
        i = params_parse(b, len, fl & F_SIZE, &v);
        if ((fl & F_PAIR) && i < len && b[i] == '/') {   /* N/M; "/M" (gogo_config: M alone) keeps N */
            j = params_parse(b + i + 1u, len - i - 1u, false, &m);
            if (!i) v = *f;
            i += 1u + j;
            if (!j || !m) j = 0;
        } else if (!i) {
            j = 0;
        }
        if (!j || i != len || (v >= PARAMS_NUM_TOP && !row->hi) || m > 0xFFFFu || (row->hi == 0xFFFFFFFFu && (v > row->hi || v >= PARAMS_NUM_TOP))) {
            say(h, ctx, "bad ", row->key, "; using the default");
            continue;
        }
        if (!v && !(fl & F_ZERO)) continue;        /* 0: the default */
        if (r == PARAM_ACTOR_MEMORY && v > row->hi && v - row->hi < 1024u) v = row->hi;   /* a board's "256K" */
        v = clamp(h, ctx, row->key, b, v, row->lo, row->hi, fl & F_SIZE);
        if (fl & F_WIDE) p->memory = v;
        else *f = (uint32_t)v;
        if (r == PARAM_MAX_PORTS) ports_given = true;
        if (m) per = (uint32_t)m;
    }
    /* Ports and actors share the 65536 PID slots: ports given past what
     * the actors leave are cut, said; the default is cut silently. */
    if (p->max_actors > PORT_SLOT_TOP - p->max_ports) {
        p->max_ports = PORT_SLOT_TOP - p->max_actors;
        if (ports_given) {
            char t[24];
            *put_num(t, p->max_ports, false) = 0;
            say(h, ctx, "vm.max_ports cut to ", t, ": actors + ports at most 65536");
        }
    }
    if (per) {
        *put_num(b, per, false) = 0;
        p->bytes_per_actor = (uint32_t)clamp(h, ctx, "vm.handles per actor", b, per, 1u, p->bytes_handles, false);
    } else {
        p->bytes_per_actor = def_per < p->bytes_handles ? def_per : p->bytes_handles;
    }
}

int32_t params_overlay(const char *const text[PARAM_COUNT], const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    for (uint32_t r = 0; r < PARAM_COUNT; r++) {
        if (!text[r] || strlen(rows[r].key) != kl || memcmp(rows[r].key, k, kl) != 0) continue;
        uint32_t n = (uint32_t)strlen(text[r]);
        if (buf) memcpy(buf, text[r], n < cap ? n : cap);
        return (int32_t)n;
    }
    return -1;
}

size_t params_changed(const VmParams *a, const VmParams *b, char *buf, size_t cap) {
    char line[128], *q = line;
    for (uint32_t r = 0; r < PARAM_COUNT; r++) {
        uint32_t o = rows[r].off, w = (rows[r].flags & F_WIDE) ? 8u : (rows[r].flags & F_PAIR) ? 8u : 4u;
        if (memcmp((const char *)a + o, (const char *)b + o, w) == 0) continue;
        if (q != line) *q++ = ' ';
        q = put_s(q, rows[r].key);
    }
    size_t n = (size_t)(q - line);
    if (cap) {
        size_t k = n < cap ? n : cap - 1u;
        memcpy(buf, line, k);
        buf[k] = 0;
    }
    return n;
}

uint32_t params_node_agent_bytes(const VmParams *p) {
    uint64_t want = (uint64_t)(p->max_links ? p->max_links : GOGO_MAX_LINKS) * NODE_AGENT_LINK_BYTES + NODE_AGENT_BASE;
    uint64_t am = p->actor_memory;
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

size_t vm_params_format(const VmParams *p, char *buf, size_t cap) {
    char line[200], *q = line;
    uint64_t mem = p->memory;
    if (GOGO_ARENA_BYTES && (!mem || mem > GOGO_ARENA_BYTES)) mem = GOGO_ARENA_BYTES;
    q = put_num(put_s(q, "actor_memory="), p->actor_memory, true);
    q = put_s(q, " memory=");
    q = mem ? put_num(q, (params_num)mem, true) : put_s(q, "unlimited");
    q = put_num(put_s(q, " max_actors="), p->max_actors, false);
    q = put_num(put_s(q, " max_ports="), p->max_ports, false);
    q = put_num(put_s(q, " max_links="), p->max_links ? p->max_links : GOGO_MAX_LINKS, false);
    q = put_num(put_s(q, " code_mem="), p->code_mem, true);
    q = put_num(put_s(q, " bytes_handles="), p->bytes_handles, false);
    *q++ = '/';
    q = put_num(q, p->bytes_per_actor, false);
    size_t n = (size_t)(q - line);
    if (cap) {
        size_t k = n < cap ? n : cap - 1u;
        memcpy(buf, line, k);
        buf[k] = 0;
    }
    return n;
}
