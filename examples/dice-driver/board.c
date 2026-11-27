/* dice-board: a test-only VM whose board has one port driver, "dice"
 * (dice.c). It boots an image the way gogovm and the firmware do
 * (gogo_board_main) on the unix host, so the image's requirements are
 * checked against this board's driver list.
 *
 *   dice-board IMAGE.ggb
 *
 * Exit status: 0 after the run, 3 if the image requires something this
 * board lacks, 2 for an image that cannot be read or loaded. */
#include <stdio.h>
#include <stdlib.h>

#include "actor.h"
#include "config.h"
#include "board_main.h"
#include "module.h"
#include "platform_unix.h"

extern const PortDriver dice_driver;

/* What this board provides: the drivers of its ports, and no mounts. */
static const PortDriver *const dice_ports[] = { &dice_driver, NULL };
static const VmMount *const no_mounts[] = { NULL };
static const VmBoard dice_board = { .ports = dice_ports, .mounts = no_mounts };

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: dice-board IMAGE.ggb\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *words = size > 0 && size % 4 == 0 ? malloc((size_t)size) : NULL;
    if (!words || fread(words, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "dice-board: cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    static Vm vm;   /* too big for the stack */
    /* No file system (vfs NULL: gogo/fs is not linked in); params NULL:
     * the build's defaults and config keys. */
    BoardRun run = { .vm = &vm };
    int run_rc = 0;
    int rc = gogo_board_main(&dice_board, &unix_host, &run, words, (uint32_t)(size / 4), &run_rc);
    fflush(stdout);
    free(words);
    if (rc == VM_BOOT_REFUSED) return 3;   /* the VM wrote the refusal line */
    if (rc != VM_BOOT_OK) { fprintf(stderr, "dice-board: boot error %d\n", rc); return 2; }
    return 0;
}
