/* Wall clock (platform.h), per Vm: Unix time as an offset against the
 * monotonic platform_now_ms, unknown until set. vm_run sets it from
 * platform_time_reset's result each time it restarts the monotonic clock. */
#include "platform.h"
#include "actor.h"

int64_t vm_unix_time_ms(const Vm *vm) {
    return vm->wall_known ? vm->wall_offset + (int64_t)platform_now_ms() : 0;
}

int vm_set_unix_time_ms(Vm *vm, int64_t ms) {
    if (ms > 0 && ms < GOGO_BUILD_UNIX_MS) return 0;
    vm->wall_known = ms > 0;
    vm->wall_offset = vm->wall_known ? ms - (int64_t)platform_now_ms() : 0;
    return 1;
}
