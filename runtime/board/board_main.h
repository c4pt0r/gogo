/* One boot path for a board's firmware (ESP32-S3) and the unix VM: from
 * an image to a running VM on the board's hardware, in the order the
 * subsystems need. Pure C99; not part of libgogo (an embedded VM boots
 * through gogo_new / gogo_load instead).
 *
 * The board keeps every piece of state (runtime/ keeps none): the Vm
 * (too big for most stacks), its VfsState, InputState and NetState, in a
 * BoardRun it fills, plus hooks for what only it does. */
#ifndef GOGO_BOARD_MAIN_H
#define GOGO_BOARD_MAIN_H

#include <stdint.h>
#include "host.h"

struct Vm;
struct VfsState;
struct InputState;
struct NetState;

typedef struct BoardRun {
    struct Vm *vm;
    uint16_t table_cap;                /* vm_init's actor table size (GOGO_TABLE_CAP) */
    struct VfsState *vfs;              /* NULL: no file system builtin */
    struct InputState *input;          /* NULL: no input source */
    struct NetState *net;              /* NULL: no network in this run */
    /* net_install (runtime/net), set by a build that has the network: a
     * pointer, so a build without it links no net code. */
    void (*net_install)(struct Vm *vm, struct NetState *st);
    /* Optional hooks, each with ctx:
     *  - booting: after vm_init, before vm_boot (unix: the wall clock);
     *  - ready: every subsystem attached, before the run;
     *  - run: runs the VM, returns its result (NULL: vm_run). */
    void (*booting)(struct Vm *vm, void *ctx);
    void (*ready)(struct Vm *vm, void *ctx);
    int (*run)(struct Vm *vm, void *ctx);
    void *ctx;
} BoardRun;

/* Boots words (nwords, the image) as module 0 on board b with host h
 * (its ctx NULL), and runs it: vm_init, booting, vm_boot (the requirement
 * check), then net_install (with r->net), vfs_init (mounting the board's
 * file systems), input_install, in that order, then ready and the run.
 * Returns VM_BOOT_OK after the run, with its result in *run_rc; else the
 * boot's error, with nothing attached and nothing run: VM_BOOT_NO_MEMORY,
 * VM_BOOT_REFUSED (the refusal line is written) or an image error
 * (IMG_ERR_*, negative). */
int gogo_board_main(const VmBoard *b, const gogo_host *h, BoardRun *r, const uint32_t *words, uint32_t nwords,
                    int *run_rc);

#endif
