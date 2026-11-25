/* gogovm: runs a gogo image file.
 *
 *   gogovm [--key c=pin]... [--fs-root DIR] [--net-fd N [--sim-clock]] <file.gogo>
 *          exit 0 when the run queue empties, 2 on load error
 *   gogovm --sizeof-actor  prints sizeof(Actor)
 *   gogovm --sizeof-net    prints the net stack's fixed state in bytes
 *
 * --net-fd N: fd N (inherited, e.g. a socketpair end) is the network link
 * (net_unix.h); --sim-clock: time is simulated, moved only by the peer
 * simulator over that link (spec M5 §9.7).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "platform.h"
#include "actor.h"
#include "interp.h"
#include "module.h"
#include "config.h"
#include "caps_unix.h"
#include "gpio_unix.h"
#include "input_unix.h"
#include "input.h"
#include "net_unix.h"
#include "net.h"
#include "vfs.h"
#include "fs_host.h"

#include <sys/stat.h>
#include <errno.h>

#include <signal.h>


static int load_error(const char *path, const char *what) {
    fprintf(stderr, "gogovm: %s: %s\n", path, what);
    return 2;
}

/* The unix keyboard, battery, motion sensor and infrared are simulated from stdin (input_unix.c). */
const InputHw *platform_input(void) { return &input_unix_hw; }
int platform_unix_input_drivers(const struct PortDriver **out, int cap) {
    if (cap < 5) {   /* the driver list in platform_unix.c is full: skip them, loudly */
        fprintf(stderr, "gogovm: no room in the port driver list: kbd, power, imu and ir skipped\n");
        return 0;
    }
    out[0] = &input_kbd_driver;
    out[1] = &input_power_driver;
    out[2] = &input_imu_driver;
    out[3] = &input_ir_tx_driver;
    out[4] = &input_ir_rx_driver;
    return 5;
}


int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--sizeof-actor") == 0) {
        printf("%zu\n", sizeof(Actor));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--sizeof-net") == 0) {
        printf("%u\n", (unsigned)net_fixed_bytes());   /* the net stack's fixed state */
        return 0;
    }
    int net_fd = -1, sim = 0;
    const char *fs_root = NULL;
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
        } else if (strcmp(argv[1], "--caps") == 0) {
            /* --caps <hex>: test only, the firmware capability bits. */
            char tail;
            unsigned caps;
            if (sscanf(argv[2], "%x%c", &caps, &tail) != 1) {
                fprintf(stderr, "gogovm: bad --caps %s (want hex)\n", argv[2]);
                return 2;
            }
            platform_unix_set_caps(caps);
        } else if (strcmp(argv[1], "--fs-root") == 0) {
            fs_root = argv[2];
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
    if (argc != 2) {
        fprintf(stderr, "usage: gogovm [--caps <hex>] [--key <c>=<pin>]... [--fs-root <dir>] [--net-fd <n> [--sim-clock]] <file.gogo>\n       gogovm --sizeof-actor | --sizeof-net\n");
        return 2;
    }
    if (net_fd >= 0) {
        signal(SIGPIPE, SIG_IGN);        /* a closed link is EOF / a failed send, not death */
        platform_unix_net_open(net_fd, sim != 0);
        net_attach();
    }
    if (fs_root) {
        /* /flash -> DIR/flash (created), /sd -> DIR/sd (only if it exists). */
        char dir[FS_HOST_ROOT_MAX];
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
        const VfsBackend *flash = fs_host_open(0, dir);
        if (!flash) {
            fprintf(stderr, "gogovm: --fs-root too long\n");
            return 2;
        }
        vfs_mount("/flash", flash);
        if (snprintf(dir, sizeof dir, "%s/sd", fs_root) < (int)sizeof dir &&
            stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            const VfsBackend *sd = fs_host_open(1, dir);
            if (sd) vfs_mount("/sd", sd);
        }
    }
    vfs_install();
    input_install();
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

    /* The unified boot (module.h): load the image as module 0 (caps check
     * included), then vm_run starts its "main" export. */
    Vm vm;
    int rc = vm_boot(&vm, words, (uint32_t)(size / 4), (uint16_t)GOGO_TABLE_CAP);
    if (rc != VM_BOOT_OK) {
        free(words);
        if (rc < 0) return load_error(path, image_err_str(rc));
        if (rc == VM_BOOT_NO_MEMORY) fprintf(stderr, "gogovm: out of memory\n");
        return 1;                    /* VM_BOOT_REFUSED: the line is written */
    }
    rc = vm_run(&vm);
    fflush(stdout);
    free(words);
    return rc;
}
