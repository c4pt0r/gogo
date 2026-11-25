/* input/ internals shared by kbd.c and source.c. */
#ifndef GOGO_INPUT_INT_H
#define GOGO_INPUT_INT_H

#include "input.h"
#include "opcodes.h"
#include "actor.h"
#include "stream.h"

/* Deadlines inside input/ are 32-bit ms (wrapping, compared as signed
 * deltas) to save RAM; reached() and deadline_abs() convert from and to the
 * scheduler's 64-bit clock. */
static inline bool reached(uint64_t now, uint32_t t) { return (int32_t)((uint32_t)now - t) >= 0; }
static inline uint64_t deadline_abs(uint64_t now, uint32_t t) {
    int32_t d = (int32_t)(t - (uint32_t)now);
    return d <= 0 ? now : now + (uint32_t)d;
}

/* ---- per-Vm state (InputState, in vm->input) ---- */

#define INPUT_MAX_HELD 8u
#define INPUT_PWR_AVG 4u

typedef struct { uint8_t raw, code; } InputHeld;

typedef struct {               /* kbd.c; cleared on open and close */
    const Port *port;          /* the open port, NULL = closed */
    InputHeld held[INPUT_MAX_HELD];   /* raw key -> the Code reported at its press */
    uint8_t nheld;
    bool rep_on;               /* rep_raw (held) repeats at rep_at */
    uint8_t rep_raw;
    uint16_t delay, period;    /* ms */
    uint32_t rep_at;           /* ms, wrapping */
    uint32_t retry_at;         /* ms, wrapping: no kbd_read before this (after repeated errors) */
    uint8_t errs;              /* consecutive kbd_read failures */
} InputKbd;

typedef struct {               /* power.c; cleared on open and close */
    const Port *port;          /* the open port, NULL = closed */
    uint32_t next_at;          /* the next scheduled sample, ms (wrapping) */
    uint16_t mv[INPUT_PWR_AVG];   /* the last samples (all alike after the first) */
    uint16_t period;           /* ms */
    uint8_t idx;               /* next slot of mv */
    bool have;                 /* a good sample exists */
    bool read_req;             /* a Read command waits */
    bool backoff;              /* a read failed or an event was dropped: wait for next_at */
    bool fail_pend;            /* the flag was set when backoff began: a stuck flag, only next_at ends it */
    bool start;                /* the open-time sample is still owed */
    bool reported;             /* last_* below are valid */
    int8_t last_pct, last_ch, last_ext;   /* as of the last delivered event */
} InputPower;

typedef struct {               /* imu.c */
    const Port *port;          /* the open port, NULL = closed */
    uint8_t *buf;              /* the batch, NULL when rate is 0 */
    StreamIn in;
    uint32_t next_poll;        /* ms, wrapping */
    uint32_t batch_at;         /* ms, wrapping: when the batch began */
    uint16_t cap, fill;        /* bytes */
    uint16_t batch_ms, poll_ms;
    uint8_t mask;
    uint8_t rate;              /* RateHz / 25 (0, 1, 2, 4, 8): fits a byte */
} InputImu;

typedef struct {               /* ir.c */
    Port *tx;                  /* the open ir_tx, NULL = closed; its StreamOut is tx->state */
    uint32_t carrier;
    uint32_t tx_next;          /* ms, wrapping: the next busy poll, start or Done retry */
    uint8_t duty;
    bool running;              /* the head was started (ir_tx_start succeeded) */
    const Port *rx;            /* the open ir_rx, NULL = closed */
    uint16_t *buf;             /* IR_FRAME_MAX values, while rx is open */
    StreamIn in;
    uint32_t rx_next;          /* ms, wrapping: the next ir_rx_read poll */
} InputIr;

typedef struct {               /* audio.c */
    const Port *mic;           /* the open mic, NULL = closed */
    uint8_t *buf;              /* one frame */
    StreamIn in;
    uint32_t t0;               /* ms, wrapping: the open */
    uint32_t frames;           /* frames made (delivered, dropped or lost) */
    uint32_t mic_next;         /* ms, wrapping: the next mic_read poll */
    uint16_t cap, fill;        /* frame bytes, bytes in buf */
    uint16_t skip;             /* bytes still to discard after a gap (to a frame boundary) */
    uint16_t frame_ms, poll_ms;
    bool more;                 /* the last poll read something: more may wait */
    Port *spk;                 /* the open spk, NULL = closed; its StreamOut is spk->state */
    uint32_t written;          /* bytes taken by spk_write since the open, wrapping */
    uint32_t end[STREAM_OUT_MAX];   /* Out k fully written: `written` at its end */
    uint32_t cur;              /* bytes of Out `wi` written */
    uint32_t spk_next;         /* ms, wrapping: the next spk poll */
    uint8_t wi;                /* the Out being written; o->n when all are */
    uint8_t bad;               /* bit k: Out k failed to write (Done{-1} when it is the head) */
} InputAudio;

/* One Vm's input state. A board keeps it in static storage; input_install
 * clears it and points vm->input at it. */
struct InputState {
    InputKbd kbd;
    InputPower pwr;
    InputImu imu;
    InputIr ir;
    InputAudio au;
    uint32_t dropped, read_errors;   /* input_dropped(), input_read_errors() */
    bool installed, failed;    /* the source is registered / found no slot */
};

/* Ports open only after input_install found a slot (vm->input set, not failed). */
bool input_failed(const Vm *vm);
const InputHw *input_hw(const Vm *vm);       /* the board's input chips; never NULL (fields NULL when absent) */
bool kbd_work(const Vm *vm, uint64_t now);                 /* a raw event waits, or a repeat is due */
bool kbd_next_deadline(const Vm *vm, uint64_t now, uint64_t *d);   /* the next auto-repeat or error retry */
void kbd_drain(Vm *vm, uint64_t now);

bool power_work(const Vm *vm, uint64_t now);                /* a sample is due, wanted (Read) or flagged */
bool power_next_deadline(const Vm *vm, uint64_t now, uint64_t *d);       /* the next scheduled sample */
void power_drain(Vm *vm, uint64_t now);
bool imu_work(const Vm *vm, uint64_t now);                  /* a poll is due */
bool imu_next_deadline(const Vm *vm, uint64_t now, uint64_t *d);        /* the next poll */
void imu_drain(Vm *vm, uint64_t now);
bool ir_work(const Vm *vm, uint64_t now);                   /* a Done, a start or a receive poll is due */
bool ir_next_deadline(const Vm *vm, uint64_t now, bool live, uint64_t *d);   /* the next of those (receive polls only if live) */
void ir_drain(Vm *vm, uint64_t now);
bool audio_work(const Vm *vm, uint64_t now);                /* a mic or spk poll is due */
bool audio_next_deadline(const Vm *vm, uint64_t now, bool live, uint64_t *d);   /* the next poll (mic only while data may come) */
void audio_drain(Vm *vm, uint64_t now);
static inline void dropped_inc(Vm *vm) { vm->input->dropped++; }   /* an event dropped, for input_dropped() */

#endif
