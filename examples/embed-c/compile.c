/* compile: compile gogo source inside a C program and run it
 * (libgogoc.a, gogoc.h; then libgogo.a, gogo.h).
 *
 *   compile DIR | FILE.gg     the program at a path, as gogo build reads it
 *   compile -e SOURCE         a program given as text (one file, package main)
 *
 * The compiler's warnings go to stderr; the program's output to stdout.
 * Build it with `make example-embed` (build/embed-c/compile); it links
 * both libraries, and so carries the Go runtime (gogoc.h). */
#define _POSIX_C_SOURCE 200809L
#include "gogoc.h"
#include "gogo_posix.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int source = argc == 3 && strcmp(argv[1], "-e") == 0;
    if (argc != 2 && !source) {
        fprintf(stderr, "usage: %s DIR | FILE.gg | -e SOURCE\n", argv[0]);
        return 2;
    }
    const char *in = argv[argc - 1];

    uint8_t *img;
    size_t len;
    char *msg;
    if (gogoc_compile(in, strlen(in), source ? GOGOC_SOURCE : 0, &img, &len, &msg) != 0) {
        fprintf(stderr, "%s\n", msg);   /* "main.gg:3:5: ..." */
        gogoc_free(msg);
        return 1;
    }
    if (msg) fputs(msg, stderr);        /* warnings, one per line */
    gogoc_free(msg);

    gogo_vm *vm = NULL;
    int rc = gogo_new_posix(NULL, &vm);
    if (rc == GOGO_OK) rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);
    gogoc_free(img);                    /* the VM has its own copy */
    if (rc == GOGO_OK) rc = gogo_run(vm);
    gogo_free(vm);
    if (rc != GOGO_OK) fprintf(stderr, "compile: %s (%d)\n", gogo_strerror(rc), rc);
    return rc == GOGO_OK ? 0 : 1;
}
