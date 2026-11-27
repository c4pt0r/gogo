/* The one boot path (board_main.h). */
#include "board_main.h"
#include "actor.h"
#include "interp.h"
#include "module.h"
#include "vfs.h"
#include "input.h"
#include "port.h"
#include "params.h"

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
    rc = VM_BOOT_NO_MEMORY;
    if (vm_init(r->vm, h, NULL, b, (uint16_t)p->max_actors) == 0 && vm_params_apply(r->vm, p) == 0) {
        if (r->booting) r->booting(r->vm, r->ctx);
        rc = vm_boot(r->vm, words, nwords);
    }
    if (rc != VM_BOOT_OK) return rc;
    /* After vm_init: it starts the Vm's source list and fs hook empty. */
    if (r->net && r->net_install) r->net_install(r->vm, r->net);
    if (r->vfs) vfs_init(r->vm, r->vfs);
    if (r->input) input_install(r->vm, r->input);
    if (r->ready) r->ready(r->vm, r->ctx);
    *run_rc = r->run ? r->run(r->vm, r->ctx) : vm_run(r->vm);
    return VM_BOOT_OK;
}
