/* make test-gogoc: libgogoc.a (gogoc.h) from C, beside libgogo.a.
 *
 *   test_gogoc DIR     DIR: examples/many_idle (compiled from its path)
 *
 * Compiles a source string and runs it on a VM, compiles a directory,
 * checks the failures and warnings, compiles from several threads at
 * once, and defines functions named like the Go runtime's C symbols,
 * which libgogoc.a keeps local (the link would fail otherwise). */
#define _POSIX_C_SOURCE 200809L
#include "gogoc.h"
#include "gogo_posix.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Named like symbols of the Go runtime's cgo support. */
void crosscall2(void) {}
void fatalf(void) {}
void threadentry(void) {}
void x_cgo_init(void) {}

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char hello[] =
    "package main\n"
    "\n"
    "func main() {\n"
    "\tprintln(\"compiled in process\", 6*7)\n"
    "}\n";

/* The VM's output, collected. */
static char out[256];
static size_t out_len;
static void collect(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    if (stream == 1 && out_len + n < sizeof out) {
        memcpy(out + out_len, s, n);
        out_len += n;
    }
}

static void run_hello(void) {
    uint8_t *img = NULL;
    size_t n = 0;
    char *err = (char *)1;
    CHECK(gogoc_compile(hello, strlen(hello), GOGOC_SOURCE, &img, &n, &err) == 0);
    CHECK(img != NULL && n > 0 && err == NULL);
    gogo_image_info info = GOGO_IMAGE_INFO_INIT;
    CHECK(gogo_image_info_get(img, n, &info) == GOGO_OK);   /* malloc's alignment */

    gogo_host h = *gogo_host_posix();
    h.write = collect;
    void *ctx = gogo_posix_ctx_new();
    gogo_vm *vm = NULL;
    CHECK(ctx && gogo_new(&h, ctx, NULL, &vm) == GOGO_OK);
    CHECK(gogo_load(vm, img, n, GOGO_LOAD_COPY) == GOGO_OK);
    gogoc_free(img);
    CHECK(gogo_run(vm) == GOGO_OK);
    gogo_free(vm);
    gogo_posix_ctx_free(ctx);
    CHECK(out_len == strlen("compiled in process 42\n") && memcmp(out, "compiled in process 42\n", out_len) == 0);
}

static void failures_and_warnings(void) {
    uint8_t *img = (uint8_t *)1;
    size_t n = 1;
    char *err = NULL;
    static const char bad[] = "package main\n\nfunc main() {\n\tnope()\n}\n";
    CHECK(gogoc_compile(bad, strlen(bad), GOGOC_SOURCE, &img, &n, &err) == -1);
    CHECK(img == NULL && n == 0);
    CHECK(err && strncmp(err, "main.gg:4:2: ", 13) == 0);
    gogoc_free(err);

    /* err may be NULL; a failure is still -1. */
    CHECK(gogoc_compile(bad, strlen(bad), GOGOC_SOURCE, &img, &n, NULL) == -1);
    /* Bad arguments. */
    CHECK(gogoc_compile(hello, strlen(hello), GOGOC_SOURCE, NULL, &n, &err) == -1 && err);
    gogoc_free(err);
    CHECK(gogoc_compile(hello, strlen(hello), 0x80, &img, &n, &err) == -1 && err);
    gogoc_free(err);
    CHECK(gogoc_compile(NULL, 3, GOGOC_SOURCE, &img, &n, &err) == -1 && err);
    gogoc_free(err);
    CHECK(gogoc_compile("/no/such/dir", 12, 0, &img, &n, &err) == -1 && err);
    gogoc_free(err);

    /* Not NUL-terminated: only input_len bytes are read. */
    char buf[sizeof hello + 4];
    memcpy(buf, hello, strlen(hello));
    memcpy(buf + strlen(hello), "}}}}", 4);
    CHECK(gogoc_compile(buf, strlen(hello), GOGOC_SOURCE, &img, &n, NULL) == 0);
    gogoc_free(img);

    /* A warning comes with the image. */
    static const char warn[] = "package main\n\nimport \"gogo/port\"\n\n"
                               "func open(name string) { _, _ = port.Open(name, port.Host{}) }\n\n"
                               "func main() { open(\"x\") }\n";
    CHECK(gogoc_compile(warn, strlen(warn), GOGOC_SOURCE, &img, &n, &err) == 0);
    CHECK(img && err && strncmp(err, "main.gg:5:", 10) == 0 && err[strlen(err) - 1] == '\n');
    gogoc_free(img);
    gogoc_free(err);
    gogoc_free(NULL);
}

static void directory(const char *dir) {
    uint8_t *img = NULL;
    size_t n = 0;
    char *err = NULL;
    CHECK(gogoc_compile(dir, strlen(dir), GOGOC_GOGOPATH, &img, &n, &err) == 0);
    if (err) fprintf(stderr, "%s\n", err);
    gogo_image_info info = GOGO_IMAGE_INFO_INIT;
    CHECK(img && gogo_image_info_get(img, n, &info) == GOGO_OK);
    uint32_t emits = gogoc_image_format(), runs = gogo_image_format();
    CHECK(emits >> 16 == runs >> 16 && (emits & 0xFFFF) <= (runs & 0xFFFF));
    CHECK(info.format >> 16 == emits >> 16 && (info.format & 0xFFFF) <= (emits & 0xFFFF));
    gogoc_free(img);
    gogoc_free(err);
}

static void *compile_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        uint8_t *img = NULL;
        size_t n = 0;
        if (gogoc_compile(hello, strlen(hello), GOGOC_SOURCE, &img, &n, NULL) != 0) return (void *)1;
        gogoc_free(img);
    }
    return NULL;
}

static void threads(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++) CHECK(pthread_create(&t[i], NULL, compile_thread, NULL) == 0);
    for (int i = 0; i < 4; i++) {
        void *r = (void *)1;
        pthread_join(t[i], &r);
        CHECK(r == NULL);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    run_hello();
    failures_and_warnings();
    directory(argv[1]);
    threads();
    if (failures) return 1;
    printf("test-gogoc: ok\n");
    return 0;
}
