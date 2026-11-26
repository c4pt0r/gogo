/* gogovm: runs a gogo image file.
 *
 *   gogovm [--without NAME,...] [--key c=pin]... [--fs-root DIR] [--fs-quota BYTES]
 *          [--net-fd N [--sim-clock]] [--screen ansi|text|none] <file.ggb>
 *          exit 0 when the run queue empties (also after a crash), 1 when
 *          the board lacks a driver or mount the image requires, 2 on a
 *          usage or load error. --without and --fs-quota are for tests:
 *          --without drops the named port drivers ("kbd", "tls", ...) and
 *          mounts ("/sd") from the board, modelling a firmware built
 *          without them; --fs-quota simulates a full disk: each medium
 *          may hold BYTES of files (what is there counts, deletes give
 *          room back; Usage reports used and BYTES).
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
 * --net-fd N: fd N (inherited, e.g. a socketpair end) is the network link
 * (net_unix.h); --sim-clock: time is simulated, moved only by the peer
 * simulator over that link.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "platform_unix.h"
#include "actor.h"
#include "alloc.h"
#include "interp.h"
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

static int load_error(const char *path, const char *what) {
    fprintf(stderr, "gogovm: %s: %s\n", path, what);
    return 2;
}

/* The unix board: gpio, net and the simulated keyboard, battery, motion
 * sensor, infrared and audio (input_unix.c), the "tls" port when built
 * with it, and the /flash and /sd mounts. --without (test only) leaves
 * names out of both lists, so it models a firmware without them: an image
 * requiring one is refused, and port.Open of an absent driver returns
 * ok=false, as on that firmware. */
static const PortDriver *const unix_ports_all[] = {
    &gpio_unix_driver, &net_netif_driver, &net_udp_driver, &net_tcp_driver,
    &net_tcp_listen_driver, &input_kbd_driver, &input_power_driver,
    &input_imu_driver, &input_ir_tx_driver, &input_ir_rx_driver,
    &input_mic_driver, &input_speaker_driver, &screen_unix_driver,
#ifdef GOGO_TLS
    &net_tls_driver,
#endif
};
#define UNIX_PORTS_ALL (sizeof unix_ports_all / sizeof unix_ports_all[0])
static const char *const unix_mounts_all[] = { "/flash", "/sd" };
static const PortDriver *unix_ports[UNIX_PORTS_ALL + 1u];   /* filled by main: NULL-terminated */
static const char *unix_mounts[3];                           /* likewise */
static const VmBoard unix_board = {
    unix_ports, &input_unix_hw, &unix_netif,
#ifdef GOGO_TLS
    &unix_tls_hw,
#else
    NULL,
#endif
    unix_mounts
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


int main(int argc, char **argv) {
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
    const char *without = NULL;         /* --without: names left out of the board */
    const char *fs_root = NULL;
    int64_t fs_quota = -1;
    while (argc >= 2 && strncmp(argv[1], "--", 2) == 0) {
        if (strcmp(argv[1], "--sim-clock") == 0) {
            sim = 1;
            argv++;
            argc--;
            continue;
        }
        if (argc < 3) break;
        if (strcmp(argv[1], "--key") == 0) {
            /* --key <c>=<pin> (tty mode): map a key to a simulated gpio pin. */
            char c;
            int pin;
            char tail;
            if (sscanf(argv[2], "%c=%d%c", &c, &pin, &tail) != 2 || !input_unix_add_key(c, pin)) {
                fprintf(stderr, "gogovm: bad --key %s (want <char>=<pin>, pin 0..63)\n", argv[2]);
                return 2;
            }
        } else if (strcmp(argv[1], "--screen") == 0) {
            if (!screen_unix_set_mode(argv[2])) {
                fprintf(stderr, "gogovm: bad --screen %s (want ansi, text or none)\n", argv[2]);
                return 2;
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
                return 2;
            }
            fs_quota = q;
        } else if (strcmp(argv[1], "--net-fd") == 0) {
            char tail;
            if (sscanf(argv[2], "%d%c", &net_fd, &tail) != 1 || net_fd < 0) {
                fprintf(stderr, "gogovm: bad --net-fd %s\n", argv[2]);
                return 2;
            }
        } else {
            break;
        }
        argv += 2;
        argc -= 2;
    }
    if (sim && net_fd < 0) {
        fprintf(stderr, "gogovm: --sim-clock needs --net-fd\n");
        return 2;
    }
    size_t nports = 0, nmounts = 0;
    for (size_t i = 0; i < UNIX_PORTS_ALL; i++) {
        if (!listed(without, unix_ports_all[i]->name)) unix_ports[nports++] = unix_ports_all[i];
    }
    unix_ports[nports] = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (!listed(without, unix_mounts_all[i])) unix_mounts[nmounts++] = unix_mounts_all[i];
    }
    unix_mounts[nmounts] = NULL;
    if (argc != 2) {
        fprintf(stderr, "usage: gogovm [--without <name,...>] [--key <c>=<pin>]... [--fs-root <dir>] [--fs-quota <bytes per medium> (test)] [--net-fd <n> [--sim-clock]] [--screen ansi|text|none] <file.ggb>\n       gogovm --sizeof-actor | --sizeof-net\n");
        return 2;
    }
    if (net_fd >= 0) {
        signal(SIGPIPE, SIG_IGN);        /* a closed link is EOF / a failed send, not death */
        platform_unix_net_open(net_fd, sim != 0, &net_state);
    }
    if (fs_root) {
        /* /flash -> DIR/flash (created), /sd -> DIR/sd (only if it exists). */
        char dir[1024];
        struct stat st;
        if (snprintf(dir, sizeof dir, "%s/flash", fs_root) >= (int)sizeof dir) {
            fprintf(stderr, "gogovm: --fs-root too long\n");
            return 2;
        }
        if (mkdir(fs_root, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot create\n", fs_root);
            return 2;
        }
        if ((mkdir(dir, 0777) != 0 && errno != EEXIST) || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot create flash directory\n", fs_root);
            return 2;
        }
        fs_flash = fs_dir_open(&unix_host, NULL, dir, fs_quota, NULL);
        if (!fs_flash) {
            fprintf(stderr, "gogovm: --fs-root %s: cannot open flash directory\n", fs_root);
            return 2;
        }
        if (!listed(without, "/flash")) vfs_mount(&vfs_state, "/flash", fs_dir_vfs(fs_flash), fs_flash);
        if (!listed(without, "/sd") && snprintf(dir, sizeof dir, "%s/sd", fs_root) < (int)sizeof dir &&
            stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            fs_sd = fs_dir_open(&unix_host, NULL, dir, fs_quota, NULL);
            if (fs_sd) vfs_mount(&vfs_state, "/sd", fs_dir_vfs(fs_sd), fs_sd);
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) return load_error(path, "cannot open");
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

    /* The unified boot (module.h): load the image as module 0 (requirement
     * check included), then vm_run starts its "main" export. */
    Vm vm;
    int rc = VM_BOOT_NO_MEMORY;
    if (vm_init(&vm, &unix_host, NULL, &unix_board, (uint16_t)GOGO_TABLE_CAP) == 0) {
        vm_set_unix_time_ms(&vm, platform_unix_wall_start());
        rc = vm_boot(&vm, words, (uint32_t)(size / 4));
    }
    if (rc != VM_BOOT_OK) {
        uint32_t ver = size >= 8 ? words[1] : 0u;
        free(words);
        if (rc == IMG_ERR_VERSION) {
            fprintf(stderr, "gogovm: %s: unsupported image format %u.%u (this VM runs %u.0 to %u.%u)\n", path,
                    (unsigned)(ver >> 16), (unsigned)(ver & 0xFFFFu), IMAGE_MAJOR, IMAGE_MAJOR, IMAGE_MINOR);
            return 2;
        }
        if (rc < 0) return load_error(path, image_err_str(rc));
        if (rc == VM_BOOT_NO_MEMORY) fprintf(stderr, "gogovm: out of memory\n");
        return 1;                    /* VM_BOOT_REFUSED: the line is written */
    }
    /* After vm_init: it starts the Vm's source list and fs hook empty. */
    if (net_fd >= 0) net_install(&vm, &net_state);
    vfs_init(&vm, &vfs_state);
    input_install(&vm, &input_state);
    input_unix_on_fatal(screen_unix_restore);
    rc = vm_run(&vm);
#ifdef GOGO_TLS
    tls_vm_peak = va_peak_bytes(&vm);
#endif
    fflush(stdout);
    free(words);
    fs_dir_close(fs_sd);
    fs_dir_close(fs_flash);
    return rc;
}
