/* The imu port (spec input2 §2, §4): polls the board's motion sensor on the
 * scheduler thread every min(BatchMs, 50) ms. FIFO bytes (12-byte samples)
 * are gathered into one batch and sent as a port.Chunk when BatchMs has
 * passed or the batch is full; gesture bits become imu.Gesture events,
 * filtered by the Config mask. Delivery never kills the owner: a Chunk it
 * has no room for is dropped and counted (stream/). The batch buffer is
 * BatchMs * RateHz * 12 / 1000 bytes (at most IMU_BATCH_MAX), allocated on
 * open (VM allocator, charged to the owner through Port.charge_words) and
 * freed on close; the rest of the state is InputState.imu (vm->input).
 *
 * Two things programs must know: TimeMs of a Chunk is the start of its batch
 * window (the open, or the previous cut), within one poll period of the
 * first sample; and closing the port (or the owner exiting) discards the
 * partial last batch, so close right after a Chunk to lose nothing. */
#include "input_int.h"
#include "host.h"
#include "stream.h"
#include "alloc.h"
#include <string.h>

enum { SAMPLE = 12, IMU_BATCH_MAX = 2400, DEF_BATCH = 200, MIN_BATCH = 50, MAX_BATCH = 1000, MAX_POLL = 50,
       GESTURE_ALL = 0xFF };

typedef InputImu Imu;

int input_imu_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    const ImuHal *hw = PORT_HAL(ImuHal, p);
    if (input_failed(vm) || !hw) return -1;
    Imu *m = &vm->input->imu;
    if (m->port || !hw->open || n < 3) return -1;
    int32_t rate = cfg[0], mask = cfg[1], batch = cfg[2];
    if (rate != 0 && rate != 25 && rate != 50 && rate != 100 && rate != 200) return -1;
    if (mask < 0 || mask > GESTURE_ALL) return -1;
    if (rate && !hw->read_fifo) return -1;
    if (mask && !hw->read_gestures) return -1;
    batch = batch <= 0 ? DEF_BATCH : batch < MIN_BATCH ? MIN_BATCH : batch > MAX_BATCH ? MAX_BATCH : batch;
    uint32_t cap = (uint32_t)batch * (uint32_t)rate * SAMPLE / 1000u;
    if (cap > IMU_BATCH_MAX) cap = IMU_BATCH_MAX;
    cap -= cap % SAMPLE;
    memset(m, 0, sizeof *m);
    if (rate) {
        if (cap < SAMPLE) cap = SAMPLE;
        m->buf = va_alloc(vm, cap);
        if (!m->buf) return -1;
    }
    if (hw->open(hw->ctx, (uint16_t)rate, (uint8_t)mask) != 0) {
        if (m->buf) va_free(vm, m->buf, cap);
        memset(m, 0, sizeof *m);
        return -1;
    }
    m->cap = (uint16_t)(rate ? cap : 0);
    m->rate = (uint8_t)(rate / 25);
    m->mask = (uint8_t)mask;
    m->batch_ms = (uint16_t)batch;
    m->poll_ms = (uint16_t)(batch < MAX_POLL ? batch : MAX_POLL);
    m->batch_at = (uint32_t)vm_now(vm);
    m->next_poll = m->batch_at + m->poll_ms;
    p->charge_words = (uint16_t)((m->cap + 3u) / 4u);   /* the owner pays; port_open refuses one that cannot */
    m->port = p;
    return 0;
}

void input_imu_close(struct Vm *vm, Port *p) {
    Imu *m = vm->input ? &vm->input->imu : NULL;
    if (!m || m->port != p) return;                      /* a refused open's cleanup */
    const ImuHal *hw = PORT_HAL(ImuHal, p);
    if (hw->close) hw->close(hw->ctx);
    if (m->buf) va_free(vm, m->buf, m->cap);
    memset(m, 0, sizeof *m);
}

bool input_imu_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    const ImuHal *hw = PORT_HAL(ImuHal, p);
    return hal_live(hw->live, hw->ctx);
}

/* ---- scheduler side ---- */

bool imu_work(const Vm *vm, uint64_t now) {
    const Imu *m = &vm->input->imu;
    return m->port && (m->rate || m->mask) && reached(now, m->next_poll); }

bool imu_next_deadline(const Vm *vm, uint64_t now, uint64_t *d) {
    const Imu *m = &vm->input->imu;
    if (!m->port || (!m->rate && !m->mask) || !input_imu_can_event(vm, m->port)) return false;   /* nothing to poll */
    *d = deadline_abs(now, m->next_poll);
    return true;
}

static void send_batch(Vm *vm, uint64_t now) {
    Imu *m = &vm->input->imu;
    if (m->fill) {
        if (!stream_deliver_chunk(vm, m->port, &m->in, m->batch_at, m->buf, m->fill)) dropped_inc(vm);
        m->fill = 0;                               /* sent or dropped: never kept for later */
    }
    m->batch_at = (uint32_t)now;
}

static void gestures(Vm *vm) {
    const Imu *m = &vm->input->imu;
    ImuGestures g;
    if (!m->mask) return;
    const ImuHal *hw = PORT_HAL(ImuHal, m->port);
    if (hw->read_gestures(hw->ctx, &g) != 0) return;
    uint8_t bits = (uint8_t)(g.bits & m->mask);
    for (uint8_t k = 1; bits; k = (uint8_t)(k << 1)) {
        if (!(bits & k)) continue;
        bits = (uint8_t)(bits & ~k);
        int32_t v[2] = { k, k == IMU_ORIENTATION ? g.orient : k == IMU_STEP ? (g.steps > 0x7FFFFFFFu ? 0x7FFFFFFF : (int32_t)g.steps) : 0 };
        if (!port_deliver_ints(vm, m->port, TYPE_IMU_GESTURE, v, 2, 0)) dropped_inc(vm);
    }
}

void imu_drain(Vm *vm, uint64_t now) {
    if (!imu_work(vm, now)) return;
    Imu *m = &vm->input->imu;
    m->next_poll = (uint32_t)now + m->poll_ms;
    const ImuHal *hw = PORT_HAL(ImuHal, m->port);
    while (m->rate) {                              /* RateHz 0: read_fifo is never called */
        uint32_t room = (uint32_t)(m->cap - m->fill);
        int r = hw->read_fifo(hw->ctx, m->buf + m->fill, room);
        if (r <= 0) break;                        /* empty, or an error: the next poll retries */
        r -= r % SAMPLE;
        if ((uint32_t)r > room) break;            /* a hook that broke its contract */
        m->fill = (uint16_t)(m->fill + r);
        if (m->fill == m->cap) send_batch(vm, now);       /* full: more may wait in the FIFO */
        else break;
    }
    if (m->rate && reached(now, m->batch_at + m->batch_ms)) send_batch(vm, now);
    gestures(vm);
}
