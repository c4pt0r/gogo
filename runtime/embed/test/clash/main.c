/* A host whose own functions have the names of libgogo's internal ones
 * (msg_free, heap_alloc, vm_step, bytes_new, port_open). It must link
 * against libgogo.a, which exports only the gogo_* API, and run an image;
 * each side must keep calling its own functions. make test-libgogo builds
 * it (outside test_embed, which links the runtime's sources directly). */
#include "gogo.h"
#include <stdio.h>
#include <stdlib.h>

int msg_free(int x) { return x + 1; }
int heap_alloc(int x) { return x * 2; }
int vm_step(int x) { return x - 3; }
int bytes_new(int x) { return x + 4; }
int port_open(int x) { return x * 5; }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s IMAGE.ggb\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *img = n > 0 ? malloc((size_t)n) : NULL;
    if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(img); return 1; }
    fclose(f);
    gogo_vm *vm = NULL;
    int rc = gogo_new_posix(NULL, &vm);
    if (rc == GOGO_OK) rc = gogo_load(vm, img, (size_t)n, GOGO_LOAD_COPY);
    free(img);
    if (rc == GOGO_OK) rc = gogo_run(vm);
    gogo_free(vm);
    int mine = msg_free(1) == 2 && heap_alloc(2) == 4 && vm_step(3) == 0 && bytes_new(0) == 4 && port_open(1) == 5;
    fprintf(stderr, "run %d, host functions %s\n", rc, mine ? "ok" : "WRONG");
    return rc == GOGO_OK && mine ? 0 : 1;
}
