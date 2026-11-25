/* The input event source (spec input §5.1): reads the keyboard chip and samples
 * the power chip on the scheduler thread and delivers
 * straight to the port owners; it never touches the event ring. */
#include "input_int.h"
#include "host.h"
#include <stdio.h>

static const InputHw none;   /* no hardware */
const InputHw *input_hw(const Vm *vm) { return vm->board->input ? vm->board->input : &none; }

static bool src_pending(const Vm *vm, void *ctx) {
    (void)ctx;
    if (!kbd_is_open() && !power_is_open() && !imu_is_open() && !ir_is_open() && !audio_is_open()) return false;   /* no clock read for programs without input */
    uint64_t now = vm_now(vm);
    return kbd_work(vm, now) || power_work(vm, now) || imu_work(now) || ir_work(now) || audio_work(now);
}

static void src_drain(Vm *vm, void *ctx, uint64_t now) {
    (void)ctx;
    kbd_drain(vm, now);
    power_drain(vm, now);
    imu_drain(vm, now);
    ir_drain(vm, now);
    audio_drain(vm, now);
}

static bool src_any_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }   /* open ports keep the VM alive */

static bool src_next_deadline(const Vm *vm, void *ctx, uint64_t *d) {
    (void)ctx;
    if (!kbd_is_open() && !power_is_open() && !imu_is_open() && !ir_is_open() && !audio_is_open()) return false;
    /* Dead hardware (live false): no repeat, sample or receive poll is
     * coming; an IR frame being sent still finishes (its Done). */
    bool live = !input_hw(vm)->live || input_hw(vm)->live();
    uint64_t now = vm_now(vm), t;
    bool any = false;
    if (live && kbd_next_deadline(vm, now, &t)) { *d = t; any = true; }
    if (live && power_next_deadline(vm, now, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (live && imu_next_deadline(now, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (ir_next_deadline(now, live, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (audio_next_deadline(now, live, &t) && (!any || t < *d)) { *d = t; any = true; }
    return any;
}

static const PortSource input_source = { src_pending, src_drain, src_any_live, src_next_deadline, NULL };

static bool installed, failed;

void input_install(Vm *vm) {
    failed = !port_add_source(vm, &input_source, NULL);
    installed = !failed;
    if (failed) fprintf(stderr, "gogo: input: no free event-source slot, kbd, power, imu, ir, mic and spk disabled\n");
}

void input_uninstall(Vm *vm) {   /* tests */
    if (installed) port_remove_source(vm, &input_source);
    installed = failed = false;
}

bool input_failed(void) { return failed; }
