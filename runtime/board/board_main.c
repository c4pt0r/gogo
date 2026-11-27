/* The one boot path (board_main.h). */
#include "board_main.h"
#include "actor.h"
#include "alloc.h"
#include "interp.h"
#include "module.h"
#include "vfs.h"
#include "input.h"
#include "port.h"
#include "params.h"
#if GOGO_BOARD_PORTS
#include "board_ports.h"
#endif
#include <stdio.h>

int gogo_board_main(const VmBoard *b, const gogo_host *h, BoardRun *r, const uint32_t *words, uint32_t nwords,
                    int *run_rc) {
    /* The unified boot (module.h): the image becomes module 0
     * (requirement check included); the run starts its "main" export. */
    VmParams own, def;
    const VmParams *p = r->params;
    int rc;
    if (r->defaults) def = *r->defaults;
    else vm_params_default(&def);
    if (!p) {
        own = def;
        vm_params_resolve(h, NULL, &own);
        p = &own;
    }
#if GOGO_BOARD_PORTS
    struct EmbedPorts *ports = board_ports_new(h, r->ports_host ? r->ports_host : h);   /* NULL: none */
    board_ports_expect(b, r->ports_expected, h);
#endif
    for (;;) {
        rc = VM_BOOT_NO_MEMORY;
        if (vm_init(r->vm, h, NULL, b, (uint16_t)p->max_actors) == 0) {
            int ok = vm_params_apply(r->vm, p) == 0;
#if GOGO_BOARD_PORTS
            ok = ok && (!ports || board_ports_attach(ports, r->vm, h) == 0);
#endif
            if (ok) {
                if (r->booting) r->booting(r->vm, r->ctx);
                rc = vm_boot(r->vm, words, nwords);   /* a refusal gives the table back */
            } else {
                va_free(r->vm, r->vm->table, (size_t)r->vm->table_cap * sizeof(uintptr_t));
            }
        }
        /* Out of memory with parameters the keys changed: say which, and
         * boot once more with the defaults, so a bad key never keeps a
         * board from booting (or from answering gogo config). A failed
         * try leaves nothing allocated: the table is the one thing taken
         * before a failure point, and it is given back. */
        if (rc != VM_BOOT_NO_MEMORY || p == &def) break;
        char keys[128], m[180];
        if (!params_changed(p, &def, h, NULL, keys, sizeof keys)) break;
        size_t n = (size_t)snprintf(m, sizeof m, "gogo: out of memory with %s as set; starting with the defaults\n", keys);
        h->write(NULL, 2, m, n < sizeof m ? n : sizeof m - 1u);
        p = &def;
    }
    if (rc != VM_BOOT_OK) return rc;
#if GOGO_BOARD_PORTS
    if (ports) board_ports_loaded(ports);
#endif
    /* After vm_init: it starts the Vm's source list and fs hook empty. */
    if (r->net && r->net_install) r->net_install(r->vm, r->net);
    if (r->vfs) vfs_init(r->vm, r->vfs);
    if (r->input) input_install(r->vm, r->input);
    if (r->ready) r->ready(r->vm, r->ctx);
    *run_rc = r->run ? r->run(r->vm, r->ctx) : vm_run(r->vm);
#if GOGO_BOARD_PORTS
    if (ports) board_ports_end(ports);
#endif
    return VM_BOOT_OK;
}
