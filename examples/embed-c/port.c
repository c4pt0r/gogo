/* port: a port defined by the host (gogo_register_port).
 *
 *   port hostecho.ggb
 *
 * Lists what the image requires, registers a driver named "echo" if it is
 * among them, and runs examples/hostecho on it. Its open refuses a
 * negative Arg and otherwise answers with an event (Kind = Arg, Data =
 * the config's Data); every port.Post the program sends to it comes back
 * as an event with Kind + 1 and the same Data, except Kind 100, which
 * ends the port from the host's side. */
#define _POSIX_C_SOURCE 200809L
#include "gogo_posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Whether the image requires want (a driver or mount name), listing every
 * requirement on stderr. */
static int requires(const void *img, size_t len, const char *want) {
    gogo_name names[16];
    uint32_t n;
    int found = 0;
    if (gogo_image_requires(img, len, names, 16, &n) != GOGO_OK) return 0;
    for (uint32_t i = 0; i < n && i < 16; i++) {
        fprintf(stderr, "requires %.*s\n", (int)names[i].len, names[i].ptr);
        if (names[i].len == strlen(want) && memcmp(names[i].ptr, want, names[i].len) == 0) found = 1;
    }
    return found;
}

/* port.Open("echo", port.Host{Arg, Data}). Events emitted here reach the
 * owner once the open succeeds. */
static int echo_open(void *ctx, gogo_vm *vm, gogo_port p, int32_t arg, const uint8_t *data, size_t len) {
    (void)ctx;
    if (arg < 0) return GOGO_EINVAL;   /* refused: port.Open fails with errors.BadArg */
    gogo_port_emit(vm, p, arg, data, len);
    return 0;
}

/* actor.Send(p, port.Post{Kind, Data}) */
static void echo_command(void *ctx, gogo_vm *vm, gogo_port p, int32_t kind, const uint8_t *data, size_t len) {
    int *commands = ctx;
    ++*commands;
    if (kind == 100) {
        gogo_port_close(vm, p, GOGO_E_IO);   /* the owner gets port.Closed{p, errors.IO} */
        return;
    }
    gogo_port_emit(vm, p, kind + 1, data, len);
}

static const gogo_port_driver echo_driver = {
    .size = sizeof(gogo_port_driver),
    .open = echo_open,
    .command = echo_command,
};

int main(int argc, char **argv) {
    size_t len;
    void *img = argc == 2 ? read_file(argv[1], &len) : NULL;
    if (!img) { fprintf(stderr, "usage: %s hostecho.ggb\n", argv[0]); return 2; }
    int commands = 0;
    gogo_vm *vm = NULL;
    int rc = gogo_new_posix(NULL, &vm);
    if (rc == GOGO_OK && requires(img, len, "echo"))
        rc = gogo_register_port(vm, "echo", 4, &echo_driver, &commands);   /* before load */
    if (rc == GOGO_OK) rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);
    free(img);
    if (rc == GOGO_OK) rc = gogo_run(vm);
    gogo_free(vm);
    fprintf(stderr, "%d commands\n", commands);
    return rc == GOGO_OK ? 0 : 1;
}
