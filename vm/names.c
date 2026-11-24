#include "names.h"
#include "alloc.h"
#include <string.h>

struct Name {
    struct Name *next;
    term pid;
    uint32_t len;
    uint8_t bytes[];          /* len bytes, no terminator */
};

static size_t name_size(uint32_t len) { return sizeof(Name) + len; }

/* The link that points at the entry for name, or NULL if there is none. */
static Name **find(Vm *vm, const uint8_t *name, uint32_t len) {
    for (Name **p = &vm->names; *p; p = &(*p)->next) {
        if ((*p)->len == len && (len == 0 || memcmp((*p)->bytes, name, len) == 0)) return p;
    }
    return NULL;
}

static void unlink_free(Name **p) {
    Name *n = *p;
    *p = n->next;
    va_free(n, name_size(n->len));
}

int names_register(Vm *vm, const uint8_t *name, uint32_t len, term pid) {
    const Actor *a = actor_lookup(vm, pid);
    if (!a || a->state == A_EXITED || (a->flags & AF_EXIT_PENDING)) return 0;
    if (find(vm, name, len)) return 0;
    Name *n = va_alloc(name_size(len));
    if (!n) return -1;
    n->pid = pid;
    n->len = len;
    if (len) memcpy(n->bytes, name, len);
    n->next = vm->names;
    vm->names = n;
    return 1;
}

bool names_whereis(Vm *vm, const uint8_t *name, uint32_t len, term *pid) {
    Name **p = find(vm, name, len);
    if (!p) return false;
    *pid = (*p)->pid;
    return true;
}

void names_unregister(Vm *vm, const uint8_t *name, uint32_t len) {
    Name **p = find(vm, name, len);
    if (p) unlink_free(p);
}

void names_drop_actor(Vm *vm, term pid) {
    Name **p = &vm->names;
    while (*p) {
        if ((*p)->pid == pid) unlink_free(p);
        else p = &(*p)->next;
    }
}
