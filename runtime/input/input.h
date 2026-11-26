/* Keyboard, power, IMU, IR and audio ports (spec input §5, input2 §4, audio §3):
 * chip-independent code; the IMU, IR and audio streams go through stream/. A
 * platform describes its hardware with an InputHw; input/ turns raw key
 * events into kbd.Key messages (codes, characters, modifiers, auto-repeat)
 * and, on the scheduler thread, polls the power chip. Every hook runs on
 * the scheduler thread, from the source's pending/drain; only the flag
 * behind kbd_pending is written by an ISR. Pure C99. */
#ifndef GOGO_INPUT_H
#define GOGO_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "port.h"

/* What imu_read_gestures reports: the gesture kinds seen since the last
 * call (bits as imu.Tap .. imu.Step), and the values that go with two of
 * them: the orientation number (valid with the Orientation bit) and the
 * cumulative step count (valid with the Step bit). */
typedef struct { uint8_t bits, orient; uint32_t steps; } ImuGestures;
/* The most timings in one IR frame, sent or received (spec input2 §3). */
#define IR_FRAME_MAX 256u

/* The gesture bits (imu.Tap .. imu.Step) that carry a Value. */
enum { IMU_ORIENTATION = 8, IMU_STEP = 128 };

/* A board's key table, indexed by the chip's raw key number. */
typedef struct { uint8_t code, fn_code; } KbdKey;   /* code 0 = no key at this raw index */

typedef struct InputHw {
    const KbdKey *keys; uint16_t nkeys;             /* NULL = no keyboard */
    int  (*kbd_open)(void);                         /* 0 ok: init chip, flush its FIFO, enable INT */
    void (*kbd_close)(void);                        /* disable INT */
    /* True while the chip has events: a pure read of the ISR's flag (no side
     * effects; the source may call it several times per pass). The platform
     * clears the flag only when kbd_read returns 0 (empty), never on a read
     * that returned an event or an error; after an error it stays set, so the
     * next pass retries. Scheduler thread only. */
    bool (*kbd_pending)(void);
    int  (*kbd_read)(uint16_t *raw, bool *down);    /* 1 event, 0 empty, -1 error */
    int  (*power_read)(int32_t *mv, int8_t *charging, int8_t *external); /* NULL = no power; 0 ok */
    /* NULL = none. True: the chip has a new value, so the source calls
     * power_read now instead of waiting for PeriodMs. Like kbd_pending it is
     * a pure read (no side effects, may be called several times per pass);
     * the platform clears its own flag when power_read consumes the new
     * value. Scheduler thread only. */
    bool (*power_pending)(void);
    /* Optional, NULL = always live. False once the hardware can produce no
     * more events (the unix stdin script hit EOF); the kbd and power drivers'
     * can_event then say so, and a program with no timers can exit. */
    bool (*live)(void);
    /* Optional. Called with true after the power port opened and with false
     * when it closed, so a platform that feeds it from a shared source (the
     * unix stdin script) knows when a consumer exists. */
    void (*power_enable)(bool on);
    /* True: the chip is not polled. The source samples only at open, on a
     * Read, and when power_pending rises; PeriodMs is ignored and no sample
     * deadline keeps the VM waiting (unix: only `battery` script lines and
     * Read produce events). */
    bool power_event_driven;
    /* IMU (spec input2 §4); imu_open NULL = no IMU. imu_read_fifo is needed
     * only for RateHz > 0 and imu_read_gestures only for a non-zero gesture
     * mask: a Config asking for what a hook table lacks is refused. Polled from the scheduler
     * thread every min(BatchMs, 50) ms; there is no interrupt flag.
     * imu_open: starts the sensor at rate_hz (0 = no samples, gestures only;
     * 25, 50, 100 or 200) with the gesture kinds in `gestures` enabled, and
     * flushes its FIFO; 0 ok. imu_close stops it (optional). imu_read_fifo:
     * moves up to cap bytes of whole 12-byte samples (int16 LE gx gy gz ax
     * ay az) into buf, returns the byte count (0 = empty, -1 error, retried
     * at the next poll); it is never called when rate_hz is 0.
     * imu_read_gestures: the kinds seen since the last call, which it clears;
     * 0 ok, -1 error. */
    int  (*imu_open)(uint16_t rate_hz, uint8_t gestures);
    void (*imu_close)(void);
    int  (*imu_read_fifo)(uint8_t *buf, uint32_t cap);
    int  (*imu_read_gestures)(ImuGestures *g);
    /* Infrared (spec input2 §3, §4). Timings are microseconds, mark and
     * space alternating, mark first, each 1..32767; a frame holds at most
     * IR_FRAME_MAX values either way.
     * Transmitter: both hooks or neither (NULL = no ir_tx); the optional
     * ir_tx_open / ir_tx_close are at the end of the struct. ir_tx_start
     * starts sending n (1..IR_FRAME_MAX) timings, given as 2n bytes of
     * uint16 LE, with the carrier at carrier_hz and duty_pct; it copies
     * (converts) what it needs before returning, so the bytes are not used
     * afterwards, and it is called only while ir_tx_busy is false; 0 ok,
     * -1 error (the program gets Done{-1}). ir_tx_busy: true while a frame
     * is being sent; a pure read, polled. */
    int  (*ir_tx_start)(const uint8_t *timings, uint32_t n, uint32_t carrier_hz, uint8_t duty_pct);
    bool (*ir_tx_busy)(void);
    /* Receiver: ir_rx_open and ir_rx_read (NULL = no ir_rx); ir_rx_close is
     * optional. ir_rx_open: a gap longer than gap_us (1..32767) ends a
     * frame; 0 ok. ir_rx_read: the oldest complete frame, its values (each
     * clamped to 1..32767 by the caller) into buf: returns its value count,
     * which may exceed cap (then only cap were stored and the rest are
     * lost), 0 when no complete frame waits, -1 error (retried at the next
     * poll). *start_ms holds the poll's time (host->now_ms, low 32 bits)
     * on the call; a hook that knows when the frame began stores that
     * instead. Polled every 20 ms while ir_rx is open. */
    int  (*ir_rx_open)(uint32_t gap_us);
    int  (*ir_rx_read)(uint16_t *buf, uint32_t cap, uint32_t *start_ms);
    void (*ir_rx_close)(void);
    /* Transmitter, optional (NULL = nothing to do), like ir_rx_close: called
     * when an ir_tx port opens (after its config is checked; -1 makes the
     * open fail) and when it closes, so a platform can power the emitter
     * and hold its resources only while a port is open. carrier_hz and
     * duty_pct are the port's, the same values every ir_tx_start of that
     * port gets. ir_tx_close may cut a frame still being sent; the next
     * ir_tx_open starts from idle (ir_tx_busy false). Kept at the end of
     * the struct so positional initialisers stay valid. */
    int  (*ir_tx_open)(uint32_t carrier_hz, uint8_t duty_pct);
    void (*ir_tx_close)(void);
    /* Audio (spec audio §3): 16-bit little-endian mono PCM at rate 8000 or
     * 16000. The two sides are half duplex: input/ never has both open, so
     * a platform may share one codec and one I2S channel pair.
     * Microphone: mic_open and mic_read (NULL = no mic); mic_close is
     * optional. mic_open: starts capture at rate with gain_db (0, 6, 12,
     * 18 or 24) of extra gain above the platform's normal mic level (0 =
     * normal: speech at arm's length clearly above the noise floor without
     * clipping; the platform picks the codec gain for that) and discards
     * the start-up garbage (the first 2 DMA buffers) itself; 0 ok. Its buffer should hold at least
     * 300 ms, so a late poll (an SD write stall) loses nothing.
     * mic_read: moves up to cap bytes (cap is even) of captured samples
     * into buf and returns the byte count, even (0 = nothing waits, -1
     * error, retried at the next poll); it may return less than cap with
     * more waiting (one DMA buffer per call): the caller reads until 0.
     * *lost (0 on the call) is set to the bytes lost since the last call
     * because its buffer was full. Polled every min(FrameMs, 50) ms while
     * mic is open.
     * Speaker: spk_open and spk_write (NULL = no speaker); spk_queued and
     * spk_close are optional. spk_open: powers the codec and the amplifier
     * (in the order that does not pop) at rate and volume (0 mute, 1..100
     * from 40 dB below the platform's loudest level up to it); 0 ok.
     * spk_write: never blocks; queues up to len bytes (len even, > 0) to
     * play after what it already has, copying them, and returns how many it
     * took (0 = its buffer is full, -1 error: that Out gets Done{-1}).
     * spk_queued: the bytes written but not played yet (0 = idle); a pure
     * read, polled; NULL = a hardware that plays what it takes at once
     * (Done when written). spk_close: stops at once, cutting what is
     * queued, and switches the amplifier off. */
    int  (*mic_open)(uint32_t rate, uint8_t gain_db);
    int  (*mic_read)(uint8_t *buf, uint32_t cap, uint32_t *lost);
    void (*mic_close)(void);
    int  (*spk_open)(uint32_t rate, uint8_t volume);
    int  (*spk_write)(const uint8_t *data, uint32_t len);
    uint32_t (*spk_queued)(void);
    void (*spk_close)(void);
} InputHw;

/* A board gives its InputHw in VmBoard.input (host.h); NULL = none. */

extern const PortDriver input_kbd_driver, input_power_driver, input_imu_driver, input_ir_tx_driver,
    input_ir_rx_driver, input_mic_driver, input_speaker_driver;

/* One Vm's input state (input_int.h). */
typedef struct InputState InputState;

/* Clears st, points vm->input at it and registers the input source on vm
 * (once per Vm, after vm_boot: vm_init clears the source list). st must
 * outlive the Vm. Without it every input port refuses to open.
 * If the source could not be registered (every slot taken), a line is logged
 * and the kbd and power ports refuse to open, as on absent hardware. */
void input_install(struct Vm *vm, InputState *st);
void input_uninstall(struct Vm *vm);   /* tests: removes the source so input_install can run again */

/* Key codes are USB HID usages; Fn has no usage, gogo uses 0xF0. */
enum { KEY_CTRL = 0xE0, KEY_SHIFT = 0xE1, KEY_ALT = 0xE2, KEY_OPT = 0xE3, KEY_FN = 0xF0 };
enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_OPT = 8, MOD_FN = 16 };

/* The ASCII character of key `code` under mods (US layout), 0 if none. */
uint8_t input_key_char(uint8_t code, uint32_t mods);

uint32_t input_dropped(const struct Vm *vm);       /* events dropped (owner gone, or over its quota) */
uint32_t input_read_errors(const struct Vm *vm);   /* kbd_read failures */

#endif
