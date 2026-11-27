/* gogovm: runs a gogo image file.
 *
 *   gogovm [--without NAME,...] [--key c=pin]... [--fs-root DIR] [--fs-quota BYTES]
 *          [--net-fd N [--sim-clock]] [--screen ansi|text|none] <file.ggb>
 *          runs until the run queue empties, then exits with the main
 *          actor's status (below). --without and --fs-quota are for tests:
 *          --without drops the named port drivers ("kbd", "tls", ...) and
 *          mounts ("/sd") from the board, modelling a firmware built
 *          without them; --fs-quota simulates a full disk: each medium
 *          may hold BYTES of files (what is there counts, deletes give
 *          room back; Usage reports used and BYTES).
 *   gogovm [flags] <file.ggb> [args...]
 *          everything after the image is the program's own: sys.Args
 *          returns it (flags come before the image, as for gogo run).
 *   gogovm [flags] --info  prints the VM's parameters as this run would
 *          resolve them (vm_params_format: flags, then the config keys,
 *          then the defaults) and exits
 *   gogovm [flags] --provides  prints the requirement names this VM
 *          provides, one a line: the board's drivers (after --without),
 *          its mounts, "float" in a float build, then the board ports
 *          (GOGO_BOARD_PORT drivers built in, `make vm DRIVERS=...`) it
 *          would register, in registration order; as `gogo device info`
 *          lists a board's
 *   gogovm -h | --help     lists every flag (usage(), below)
 *   gogovm --sizeof-actor  prints sizeof(Actor)
 *   gogovm --sizeof-net    prints the net stack's fixed state in bytes
 *   gogovm --has-tls       exit 0 if built with the "tls" port (GOGO_TLS)
 *
 * Built with GOGO_TLS (the Makefile does when it finds the mbedTLS source),
 * gogovm has the "tls" port; $GOGO_TLS_STATS set makes it print
 * one "tls-stats ..." line to stderr at exit (tls_stats, the TLS heap, and
 * vm_peak: the VM allocator's peak, va_peak_bytes).
 *
 * --screen: how the "screen" port shows its grid (screen_unix.c): ANSI on
 * the terminal, a text dump of every frame on stdout, or nothing; the
 * default is ansi when stdout is a terminal, else text.
 *
 * --host-net (or $GOGO_NET=host): the "tcp" and "tcp_listen" ports use the
 * host's sockets (hostnet.c), so the VM can be a real network peer, e.g. the
 * server node of distributed actors; netif, udp and DNS stay the simulated
 * ones. Listening binds every interface; outbound connections go anywhere.
 * Not with --net-fd.
 *
 * The VM's parameters (runtime/board/params.h, shared with the board firmware
 * and libgogo; a flag beats the config key, which beats the default, and
 * a value past the ceiling is clamped with one line on stderr):
 *   --actor-memory SIZE  an actor's default memory limit (vm.actor_memory;
 *                        1 MB; at most 1 GB)
 *   --memory SIZE        all actors together (vm.memory; default no limit)
 *   --max-actors N       live actors at once (vm.max_actors; 16384)
 *   --max-ports N        ports open at once (vm.max_ports; 1024, 1..2048:
 *                        their PIDs take the top slots of the 65536)
 *   --max-links N        node links (node.max_links; 512, 1..2048)
 *   --bytes-handles N[/M] off-heap Bytes handles, VM-wide and per actor
 *                        (vm.handles; 4096/256)
 * SIZE is bytes with an optional K, M or G suffix; a board's defaults are
 * 16K, 160K (its arena), 1024, 16, 8 and 64/16.
 *
 * --net-fd N: fd N (inherited, e.g. a socketpair end) is the network link
 * (net_unix.h); --sim-clock: time is simulated, moved only by the peer
 * simulator over that link.
 *
 * Exit status (gogo run passes it through; cmd/gogo/exitcodes.go):
 *   0        main exited Normal (other actors' crashes do not count)
 *   1        usage error, --fs-root error, out of memory at boot, no main
 *   2        bad image (cannot be read or refused by the loader), or an
 *            image format this VM does not run
 *   3        the image requires a driver or mount this VM lacks
 *   4        main crashed: a VM reason (killed, oom, divzero, ...), or
 *            an exit code below 64
 *   64..99   main exited with a library failure (actor.Failed + a
 *            shared gogo/errors code), as it is
 *   100..255 main exited with a user code; one above 255 is 255
 * The VM core does not keep main's exit reason: it reports every exit
 * other than Normal and Shutdown on stream 2 while the actor is still in
 * its slot (report_crash, signals.c), and main_write below picks out
 * main's report by the actor's PID.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "platform_unix.h"
#include "actor.h"
#include "alloc.h"
#include "interp.h"
#include "signals.h"
#include "module.h"
#include "config.h"
#include "gpio_unix.h"
#include "input_unix.h"
#include "input_int.h"
#include "net_unix.h"
#include "net_int.h"
#include "vfs_int.h"
#include "fs_dir.h"
#include "screen_unix.h"
#include "board_main.h"
#include "board_ports.h"
#include "hostnet.h"
#include "dist.h"
#include "params.h"

#include <sys/stat.h>
#include <errno.h>

#include <signal.h>

#ifdef GOGO_TLS
#include "tls.h"
#include "tls_unix.h"
#define TLS_DRIVERS 1
#else
#define TLS_DRIVERS 0
#endif


/* The board's one Vm's vfs, input and net state (runtime/ keeps none). */
static VfsState vfs_state;
static FsDir *fs_flash, *fs_sd;   /* --fs-root: /flash and /sd */
static InputState input_state;
static NetState net_state;

/* The host's default caps (gogovm_mcu, which models a board, has 16 and 8). */
#ifndef GOGO_HOST_PORTS
#define GOGO_HOST_PORTS 1024
#endif
#ifndef GOGO_HOST_LINKS
#define GOGO_HOST_LINKS 512
#endif

enum { EXIT_OK, EXIT_USAGE, EXIT_BAD_IMAGE, EXIT_MISSING, EXIT_CRASHED };

static int load_error(const char *path, const char *what) {
    fprintf(stderr, "gogovm: %s: %s\n", path, what);
    return EXIT_BAD_IMAGE;
}

/* ---- main's exit status ---- */

static Vm *main_vm;            /* the Vm, once main runs */
static uint32_t main_pid;      /* main's PID term; 0 before vm_start */
static int main_status;        /* EXIT_OK until main's report is seen */
static char err_line[64];      /* the start of the stream-2 line being written */
static size_t err_len;

/* The status for main's exit code (the table at the top). */
static int exit_status(int32_t code) {
    if (code >= 64 && code <= 99) return code;
    if (code >= 100) return code > 255 ? 255 : code;
    return EXIT_CRASHED;
}

/* One whole stream-2 line: if it is the crash report of main
 * ("gogo: actor <pid N> crashed: <kind> | exit(<code>)[ in f]", N the
 * slot, which still holds main), main_status is its status. */
static void err_line_done(void) {
    static const char pre[] = CRASH_REPORT_PID, mid[] = CRASH_REPORT_KIND;
    const char *p = err_line, *end = err_line + err_len;
    if (err_len < sizeof pre - 1u || memcmp(p, pre, sizeof pre - 1u) != 0) return;
    p += sizeof pre - 1u;
    uint32_t slot = 0;
    const char *d = p;
    while (p < end && *p >= '0' && *p <= '9' && slot <= 0xFFFFu) slot = slot * 10u + (uint32_t)(*p++ - '0');
    if (p == d || (size_t)(end - p) < sizeof mid - 1u || memcmp(p, mid, sizeof mid - 1u) != 0) return;
    p += sizeof mid - 1u;
    if (slot >= main_vm->table_cap) return;
    uintptr_t e = main_vm->table[slot];
    if ((e & 1u) || ((const Actor *)e)->id != main_pid) return;   /* a free slot, or another actor */
    int32_t code = 1;                                             /* a kind: a crash */
    if (end - p > 5 && memcmp(p, "exit(", 5) == 0) {
        char num[16];
        size_t n = 0;
        for (p += 5; p < end && *p != ')' && n < sizeof num - 1u; p++) num[n++] = *p;
        num[n] = 0;
        code = (int32_t)strtol(num, NULL, 10);
    }
    main_status = exit_status(code);
}

/* The host's write, watching stream 2 for main's report. Only the start
 * of a line is kept: the report's fields fit in it. */
static void main_write(void *ctx, int stream, const char *s, size_t n) {
    unix_host.write(ctx, stream, s, n);
    if (stream != 2 || !main_pid) return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            err_line_done();
            err_len = 0;
        } else if (err_len < sizeof err_line) {
            err_line[err_len++] = s[i];
        }
    }
}

/* The VM's parameters as main resolved them (for main_config), and the
 * parameter flags as typed, by PARAM_* (NULL: not given). */
static const VmParams *run_params;
static const char *flag_text[PARAM_COUNT];

/* The host's config_get: the parameter flags first (params_overlay), so a
 * flag beats its key when the parameters are resolved and sys.Config sees
 * what the VM runs with; NODE_AGENT_KEY (params.h: the node agent's memory
 * limit from the link cap and the actor memory, for gogo/node; no store
 * key is that long); then the store. */
static int32_t flags_config(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    return params_overlay(flag_text, k, kl, buf, cap);
}

static int32_t main_config(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    static const char agent[] = NODE_AGENT_KEY;
    int32_t f = params_overlay(flag_text, k, kl, buf, cap);
    if (f >= 0) return f;
    if (kl == sizeof agent - 1u && memcmp(k, agent, kl) == 0 && run_params) {
        char num[12];
        int n = snprintf(num, sizeof num, "%u", (unsigned)params_node_agent_bytes(run_params));
        if (buf) memcpy(buf, num, (uint32_t)n < cap ? (size_t)n : cap);
        return n;
    }
    return unix_host.config_get(ctx, k, kl, buf, cap);
}

/* Runs the started Vm as vm_run does, then returns main's status. main
 * is the one actor vm_start made. */
/* BoardRun.booting: the wall clock (the host's, or $GOGO_UNIX_TIME_MS). */
static void set_wall_clock(Vm *vm, void *ctx) {
    (void)ctx;
    vm_set_unix_time_ms(vm, platform_unix_wall_start());
}

/* BoardRun.ready with --host-net: the sockets' event source. */
static void host_net_ready(Vm *vm, void *ctx) {
    (void)ctx;
    hostnet_install(vm);
}

static int run_main(Vm *vm, void *ctx) {
    (void)ctx;
    platform_set_links(vm->max_links);   /* gogo/node's handshake slots ("\0links") */
    if (vm_start(vm)) return EXIT_USAGE;   /* said on stream 2: no main, or no memory */
    for (uint16_t i = 0; i < vm->table_cap && !main_pid; i++) {
        if (!(vm->table[i] & 1u)) main_pid = ((const Actor *)vm->table[i])->id;
    }
    if (vm->live != 1u) {   /* vm_start makes main only: anything else is a bug here */
        fprintf(stderr, "gogovm: %u actors at start, want 1\n", (unsigned)vm->live);
        abort();
    }
    main_vm = vm;
    while (vm_step(vm, 0) != VM_DONE) {
        uint64_t next, now;
        if (!vm_next_deadline(vm, &next)) {
            vm_wait(vm, UINT32_MAX);
        } else {
            now = vm_now(vm);
            if (next > now) vm_wait(vm, next - now > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)(next - now));
        }
    }
    vm_stop(vm);
    return main_status;
}

/* The unix board: gpio, net and the simulated keyboard, battery, motion
 * sensor, infrared and audio (input_unix.c), the "tls" port when built
 * with it, and the /flash and /sd mounts. --without (test only) leaves
 * names out of both lists, so it models a firmware without them: an image
 * requiring one is refused, and port.Open of an absent driver fails with
 * errors.NotFound, as on that firmware. */
static const PortDriver unix_kbd = INPUT_KBD_DRIVER(&input_unix_kbd);
static const PortDriver unix_power = INPUT_POWER_DRIVER(&input_unix_power);
static const PortDriver unix_imu = INPUT_IMU_DRIVER(&input_unix_imu);
static const PortDriver unix_ir_tx = INPUT_IR_TX_DRIVER(&input_unix_ir_tx);
static const PortDriver unix_ir_rx = INPUT_IR_RX_DRIVER(&input_unix_ir_rx);
static const PortDriver unix_mic = INPUT_MIC_DRIVER(&input_unix_mic);
static const PortDriver unix_speaker = INPUT_SPEAKER_DRIVER(&input_unix_spk);
static const PortDriver *const unix_ports_all[] = {
    &gpio_unix_driver, &net_netif_driver, &net_udp_driver, &net_tcp_driver,
    &net_tcp_listen_driver, &unix_kbd, &unix_power, &unix_imu, &unix_ir_tx, &unix_ir_rx,
    &unix_mic, &unix_speaker, &screen_unix_driver, &dist_node_driver,
#ifdef GOGO_TLS
    &net_tls_driver,
#endif
};
#define UNIX_PORTS_ALL (sizeof unix_ports_all / sizeof unix_ports_all[0])
/* /flash and /sd: served from --fs-root's flash/ and sd/ (main fills in
 * the backend), else listed with none (NotMounted). */
static VmMount unix_flash = { .prefix = "/flash" }, unix_sd = { .prefix = "/sd" };
static VmMount *const unix_mounts_all[] = { &unix_flash, &unix_sd };
static const PortDriver *unix_ports[UNIX_PORTS_ALL + 1u];   /* filled by main: NULL-terminated */
static const VmMount *unix_mounts[3];                        /* likewise */
static VmBoard unix_board = {   /* .dist_carrier set by --host-net */
    .ports = unix_ports,
    .netif = &unix_netif,
#ifdef GOGO_TLS
    .tls = &unix_tls_hw,
#endif
    .mounts = unix_mounts,
};

/* True if name is one of the comma-separated names in list (NULL: none). */
static int listed(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && strncmp(p, name, n) == 0) return 1;
        p = e ? e + 1 : p + l;
    }
    return 0;
}

#ifdef GOGO_TLS
static size_t tls_heap_base;   /* the TLS heap once tls_init has run (PSA's own state) */
static size_t tls_vm_peak;     /* the VM allocator's peak, taken when vm_run returns */

static void tls_report(void) {
    if (!getenv("GOGO_TLS_STATS")) return;
    const TlsStats *s = tls_stats(&net_state);
    size_t now, peak;
    platform_unix_tls_heap(&now, &peak);
    fprintf(stderr, "tls-stats opens=%u handshakes=%u steps=%u restarts=%u max_step_us=%u max_step_state=%d "
            "max_io_us=%u live=%u last_hs_us=%u last_error=%d heap_base=%zu heap_now=%zu heap_peak=%zu vm_peak=%zu\n", s->opens,
            s->handshakes, s->steps, s->restarts, s->max_step_us, (int)s->max_step_state, s->max_io_us, s->live,
            s->last_hs_us, (int)s->last_error, tls_heap_base, now, peak, tls_vm_peak);
}
#endif


/* A flag's number into *v: decimal, with a K, M or G suffix when size;
 * at most max. False if it is not one. */
static int flag_num(const char *s, int size, unsigned long long max, unsigned long long *v) {
    char *end;
    if (*s < '0' || *s > '9') return 0;
    unsigned long long x = strtoull(s, &end, 10);
    unsigned sh = 0;
    if (size && *end) {
        sh = *end == 'K' || *end == 'k' ? 10u : *end == 'M' || *end == 'm' ? 20u : *end == 'G' || *end == 'g' ? 30u : 99u;
        if (sh == 99u || end[1] || x > max >> sh) return 0;
        end++;
    }
    if (*end || (x << sh) > max) return 0;
    *v = x << sh;
    return 1;
}

/* -h / --help (stdout, exit 0) and a usage error (stderr, exit 1). */
static int usage(FILE *f, int rc) {
    fputs("usage: gogovm [flags] <file.ggb> [args...]   run an image; args are the program's (sys.Args)\n"
          "       gogovm [flags] --info                 print the VM's parameters and exit\n"
          "       gogovm [flags] --provides             print the requirement names this VM provides and exit\n"
          "       gogovm -h | --help                    this list\n"
          "flags (before the image):\n"
          "  --actor-memory SIZE  an actor's default memory limit (default 1M, at most 1G; key vm.actor_memory)\n"
          "  --memory SIZE        all actors together (default no limit; key vm.memory)\n"
          "  --max-actors N       live actors at once (default 16384; key vm.max_actors)\n"
          "  --max-ports N        ports open at once, 1..2048 (default 1024; key vm.max_ports)\n"
          "  --max-links N        node links of distributed actors, 1..2048 (default 512; key node.max_links)\n"
          "  --bytes-handles N[/M] off-heap Bytes handles, VM-wide and per actor (default 4096/256; key vm.handles)\n"
          "  (SIZE: bytes, or with a K, M or G suffix; a flag beats the config key, which beats the default)\n"
          "  --host-net           tcp and tcp_listen on the host's sockets ($GOGO_NET=host)\n"
          "  --net-fd N           fd N is the simulated network link (a socketpair end)\n"
          "  --sim-clock          time moves only by the peer simulator (needs --net-fd)\n"
          "  --fs-root DIR        serve DIR/flash as /flash and DIR/sd (if there) as /sd\n"
          "  --screen MODE        the screen port: ansi, text or none (default ansi on a terminal, else text)\n"
          "  --key C=PIN          map key C to simulated gpio pin PIN (0..63; repeatable)\n"
          "  --without NAME,...   test: leave drivers (kbd, tls, ...) and mounts (/sd) out of the board\n"
          "  --fs-quota BYTES     test: each medium holds at most BYTES of files\n"
          "  --sizeof-actor | --sizeof-net | --has-tls   build facts (alone)\n"
          "environment: GOGO_CONFIG (the config store, default ~/.gogo/config), GOGO_NET, GOGO_UNIX_TIME_MS,\n"
          "  GOGO_TLS_CA_FILE, GOGO_TLS_BUNDLE_FILE, GOGO_TLS_STATS\n", f);
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) return usage(stdout, EXIT_OK);
    if (argc == 2 && strcmp(argv[1], "--sizeof-actor") == 0) {
        printf("%zu\n", sizeof(Actor));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--has-tls") == 0) return TLS_DRIVERS ? 0 : 1;
#ifdef GOGO_TLS
    if (tls_init(&unix_tls_hw)) {                        /* at boot, so the heap base is known */
        size_t peak;
        platform_unix_tls_heap(&tls_heap_base, &peak);
    }
    atexit(tls_report);
#endif
    if (argc == 2 && strcmp(argv[1], "--sizeof-net") == 0) {
        printf("%u\n", (unsigned)net_fixed_bytes());   /* the net stack's fixed state */
        return 0;
    }
    int net_fd = -1, sim = 0;
    const char *gn = getenv("GOGO_NET");
    int host_net = gn && strcmp(gn, "host") == 0;
    const char *without = NULL;         /* --without: names left out of the board */
    const char *fs_root = NULL;
    int64_t fs_quota = -1;
    int info = 0;
    while (argc >= 2 && (strncmp(argv[1], "--", 2) == 0 || strcmp(argv[1], "-h") == 0)) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) return usage(stdout, EXIT_OK);
        if (strcmp(argv[1], "--provides") == 0) {
            info = 2;
            argv++;
            argc--;
            continue;
        }
        if (strcmp(argv[1], "--info") == 0) {
            info = 1;
            argv++;
            argc--;
            continue;
        }
        if (strcmp(argv[1], "--host-net") == 0) {
            host_net = 1;
            argv++;
            argc--;
            continue;
        }
        if (strcmp(argv[1], "--sim-clock") == 0) {
            sim = 1;
            argv++;
            argc--;
            continue;
        }
        if (argc < 3) {
            fprintf(stderr, "gogovm: %s needs a value, or is unknown (gogovm -h lists the flags)\n", argv[1]);
            return EXIT_USAGE;
        }
        if (strcmp(argv[1], "--key") == 0) {
            /* --key <c>=<pin> (tty mode): map a key to a simulated gpio pin. */
            char c;
            int pin;
            char tail;
            if (sscanf(argv[2], "%c=%d%c", &c, &pin, &tail) != 2 || !input_unix_add_key(c, pin)) {
                fprintf(stderr, "gogovm: bad --key %s (want <char>=<pin>, pin 0..63)\n", argv[2]);
                return EXIT_USAGE;
            }
        } else if (strcmp(argv[1], "--screen") == 0) {
            if (!screen_unix_set_mode(argv[2])) {
                fprintf(stderr, "gogovm: bad --screen %s (want ansi, text or none)\n", argv[2]);
                return EXIT_USAGE;
            }
        } else if (strcmp(argv[1], "--without") == 0) {
            /* --without <name,...>: test only, drivers and mounts the board leaves out. */
            without = argv[2];
        } else if (strcmp(argv[1], "--fs-root") == 0) {
            fs_root = argv[2];
        } else if (strcmp(argv[1], "--fs-quota") == 0) {
            /* --fs-quota <bytes>: test only, the bytes the files of each
             * medium may hold (fs_dir.h); a write past it is NoSpace, 0
             * is a full medium. */
            char tail;
            long long q;
            if (sscanf(argv[2], "%lld%c", &q, &tail) != 1 || q < 0) {
                fprintf(stderr, "gogovm: bad --fs-quota %s\n", argv[2]);
                return EXIT_USAGE;
            }
            fs_quota = q;
        } else if (strcmp(argv[1], "--max-ports") == 0 || strcmp(argv[1], "--max-links") == 0) {
            char tail;
            unsigned v;
            if (sscanf(argv[2], "%u%c", &v, &tail) != 1 || v < 1 || v > 2048) {
                fprintf(stderr, "gogovm: bad %s %s (want 1..2048)\n", argv[1], argv[2]);
                return EXIT_USAGE;
            }
            flag_text[argv[1][6] == 'p' ? PARAM_MAX_PORTS : PARAM_MAX_LINKS] = argv[2];
        } else if (strcmp(argv[1], "--actor-memory") == 0 || strcmp(argv[1], "--memory") == 0 ||
                   strcmp(argv[1], "--max-actors") == 0) {
            unsigned long long v;
            int size = argv[1][3] != 'a';   /* --max-actors: a count */
            int mem = argv[1][2] == 'm' && argv[1][3] == 'e';
            /* --memory 0: no limit (it undoes a vm.memory key). */
            if (!flag_num(argv[2], size, size ? 1ull << 48 : 0xFFFFFFFFull, &v) || (v == 0 && !mem)) {
                fprintf(stderr, "gogovm: bad %s %s (want %s)\n", argv[1], argv[2],
                        size ? "bytes, with an optional K, M or G" : "a count");
                return EXIT_USAGE;
            }
            flag_text[argv[1][2] == 'a' ? PARAM_ACTOR_MEMORY : mem ? PARAM_MEMORY : PARAM_MAX_ACTORS] = argv[2];
        } else if (strcmp(argv[1], "--bytes-handles") == 0) {
            unsigned long long n, m = 0;
            char a[24];
            const char *slash = strchr(argv[2], '/');
            size_t an = slash ? (size_t)(slash - argv[2]) : strlen(argv[2]);
            if (an >= sizeof a || (memcpy(a, argv[2], an), a[an] = 0, !flag_num(a, 0, 0xFFFFu, &n)) || n == 0 ||
                (slash && (!flag_num(slash + 1, 0, 0xFFFFu, &m) || m == 0))) {
                fprintf(stderr, "gogovm: bad --bytes-handles %s (want N or N/M, 1..65535)\n", argv[2]);
                return EXIT_USAGE;
            }
            flag_text[PARAM_HANDLES] = argv[2];
        } else if (strcmp(argv[1], "--net-fd") == 0) {
            char tail;
            if (sscanf(argv[2], "%d%c", &net_fd, &tail) != 1 || net_fd < 0) {
                fprintf(stderr, "gogovm: bad --net-fd %s\n", argv[2]);
                return EXIT_USAGE;
            }
        } else {
            fprintf(stderr, "gogovm: unknown flag %s (gogovm -h lists them)\n", argv[1]);
            return EXIT_USAGE;
        }
        argv += 2;
        argc -= 2;
    }
    if (sim && net_fd < 0) {
        fprintf(stderr, "gogovm: --sim-clock needs --net-fd\n");
        return EXIT_USAGE;
    }
    if (host_net) unix_board.dist_carrier = &hostnet_carrier;
    if (host_net && net_fd >= 0) {
        fprintf(stderr, "gogovm: --host-net and --net-fd exclude each other\n");
        return EXIT_USAGE;
    }
    size_t nports = 0, nmounts = 0;
    for (size_t i = 0; i < UNIX_PORTS_ALL; i++) {
        const PortDriver *d = unix_ports_all[i];
        if (host_net && d == &net_tcp_driver) d = &hostnet_tcp_driver;
        if (host_net && d == &net_tcp_listen_driver) d = &hostnet_listen_driver;
        if (!listed(without, d->name)) unix_ports[nports++] = d;
    }
    unix_ports[nports] = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (!listed(without, unix_mounts_all[i]->prefix)) unix_mounts[nmounts++] = unix_mounts_all[i];
    }
    unix_mounts[nmounts] = NULL;
    /* The parameters: the flags, then the config keys, then the host's
     * defaults (params.h). */
    VmParams params, defaults;
    gogo_host host = unix_host, flags_only = unix_host;
    host.write = main_write;
    host.config_get = main_config;
    flags_only.write = NULL;            /* said below, with the keys */
    flags_only.config_get = flags_config;
    vm_params_default(&defaults);
    defaults.max_ports = GOGO_HOST_PORTS;
    defaults.max_links = GOGO_HOST_LINKS;
    params = defaults;
    /* What a boot that does not fit falls back to: the defaults with the
     * flags (gogo_board_main drops only the store's keys; a flag that
     * cannot fit stays an error). */
    vm_params_resolve(&flags_only, NULL, &defaults);
    vm_params_resolve(&host, NULL, &params);
    if (info == 2) {
        for (const PortDriver *const *d = unix_ports; *d; d++) printf("%s\n", (*d)->name);
        for (const VmMount *const *m = unix_mounts; *m; m++) printf("%s\n", (*m)->prefix);
#if GOGO_FLOAT
        printf("%s\n", PORT_REQ_FLOAT);
#endif
        const char *bp[16];
        uint32_t n = board_ports_names(&unix_board, bp, 16);
        for (uint32_t i = 0; i < n && i < 16; i++) printf("%s\n", bp[i]);
        return EXIT_OK;
    }
    if (info) {
        char line[256];
        vm_params_format(&params, line, sizeof line);
        printf("%s\n", line);
        return EXIT_OK;
    }
    if (argc < 2) return usage(stderr, EXIT_USAGE);
    if (net_fd >= 0 || host_net) signal(SIGPIPE, SIG_IGN);   /* a closed link is EOF / a failed send, not death */
    if (net_fd >= 0) platform_unix_net_open(net_fd, sim != 0, &net_state);
    if (fs_root) {
        /* /flash -> DIR/flash (created), /sd -> DIR/sd (only if it exists). */
        char dir[1024];
        struct stat st;
        if (snprintf(dir, sizeof dir, "%s/flash", fs_root) >= (int)sizeof dir) {
            fprintf(stderr, "gogovm: --fs-root too long\n");
            return EXIT_USAGE;
        }
        if (mkdir(fs_root, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot create\n", fs_root);
            return EXIT_USAGE;
        }
        if ((mkdir(dir, 0777) != 0 && errno != EEXIST) || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot create flash directory\n", fs_root);
            return EXIT_USAGE;
        }
        fs_flash = fs_dir_open(&unix_host, NULL, dir, fs_quota, NULL);
        if (!fs_flash) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot open flash directory\n", fs_root);
            return EXIT_USAGE;
        }
        unix_flash.be = fs_dir_vfs(fs_flash);
        unix_flash.ctx = fs_flash;
        if (!listed(without, "/sd") && snprintf(dir, sizeof dir, "%s/sd", fs_root) < (int)sizeof dir &&
            stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            fs_sd = fs_dir_open(&unix_host, NULL, dir, fs_quota, NULL);
            if (fs_sd) {
                unix_sd.be = fs_dir_vfs(fs_sd);
                unix_sd.ctx = fs_sd;
            }
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *path = argv[1];
    platform_set_args(argc - 2, argv + 2);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "gogovm: %s: cannot open\n", path); return EXIT_USAGE; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return load_error(path, "cannot read"); }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return load_error(path, "cannot read"); }
    if (size % 4 != 0) { fclose(f); return load_error(path, "size is not a multiple of 4"); }
    uint32_t *words = malloc(size > 0 ? (size_t)size : 4u);
    if (!words) { fclose(f); return load_error(path, "out of memory"); }
    size_t got = fread(words, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(words); return load_error(path, "cannot read"); }
    /* Image words are little-endian; this loader assumes a little-endian host. */

    /* The one boot path (runtime/board): vm_init, the wall clock, vm_boot
     * (the image is module 0, its requirements checked), the net stack
     * (with --net-fd), the file systems (vfs_init mounts the board's
     * /flash and /sd that have a backend) and the input source, then
     * run_main. */
    Vm vm;
    run_params = &params;
    input_unix_on_fatal(screen_unix_restore);
    BoardRun run = {
        .vm = &vm, .params = &params, .defaults = &defaults, .vfs = &vfs_state, .input = &input_state,
        .net = net_fd >= 0 ? &net_state : NULL, .net_install = net_install,
        .booting = set_wall_clock, .ready = host_net ? host_net_ready : NULL, .run = run_main,
    };
    int run_rc = 0;
    int rc = gogo_board_main(&unix_board, &host, &run, words, (uint32_t)(size / 4), &run_rc);
    if (rc != VM_BOOT_OK) {
        uint32_t ver = size >= 8 ? words[1] : 0u;
        free(words);
        if (rc == IMG_ERR_VERSION) {
            fprintf(stderr, "gogovm: %s: unsupported image format %u.%u (this VM runs %u.0 to %u.%u)\n", path,
                    (unsigned)(ver >> 16), (unsigned)(ver & 0xFFFFu), IMAGE_MAJOR, IMAGE_MAJOR, IMAGE_MINOR);
            return EXIT_BAD_IMAGE;
        }
        if (rc < 0) return load_error(path, image_err_str(rc));
        if (rc == VM_BOOT_NO_MEMORY) {
            fprintf(stderr, "gogovm: out of memory\n");
            return EXIT_USAGE;
        }
        return EXIT_MISSING;         /* VM_BOOT_REFUSED: the line is written */
    }
    rc = run_rc;
#ifdef GOGO_TLS
    tls_vm_peak = va_peak_bytes(&vm);
#endif
    fflush(stdout);
    free(words);
    fs_dir_close(fs_sd);
    fs_dir_close(fs_flash);
    return rc;
}
