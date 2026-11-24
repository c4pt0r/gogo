/* gogovm: runs a gogo image file.
 *
 *   gogovm [--key c=pin]... <file.gogo>   exit 0 when the run queue empties, 2 on load error
 *   gogovm --sizeof-actor  prints sizeof(Actor)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "actor.h"
#include "interp.h"
#include "config.h"
#include "gpio_unix.h"


static int load_error(const char *path, const char *what) {
    fprintf(stderr, "gogovm: %s: %s\n", path, what);
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--sizeof-actor") == 0) {
        printf("%zu\n", sizeof(Actor));
        return 0;
    }
    /* --key <c>=<pin> (tty mode): map a key to a simulated gpio pin. */
    while (argc >= 3 && strcmp(argv[1], "--key") == 0) {
        char c;
        int pin;
        char tail;
        if (sscanf(argv[2], "%c=%d%c", &c, &pin, &tail) != 2 || !gpio_unix_add_key(c, pin)) {
            fprintf(stderr, "gogovm: bad --key %s (want <char>=<pin>, pin 0..63)\n", argv[2]);
            return 2;
        }
        argv += 2;
        argc -= 2;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: gogovm [--key <c>=<pin>]... <file.gogo>\n       gogovm --sizeof-actor\n");
        return 2;
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
