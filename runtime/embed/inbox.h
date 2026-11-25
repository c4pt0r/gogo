/* The post inbox (gogo_post): a lock-free multi-producer stack of posts.
 * Producers (any thread) push with a CAS; the VM thread takes the whole
 * stack with one exchange, reverses it (oldest first) and delivers each
 * post as port.Post{Kind, Data} to the actor registered under its name.
 * The inbox is one of the Vm's event sources (port.h PortSource), so the
 * scheduler drains it before it goes idle, and a push after its last
 * check wakes the host (latched). */
#ifndef GOGO_INBOX_H
#define GOGO_INBOX_H

#include <stdint.h>
#include <stddef.h>
#include "port.h"

struct Vm;

typedef struct InboxNode {
    struct InboxNode *next;
    size_t bytes;                /* the allocation's size (host free) */
    int32_t kind;
    uint32_t name_len, data_len;
    uint8_t text[];              /* the name, then the data */
} InboxNode;

typedef struct Inbox {
    InboxNode *head;             /* atomic: newest first */
    int depth;                   /* atomic: posts accepted and not yet taken */
    uint64_t delivered, dropped; /* VM thread */
} Inbox;

/* The source; its ctx is the gogo_vm. */
extern const PortSource inbox_source;

/* Pushes a node (from the host's alloc) for the VM: 0, GOGO_EFULL,
 * GOGO_ENOMEM. Any thread. Does not wake. */
int inbox_push(Inbox *in, const gogo_host *h, void *hctx, const char *name, size_t name_len,
               int32_t kind, const void *data, size_t len);

/* Frees every node still queued (the VM is going away; no producer left). */
void inbox_clear(Inbox *in, const gogo_host *h, void *hctx);

#endif
