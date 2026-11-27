/* The one boot path (board_main.h). */
#include "board_main.h"
#include "actor.h"
#include "interp.h"
#include "module.h"
#include "vfs.h"
#include "input.h"
#include "port.h"
#include "params.h"
#include <string.h>

/* ---- board_params_format ---- */

/* Decimal v at p; returns the end. */
static char *put_u(char *p, uint64_t v) {
    char d[20];
    int n = 0;
    do d[n++] = (char)('0' + v % 10u); while ((v /= 10u) != 0);
    while (n) *p++ = d[--n];
    return p;
}

static char *put_s(char *p, const char *s) {
    size_t n = strlen(s);
    memcpy(p, s, n);
    return p + n;
}

size_t board_params_format(const VmParams *p, char *buf, size_t cap) {
    char line[200], *q = line;
    uint64_t mem = p->memory;
    if (GOGO_ARENA_BYTES && (!mem || mem > GOGO_ARENA_BYTES)) mem = GOGO_ARENA_BYTES;
    q = vm_params_put_size(put_s(q, "actor_memory="), p->actor_memory);
    q = put_s(q, " memory=");
    q = mem ? vm_params_put_size(q, mem) : put_s(q, "unlimited");
    q = put_u(put_s(q, " max_actors="), p->max_actors);
    q = put_u(put_s(q, " max_ports="), p->max_ports);
    q = put_u(put_s(q, " max_links="), p->max_links ? p->max_links : GOGO_MAX_LINKS);
    q = vm_params_put_size(put_s(q, " code_mem="), p->code_mem);
    q = put_u(put_s(q, " bytes_handles="), p->bytes_handles);
    *q++ = '/';
    q = put_u(q, p->bytes_per_actor);
    size_t n = (size_t)(q - line);
    if (cap) {
        size_t k = n < cap ? n : cap - 1u;
        memcpy(buf, line, k);
        buf[k] = 0;
    }
    return n;
}

int gogo_board_main(const VmBoard *b, const gogo_host *h, BoardRun *r, const uint32_t *words, uint32_t nwords,
                    int *run_rc) {
    /* The unified boot (module.h): the image becomes module 0
     * (requirement check included); the run starts its "main" export. */
    VmParams own;
    const VmParams *p = r->params;
    if (!p) {
        vm_params_default(&own);
        vm_params_resolve(h, NULL, NULL, &own);
        p = &own;
    }
    if (vm_init(r->vm, h, NULL, b, (uint16_t)p->max_actors) != 0 || vm_params_apply(r->vm, p) != 0)
        return VM_BOOT_NO_MEMORY;
    if (r->booting) r->booting(r->vm, r->ctx);
    int rc = vm_boot(r->vm, words, nwords);
    if (rc != VM_BOOT_OK) return rc;
    /* After vm_init: it starts the Vm's source list and fs hook empty. */
    if (r->net && r->net_install) r->net_install(r->vm, r->net);
    if (r->vfs) vfs_init(r->vm, r->vfs);
    if (r->input) input_install(r->vm, r->input);
    if (r->ready) r->ready(r->vm, r->ctx);
    *run_rc = r->run ? r->run(r->vm, r->ctx) : vm_run(r->vm);
    return VM_BOOT_OK;
}
