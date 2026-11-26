/* post: send messages into a running VM (gogo_post, gogo_close_posts).
 *
 *   post greeter.ggb
 *
 * Runs examples/embed-c/greeter, which registers the name "greeter". The
 * main thread steps the VM until it is idle (the name is registered by
 * then), posts three names, and hands a second thread a reference that
 * posts two more and closes posts; meanwhile the main thread runs the VM
 * to the end with gogo_run. */
#define _POSIX_C_SOURCE 200809L
#include "gogo.h"
#include <pthread.h>
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

static int greet(gogo_vm *vm, const char *who) {
    return gogo_post(vm, "greeter", 1, who, strlen(who));
}

/* Another thread: it may post because it holds a reference (gogo_retain). */
static void *poster(void *arg) {
    gogo_vm *vm = arg;
    greet(vm, "Edsger");
    greet(vm, "Barbara");
    gogo_close_posts(vm);   /* no more posts: the VM ends when its actors do */
    gogo_release(vm);
    return NULL;
}

int main(int argc, char **argv) {
    size_t len;
    void *img = argc == 2 ? read_file(argv[1], &len) : NULL;
    if (!img) { fprintf(stderr, "usage: %s greeter.ggb\n", argv[0]); return 2; }
    gogo_vm *vm = gogo_new_posix(NULL);
    if (!vm || gogo_load(vm, img, len, GOGO_LOAD_COPY) != GOGO_OK || gogo_start(vm) != GOGO_OK) {
        fprintf(stderr, "cannot start %s\n", argv[1]);
        free(img);
        gogo_free(vm);
        return 1;
    }
    free(img);

    /* Run main until it waits: by then it has registered "greeter" (a post
     * to a name nobody holds yet is dropped). */
    while (gogo_step(vm, 0) == GOGO_RUNNABLE) {}
    greet(vm, "Ada");
    greet(vm, "Grace");
    gogo_post(vm, "greeter", 2, "Alan", 4);

    pthread_t t;
    gogo_retain(vm);   /* the thread's reference */
    int started = pthread_create(&t, NULL, poster, vm) == 0;
    if (!started) {
        gogo_release(vm);
        gogo_close_posts(vm);
    }
    int rc = gogo_run(vm);   /* returns once posts are closed and the greeter idles */
    if (started) pthread_join(t, NULL);

    gogo_stats st;
    gogo_get_stats(vm, &st);
    printf("posts delivered: %llu, dropped: %llu\n",
           (unsigned long long)st.posts_delivered, (unsigned long long)st.posts_dropped);
    gogo_free(vm);
    return rc == GOGO_OK ? 0 : 1;
}
