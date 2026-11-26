/* The one boot path (board_main.h). */
#include "board_main.h"
#include "actor.h"
#include "interp.h"
#include "module.h"
#include "vfs.h"
#include "input.h"
#include "port.h"
#include <string.h>

/* The decimal value of config key k, or 0 (unset or not a number). */
static uint32_t config_num(const struct Vm *vm, const char *k) {
    uint8_t b[12];
    int32_t n = vm_config_get(vm, (const uint8_t *)k, (uint32_t)strlen(k), b, sizeof b);
    uint32_t v = 0;
    if (n <= 0 || n > 6) return 0;
    for (int32_t i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return 0;
        v = v * 10u + (uint32_t)(b[i] - '0');
    }
    return v;
}

/* Sets the caps (board_main.h, BoardRun.max_ports). */
static void set_caps(struct Vm *vm, const gogo_host *h, const BoardRun *r) {
    uint32_t ports = r->max_ports ? r->max_ports : config_num(vm, "vm.max_ports");
    uint32_t links = r->max_links ? r->max_links : config_num(vm, "node.max_links");
    if (!ports) ports = r->default_ports;
    if (!links) links = r->default_links;
    if (vm_set_max_ports(vm, ports) != 0) {
        static const char m[] = "gogo: vm.max_ports refused (at most 2048, and actors + ports at most 65536)\n";
        h->write(NULL, 2, m, sizeof m - 1u);
        vm_set_max_ports(vm, r->default_ports);
    }
    vm->max_links = (uint16_t)(links > PORT_MAX_CEILING ? PORT_MAX_CEILING : links);
}

int gogo_board_main(const VmBoard *b, const gogo_host *h, BoardRun *r, const uint32_t *words, uint32_t nwords,
                    int *run_rc) {
    /* The unified boot (module.h): the image becomes module 0
     * (requirement check included); the run starts its "main" export. */
    if (vm_init(r->vm, h, NULL, b, r->table_cap) != 0) return VM_BOOT_NO_MEMORY;
    set_caps(r->vm, h, r);
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
