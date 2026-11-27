/* dice-host: the dice driver (dice/dice.c, unchanged) as a host port of a
 * program that embeds gogo: registered with gogo_register_port instead of
 * GOGO_BOARD_PORT, then the image runs as on a board that has it.
 *
 *   dice-host IMAGE.ggb */
#include <stdio.h>
#include <stdlib.h>
#include "gogo_posix.h"
#include "dice/dice.h"

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: dice-host IMAGE.ggb\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static char img[1 << 20];
    size_t len = fread(img, 1, sizeof img, f);
    fclose(f);
    static DiceTable table;
    gogo_vm *vm = NULL;
    int rc = gogo_new_posix(NULL, &vm);
    if (rc == GOGO_OK) rc = gogo_register_port(vm, "dice", 4, &dice_driver, &table);   /* before the load */
    if (rc == GOGO_OK) rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);
    if (rc == GOGO_OK) rc = gogo_run(vm);
    if (rc != GOGO_OK) fprintf(stderr, "dice-host: %s\n", gogo_strerror(rc));
    gogo_free(vm);
    return rc == GOGO_OK ? 0 : 1;
}
