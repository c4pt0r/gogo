/* The input event source (spec input §5.1): reads the keyboard chip and samples
 * the power chip on the scheduler thread and delivers
 * straight to the port owners; it never touches the event ring. */
#include "input_int.h"
#include "platform.h"
#include <stdio.h>

static const InputHw none;   /* no hardware */
__attribute__((weak)) const InputHw *platform_input(void) { return &none; }

static bool src_pending(void) {
    if (!kbd_is_open() && !power_is_open()) return false;   /* no clock read for programs without input */
    uint64_t now = platform_now_ms();
    return kbd_work(now) || power_work(now);
}

static void src_drain(Vm *vm, uint64_t now) {
    kbd_drain(vm, now);
    power_drain(vm, now);
}

static bool src_any_live(const Vm *vm) { (void)vm; return false; }   /* open ports keep the VM alive */

static bool src_next_deadline(const Vm *vm, uint64_t *d) {
    (void)vm;
    uint64_t kd, pd;
    if (!kbd_is_open() && !power_is_open()) return false;
    if (platform_input()->live && !platform_input()->live()) return false;   /* dead hardware: no repeat or sample is coming */
    uint64_t now = platform_now_ms();
    bool k = kbd_next_deadline(now, &kd), p = power_next_deadline(now, &pd);
    if (!k && !p) return false;
    *d = !p || (k && kd < pd) ? kd : pd;
    return true;
}

static const PortSource input_source = { src_pending, src_drain, src_any_live, src_next_deadline, NULL };

static bool installed, failed;

void input_install(void) {
    if (installed) return;
    failed = !port_add_source(&input_source);
    installed = !failed;
    if (failed) fprintf(stderr, "gogo: input: no free event-source slot, kbd and power disabled\n");
}

void input_uninstall(void) {   /* tests */
    if (installed) port_remove_source(&input_source);
    installed = failed = false;
}

bool input_failed(void) { return failed; }
