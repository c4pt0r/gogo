/* The VM's parameters (params.h): precedence, the checks, apply. */
#include "test.h"
#include "test_host.h"
#include "params.h"
#include "actor.h"
#include "port.h"
#include <string.h>

/* A host whose config store is `keys` ("k=v\n" lines) and whose stream 2
 * goes to `said`. */
static const char *keys;
static const char *flags[PARAM_COUNT];   /* a platform's flags (params_overlay) */
static char said[1024];
static size_t said_n;

static int32_t kv_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    int32_t f = params_overlay(flags, k, kl, buf, cap);
    if (f >= 0) return f;
    for (const char *p = keys; p && *p;) {
        const char *e = strchr(p, '\n'), *eq = strchr(p, '=');
        if (!e) e = p + strlen(p);
        if (eq && eq < e && (uint32_t)(eq - p) == kl && memcmp(p, k, kl) == 0) {
            uint32_t n = (uint32_t)(e - eq - 1);
            if (buf) memcpy(buf, eq + 1, n < cap ? n : cap);
            return (int32_t)n;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

static void kv_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    if (stream == 2 && said_n + n < sizeof said) {
        memcpy(said + said_n, s, n);
        said_n += n;
        said[said_n] = 0;
    }
}

static const gogo_host kv_host = { .size = sizeof(gogo_host), .write = kv_write, .config_get = kv_get };

/* p resolved from the defaults with keys k and the flags in `flags`
 * (cleared after). */
static VmParams resolve(const char *k, const void *unused) {
    VmParams p;
    (void)unused;
    keys = k;
    said_n = 0;
    said[0] = 0;
    vm_params_default(&p);
    vm_params_resolve(&kv_host, NULL, &p);
    memset(flags, 0, sizeof flags);
    return p;
}

/* Defaults; a key; a flag over the key; K / M / G; bad values said and
 * ignored; out of range clamped and said. */
TEST(params_precedence) {
    VmParams d, p;
    vm_params_default(&d);
    p = resolve(NULL, NULL);
    CHECK(memcmp(&p, &d, sizeof p) == 0);
    CHECK_EQ(said_n, 0);
    CHECK_EQ(d.actor_memory, GOGO_DEFAULT_MAX_WORDS * 4u);
    CHECK_EQ(d.code_mem, GOGO_CODE_MEM_DEFAULT);

    p = resolve("vm.actor_memory=8K\nvm.max_actors=100\nvm.memory=96k\nvm.max_ports=8\nnode.max_links=3\n"
                "code.mem=1M\nvm.handles=20/5\n", NULL);
    CHECK_EQ(p.actor_memory, 8192);
    CHECK_EQ(p.max_actors, 100);
    CHECK_EQ(p.memory, 96u * 1024u);
    CHECK_EQ(p.max_ports, 8);
    CHECK_EQ(p.max_links, 3);
    CHECK_EQ(p.code_mem, 1048576);
    CHECK_EQ(p.bytes_handles, 20);
    CHECK_EQ(p.bytes_per_actor, 5);
    CHECK_EQ(said_n, 0);

    flags[PARAM_ACTOR_MEMORY] = "12K";
    flags[PARAM_MAX_ACTORS] = "50";
    flags[PARAM_HANDLES] = "10";
    p = resolve("vm.actor_memory=8K\nvm.max_actors=100\nvm.handles=20/5\n", NULL);
    CHECK_EQ(p.actor_memory, 12288);
    CHECK_EQ(p.max_actors, 50);
    CHECK_EQ(p.bytes_handles, 10);   /* the flag's, its per-actor the default's at most 10 */
    CHECK_EQ(p.bytes_per_actor, d.bytes_per_actor < 10u ? d.bytes_per_actor : 10u);

    p = resolve("vm.actor_memory=lots\nvm.max_actors=-3\nvm.handles=4/\n", NULL);
    CHECK_EQ(p.actor_memory, d.actor_memory);
    CHECK_EQ(p.max_actors, d.max_actors);
    CHECK_EQ(p.bytes_handles, d.bytes_handles);
    CHECK(strstr(said, "gogo: bad vm.actor_memory; using the default\n") != NULL);
    CHECK(strstr(said, "gogo: bad vm.max_actors; using the default\n") != NULL);
    CHECK(strstr(said, "gogo: bad vm.handles; using the default\n") != NULL);

    /* 0 is the default (a limit of none for vm.memory). */
    p = resolve("vm.actor_memory=0\nvm.memory=0\n", NULL);
    CHECK_EQ(p.actor_memory, d.actor_memory);
    CHECK_EQ(p.memory, 0);
    CHECK_EQ(said_n, 0);
}

TEST(params_ceilings) {
    VmParams d, p;
    vm_params_default(&d);
    p = resolve("vm.actor_memory=4G\nvm.max_actors=70000\nnode.max_links=5000\nvm.handles=70000/3\n", NULL);
    CHECK_EQ(p.actor_memory, GOGO_ACTOR_MAX_WORDS * 4u);
    CHECK(strstr(said, "gogo: vm.actor_memory 4G is over this VM's ") != NULL);
    CHECK_EQ(p.max_actors, PORT_SLOT_TOP - GOGO_MAX_PORTS);
    CHECK(strstr(said, "gogo: vm.max_actors 70000 is over this VM's 65520; using 65520\n") != NULL);
    CHECK_EQ(p.max_links, PORT_MAX_CEILING);
    CHECK_EQ(p.bytes_handles, GOGO_BYTES_HANDLES_MAX);
    CHECK_EQ(p.bytes_per_actor, 3);
    /* The actors take the PID slots the ports would: what is left. */
    CHECK_EQ(p.max_ports, PORT_SLOT_TOP - p.max_actors);
    p = resolve("vm.actor_memory=100\nvm.handles=8/9\n", NULL);
    CHECK_EQ(p.actor_memory, 1024);
    CHECK(strstr(said, "gogo: vm.actor_memory 100 is under 1K; using 1K\n") != NULL);
    CHECK_EQ(p.bytes_per_actor, 8);
    /* Ports past 2048 are cut to it; past what the actors leave, cut
     * further and said, the default silently. */
    p = resolve("vm.max_ports=4000\n", NULL);
    CHECK_EQ(p.max_ports, PORT_MAX_CEILING);
    CHECK(strstr(said, "gogo: vm.max_ports 4000 is over this VM's 2048; using 2048\n") != NULL);
    p = resolve("vm.max_actors=65000\nvm.max_ports=2048\n", NULL);
    CHECK_EQ(p.max_ports, 536);
    CHECK(strstr(said, "gogo: vm.max_ports cut to 536: actors + ports at most 65536\n") != NULL);
    p = resolve("vm.max_actors=65520\n", NULL);
    CHECK_EQ(p.max_ports, 16);
    CHECK_EQ(said_n, 0);
    /* M alone (gogo_config.bytes_per_actor without bytes_handles): N kept. */
    flags[PARAM_HANDLES] = "/4";
    p = resolve("vm.handles=40\n", NULL);    /* the overlay beats the whole key */
    CHECK_EQ(p.bytes_handles, d.bytes_handles);
    CHECK_EQ(p.bytes_per_actor, 4);
    CHECK_EQ(said_n, 0);
#if GOGO_ARENA_BYTES
    p = resolve("vm.memory=1G\n", NULL);
    CHECK_EQ(p.memory, GOGO_ARENA_BYTES);
#else
    p = resolve("vm.memory=1G\n", NULL);
    CHECK_EQ(p.memory, 1u << 30);
    CHECK_EQ(said_n, 0);
#endif
#if GOGO_WORDS_16
    /* A board's "256K" is its 65535 words, silently. */
    p = resolve("vm.actor_memory=256K\n", NULL);
    CHECK_EQ(p.actor_memory, 65535u * 4u);
    CHECK_EQ(said_n, 0);
#endif
}

/* vm_params_apply: the Vm takes them; an actor made after it has the
 * default limit, and Bytes caps and the memory limit hold. */
TEST(params_apply) {
    Vm vm;
    VmParams p = resolve("vm.actor_memory=4K\nvm.memory=64K\nvm.handles=8/2\ncode.mem=2K\n", NULL);
    p.max_actors = 16;
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    CHECK_EQ(vm_params_apply(&vm, &p), 0);
    CHECK_EQ(vm.actor_words, 1024);
    CHECK_EQ(vm.va.limit, 65536);
    CHECK_EQ(vm.bytes_cap, 8);
    CHECK_EQ(vm.bytes_per_actor, 2);
    CHECK_EQ(vm.mod_cap, 2048);
    Actor *a = actor_new(&vm, 24);
    CHECK(a != NULL);
    if (!a) return;
    CHECK_EQ(actor_max_words(a), 1024);
    actor_free(&vm, a);
}

/* The node agent's limit: 256 KB at least (a board's ceiling, so a board
 * always has it), the actor memory, or 320 B a link and 64 KB. */
TEST(params_node_agent) {
    VmParams p;
    vm_params_default(&p);
    p.max_links = 0;
    p.actor_memory = 16384;
    CHECK_EQ(params_node_agent_bytes(&p), GOGO_WORDS_16 ? 65535u * 4u : 262144u);
#if !GOGO_WORDS_16
    p.max_links = 2000;
    CHECK_EQ(params_node_agent_bytes(&p), 2000u * NODE_AGENT_LINK_BYTES + NODE_AGENT_BASE);
    p.actor_memory = 4u << 20;
    CHECK_EQ(params_node_agent_bytes(&p), 4u << 20);
#endif
}
