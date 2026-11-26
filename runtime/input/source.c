/* The input event source (spec input §5.1): reads the keyboard chip, samples
 * the power chip and polls the other devices (through the HAL of each open
 * port's driver instance) on the scheduler thread, and delivers
 * straight to the port owners; it never touches the event ring. */
#include "input_int.h"
#include "host.h"
#include <stdio.h>
#include <string.h>

/* Any input port open (no clock read for programs without input). */
static bool any_open(const InputState *in) {
    return in->kbd.port || in->pwr.port || in->imu.port || in->ir.tx || in->ir.rx || in->au.mic || in->au.spk;
}

static bool src_pending(const Vm *vm, void *ctx) {
    (void)ctx;
    if (!any_open(vm->input)) return false;
    uint64_t now = vm_now(vm);
    return kbd_work(vm, now) || power_work(vm, now) || imu_work(vm, now) || ir_work(vm, now) || audio_work(vm, now);
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
    if (!any_open(vm->input)) return false;
    uint64_t now = vm_now(vm), t;
    bool any = false;
    if (kbd_next_deadline(vm, now, &t)) { *d = t; any = true; }
    if (power_next_deadline(vm, now, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (imu_next_deadline(vm, now, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (ir_next_deadline(vm, now, &t) && (!any || t < *d)) { *d = t; any = true; }
    if (audio_next_deadline(vm, now, &t) && (!any || t < *d)) { *d = t; any = true; }
    return any;
}

static const PortSource input_source = { src_pending, src_drain, src_any_live, src_next_deadline, NULL };

void input_install(Vm *vm, InputState *st) {
    memset(st, 0, sizeof *st);
    vm->input = st;
    st->failed = !port_add_source(vm, &input_source, NULL);
    st->installed = !st->failed;
    if (st->failed) fprintf(stderr, "gogo: input: no free event-source slot, kbd, power, imu, ir, mic and spk disabled\n");
}

void input_uninstall(Vm *vm) {   /* tests */
    InputState *st = vm->input;
    if (!st) return;
    if (st->installed) port_remove_source(vm, &input_source);
    st->installed = st->failed = false;
}

bool input_failed(const Vm *vm) { return !vm->input || vm->input->failed; }
uint32_t input_dropped(const Vm *vm) { return vm->input ? vm->input->dropped : 0; }
uint32_t input_read_errors(const Vm *vm) { return vm->input ? vm->input->read_errors : 0; }
