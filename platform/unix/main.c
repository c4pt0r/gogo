/* gogovm: runs a gogo image file.
 *
 *   gogovm [--key c=pin]... [--net-fd N [--sim-clock]] <file.gogo>
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
#include "actor.h"
#include "interp.h"
#include "config.h"
#include "gpio_unix.h"
#include "net_unix.h"
#include "net.h"

#include <signal.h>


static int load_error(const char *path, const char *what) {
    fprintf(stderr, "gogovm: %s: %s\n", path, what);
    return 2;
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
            if (sscanf(argv[2], "%c=%d%c", &c, &pin, &tail) != 2 || !gpio_unix_add_key(c, pin)) {
                fprintf(stderr, "gogovm: bad --key %s (want <char>=<pin>, pin 0..63)\n", argv[2]);
                return 2;
            }
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
        fprintf(stderr, "usage: gogovm [--key <c>=<pin>]... [--net-fd <n> [--sim-clock]] <file.gogo>\n       gogovm --sizeof-actor | --sizeof-net\n");
        return 2;
    }
    if (net_fd >= 0) {
        signal(SIGPIPE, SIG_IGN);        /* a closed link is EOF / a failed send, not death */
        platform_unix_net_open(net_fd, sim != 0);
        net_attach();
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

    Image img;
    int rc = image_load(&img, words, (uint32_t)(size / 4));
    if (rc != IMG_OK) {
        static const char *msgs[] = {"ok", "bad magic", "unsupported version", "truncated image", "bad offsets"};
        const char *m = (rc <= 0 && rc >= -4) ? msgs[-rc] : "invalid image";
        free(words);
        return load_error(path, m);
    }
    Vm vm;
    if (vm_init(&vm, &img, (uint16_t)GOGO_TABLE_CAP) != 0) {
        free(words);
        fprintf(stderr, "gogovm: out of memory\n");
        return 1;
    }
    rc = vm_run(&vm);
    fflush(stdout);
    free(words);
    return rc;
}
