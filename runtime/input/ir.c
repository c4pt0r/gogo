/* The ir_tx and ir_rx ports (spec input2 §3, §4): chip-independent IR on
 * top of the board's hooks (InputHw ir_*), on the scheduler thread.
 *
 *  - ir_tx: every command is a port.Out of timings (stream_out_accept with
 *    stream_timings_ok and at most IR_FRAME_MAX values, the receiver's
 *    limit too: a bad or longer one, or a third in flight, is Done{-1}).
 *    The head is started with ir_tx_start once the transmitter is idle (it
 *    may still be sending a frame of a port closed earlier), then
 *    ir_tx_busy is polled, first when the frame should have ended, then
 *    every IR_TX_POLL_MS; when it is idle the Out is done (stream_out_done
 *    sends or owes Done{Seq}) and the queued one starts. A start that fails
 *    finishes its Out at once with Done{-1} (stream_out_fail). Owed Dones
 *    are retried every IR_POLL_MS. Closing forgets what is in flight
 *    (stream_out_close, no Done); a frame already started is finished by
 *    the hardware, which copied it, unless the optional ir_tx_close hook
 *    cuts it. The optional ir_tx_open / ir_tx_close hooks bracket the
 *    port's life (power and resources on the platform side).
 *  - ir_rx: ir_rx_read is polled every IR_POLL_MS; each frame becomes one
 *    port.Chunk of uint16 LE timings (TimeMs: the frame's start if the hook
 *    gives it, else the poll that read it), the same format ir_tx takes, so
 *    a frame can be sent back as it is. A frame over IR_FRAME_MAX values is
 *    cut there and counts 1 in the Chunk's Dropped (2 if that Chunk is lost
 *    too). The frame buffer (IR_FRAME_MAX * 2 bytes) is allocated on open
 *    (VM allocator, charged to the owner) and freed on close.
 *
 * One port of each kind at a time. Delivery never kills the owner (stream/). */
#include "input_int.h"
#include "host.h"
#include "stream.h"
#include "alloc.h"
#include <string.h>

enum { IR_POLL_MS = 20, IR_TX_POLL_MS = 5, IR_FRAMES_PER_POLL = 4,
       DEF_CARRIER = 38000, MIN_CARRIER = 10000, MAX_CARRIER = 500000, DEF_DUTY = 33,
       DEF_GAP = 10000, MAX_GAP = 32767, MAX_US = 32767 };

static struct {
    Port *tx;                  /* the open ir_tx, NULL = closed; its StreamOut is tx->state */
    uint32_t carrier;
    uint32_t tx_next;          /* ms, wrapping: the next busy poll, start or Done retry */
    uint8_t duty;
    bool running;              /* the head was started (ir_tx_start succeeded) */
    const Port *rx;            /* the open ir_rx, NULL = closed */
    uint16_t *buf;             /* IR_FRAME_MAX values, while rx is open */
    StreamIn in;
    uint32_t rx_next;          /* ms, wrapping: the next ir_rx_read poll */
} ir;

static StreamOut *out_of(Port *p) { return (StreamOut *)(void *)p->state; }

/* Valid timings (stream_timings_ok) of at most IR_FRAME_MAX values: what
 * ir_tx_start is promised, and the most a received frame holds. */
static bool tx_timings_ok(const Port *p, const uint8_t *d, uint32_t len) {
    return len <= IR_FRAME_MAX * 2u && stream_timings_ok(p, d, len);
}

/* ---- ir_tx ---- */

static int tx_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)index;
    const InputHw *hw = input_hw(vm);
    if (input_failed() || ir.tx || !hw->ir_tx_start || !hw->ir_tx_busy || n < 2) return -1;
    int32_t carrier = cfg[0] <= 0 ? DEF_CARRIER : cfg[0], duty = cfg[1] <= 0 ? DEF_DUTY : cfg[1];
    if (carrier < MIN_CARRIER || carrier > MAX_CARRIER || duty > 99) return -1;
    if (hw->ir_tx_open && hw->ir_tx_open((uint32_t)carrier, (uint8_t)duty) != 0) return -1;
    memset(out_of(p), 0, sizeof(StreamOut));
    ir.tx = p;
    ir.carrier = (uint32_t)carrier;
    ir.duty = (uint8_t)duty;
    ir.running = false;
    return 0;
}

static void tx_close(struct Vm *vm, Port *p) {
    (void)vm;
    if (ir.tx != p) return;                    /* a refused open's cleanup */
    stream_out_close(NULL, p, out_of(p));   /* port_close releases the held data */
    ir.tx = NULL;
    ir.running = false;
    const InputHw *hw = input_hw(vm);
    if (hw->ir_tx_close) hw->ir_tx_close();
}

static void tx_command(Vm *vm, const Actor *from, Port *p, term msg) {
    if (ir.tx != p) return;
    /* A new head starts, or a Done{-1} is flushed, at once; one queued
     * behind a running frame waits for the poll already set. */
    if (stream_out_accept(vm, from, p, out_of(p), msg, tx_timings_ok) != 0) ir.tx_next = (uint32_t)vm_now(vm);
}

static bool tx_can_event(const struct Vm *vm, const Port *p) { (void)vm;
    const StreamOut *o = (const StreamOut *)(const void *)p->state;
    return o->n || o->owed_n;                 /* Dones are coming */
}

const PortDriver input_ir_tx_driver = {
    "ir_tx", TYPE_IR_TX, TYPE_DONE, tx_open, NULL, tx_close, tx_can_event, NULL, tx_command, NULL
};

/* ---- ir_rx ---- */

static int rx_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    const InputHw *hw = input_hw(vm);
    if (input_failed() || ir.rx || !hw->ir_rx_open || !hw->ir_rx_read || n < 1) return -1;
    int32_t gap = cfg[0] <= 0 ? DEF_GAP : cfg[0] > MAX_GAP ? MAX_GAP : cfg[0];
    uint16_t *buf = va_alloc(vm, IR_FRAME_MAX * 2u);
    if (!buf) return -1;
    if (hw->ir_rx_open((uint32_t)gap) != 0) {
        va_free(vm, buf, IR_FRAME_MAX * 2u);
        return -1;
    }
    memset(&ir.in, 0, sizeof ir.in);
    ir.buf = buf;
    ir.rx = p;
    ir.rx_next = (uint32_t)vm_now(vm) + IR_POLL_MS;
    p->charge_words = IR_FRAME_MAX * 2u / 4u;   /* the owner pays; port_open refuses one that cannot */
    return 0;
}

static void rx_close(struct Vm *vm, Port *p) {
    if (ir.rx != p) return;                    /* a refused open's cleanup */
    const InputHw *hw = input_hw(vm);
    if (hw->ir_rx_close) hw->ir_rx_close();
    va_free(vm, ir.buf, IR_FRAME_MAX * 2u);
    ir.buf = NULL;
    ir.rx = NULL;
}

static void rx_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) { (void)vm; (void)p; (void)type; (void)f; (void)n; }

static bool rx_can_event(const struct Vm *vm, const Port *p) { (void)vm;
    (void)p;
    const InputHw *hw = input_hw(vm);
    return !hw->live || hw->live();
}

const PortDriver input_ir_rx_driver = {
    "ir_rx", TYPE_IR_RX, TYPE_CHUNK, rx_open, rx_command, rx_close, rx_can_event, NULL, NULL, NULL
};

/* ---- scheduler side ---- */

bool ir_is_open(void) { return ir.tx || ir.rx; }

static bool tx_waits(void) {
    const StreamOut *o = ir.tx ? out_of(ir.tx) : NULL;
    return o && (o->n || o->owed_n);
}

bool ir_work(uint64_t now) {
    return (tx_waits() && reached(now, ir.tx_next)) || (ir.rx && reached(now, ir.rx_next));
}

bool ir_next_deadline(uint64_t now, bool live, uint64_t *d) {
    bool any = false;
    if (tx_waits()) { *d = deadline_abs(now, ir.tx_next); any = true; }
    if (live && ir.rx) {
        uint64_t t = deadline_abs(now, ir.rx_next);
        if (!any || t < *d) *d = t;
        any = true;
    }
    return any;
}

/* How long n timings take, in ms, rounded up. */
static uint32_t frame_ms(const uint8_t *d, uint32_t n) {
    uint32_t us = 0;
    for (uint32_t i = 0; i < n; i++) us += (uint32_t)d[2 * i] | (uint32_t)d[2 * i + 1] << 8;
    return (us + 999u) / 1000u;
}

static void tx_drain(Vm *vm, uint64_t now) {
    StreamOut *o = out_of(ir.tx);
    const InputHw *hw = input_hw(vm);
    stream_out_flush(vm, ir.tx, o);
    uint32_t next = (uint32_t)now + IR_TX_POLL_MS;   /* while busy */
    while (!hw->ir_tx_busy()) {               /* ours, or a frame of a port closed earlier */
        if (ir.running) {
            ir.running = false;
            stream_out_done(vm, ir.tx, o);
        }
        uint32_t len;
        const uint8_t *d = stream_out_head(vm, o, &len);
        if (!d) { next = (uint32_t)now + IR_POLL_MS; break; }   /* only owed Dones may remain */
        if (hw->ir_tx_start(d, len / 2u, ir.carrier, ir.duty) == 0) {
            ir.running = true;
            next = (uint32_t)now + frame_ms(d, len / 2u);
            break;
        }
        stream_out_fail(vm, ir.tx, o);         /* not sent: Done{-1}; try the queued one */
    }
    ir.tx_next = next;
}

static void rx_drain(Vm *vm, uint64_t now) {
    const InputHw *hw = input_hw(vm);
    ir.rx_next = (uint32_t)now + IR_POLL_MS;
    for (int k = 0; k < IR_FRAMES_PER_POLL; k++) {
        uint32_t t0 = (uint32_t)vm->t0, t = (uint32_t)now + t0;   /* host clock: the hook may give the frame's start */
        int r = hw->ir_rx_read(ir.buf, IR_FRAME_MAX, &t);
        t -= t0;
        if (r <= 0) break;                    /* none, or an error: the next poll retries */
        uint32_t n = (uint32_t)r;
        if (n > IR_FRAME_MAX) {               /* cut: the rest of the frame is lost */
            n = IR_FRAME_MAX;
            ir.in.dropped_since++;
        }
        uint8_t *b = (uint8_t *)ir.buf;       /* to uint16 LE in place: value i is bytes 2i, 2i+1 */
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ir.buf[i];
            v = v == 0 ? 1 : v > MAX_US ? MAX_US : v;
            b[2 * i] = (uint8_t)(v & 0xFF);
            b[2 * i + 1] = (uint8_t)(v >> 8);
        }
        if (!stream_deliver_chunk(vm, ir.rx, &ir.in, t, b, n * 2u)) dropped_inc();
    }
}

void ir_drain(Vm *vm, uint64_t now) {
    if (tx_waits() && reached(now, ir.tx_next)) tx_drain(vm, now);
    if (ir.rx && reached(now, ir.rx_next)) rx_drain(vm, now);
}
