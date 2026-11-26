/* The host and board wrappers (host.h): one out-of-line copy of each hook
 * call, with the defaults of the optional hooks, and the Vm's wall clock. */
#include "host.h"
#include "actor.h"


void vm_out(const Vm *vm, int stream, const char *s, size_t n) { vm->host->write(vm->host_ctx, stream, s, n); }

void *vm_chunk_alloc(Vm *vm, size_t n) { return vm->host->alloc(vm->host_ctx, n); }

int32_t vm_config_get(const Vm *vm, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    return vm->host->config_get ? vm->host->config_get(vm->host_ctx, k, kl, buf, cap) : -1;
}

void *vm_code_alloc(Vm *vm, size_t n) { return vm->host->code_alloc ? vm->host->code_alloc(vm->host_ctx, n) : NULL; }
void vm_code_free(Vm *vm, void *p, size_t n) { if (vm->host->code_free) vm->host->code_free(vm->host_ctx, p, n); }

void vm_wait(const Vm *vm, uint32_t ms) { vm->host->wait(vm->host_ctx, ms); }

/* The host's raw monotonic clock: the wall clock's base (not vm_now, so a
 * new time base keeps the wall clock). */
static uint64_t host_ms(const Vm *vm) { return vm->host->now_ms(vm->host_ctx); }

uint64_t vm_now(const Vm *vm) { return host_ms(vm) - vm->t0; }

/* Not for secrets: a host that needs good randomness gives random. */
uint32_t vm_random(Vm *vm) {
    if (vm->host->random) return vm->host->random(vm->host_ctx);
    uint32_t x = vm->rand_x + 0x9E3779B9u;
    x ^= (uint32_t)host_ms(vm) * 2654435761u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;       /* xorshift32 */
    return vm->rand_x = x;
}

/* Without an RTC: unix ms = wall_offset + host_ms, added in uint64 (a
 * time set near the int64 limit would overflow) and held at INT64_MAX. */
int64_t vm_unix_time_ms(const Vm *vm) {
    if (vm->host->unix_time_ms) return vm->host->unix_time_ms(vm->host_ctx);
    if (!vm->wall_known) return 0;
    uint64_t t = (uint64_t)vm->wall_offset + host_ms(vm);
    return t > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)t;
}

int vm_set_unix_time_ms(Vm *vm, int64_t ms) {
    if (ms > 0 && ms < GOGO_BUILD_UNIX_MS) return 0;
    if (vm->host->set_unix_time_ms) return !vm->host->set_unix_time_ms(vm->host_ctx, ms);
    vm->wall_known = false;
    vm->wall_offset = 0;
    if (ms > 0) {
        vm->wall_known = true;
        vm->wall_offset = ms - (int64_t)host_ms(vm);
    }
    return 1;
}
