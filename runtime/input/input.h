/* Keyboard, power, IMU, IR and audio ports (spec input §5, input2 §4, audio §3):
 * chip-independent code; the IMU, IR and audio streams go through stream/. A
 * platform describes each device with a HAL table (KbdHal, PowerHal,
 * ImuHal, IrTxHal, IrRxHal, MicHal, SpkHal) and lists a driver instance
 * over it; input/ turns raw key events into kbd.Key messages (codes,
 * characters, modifiers, auto-repeat) and, on the scheduler thread, polls
 * the devices. Every hook runs on the scheduler thread, from a driver
 * callback or the source's pending/drain; only the flag behind
 * KbdHal.pending is written by an ISR. Pure C99. */
#ifndef GOGO_INPUT_H
#define GOGO_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "port.h"
#include "opcodes.h"

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

/* A board describes each input device with its own HAL table below, and
 * lists one driver instance per device it has (INPUT_KBD_DRIVER(&hal),
 * ...) among its ports; the instance carries the table (PortDriver.hw).
 * Every hook gets the table's ctx first, and runs on the scheduler thread
 * (only the flag behind KbdHal.pending is written by an ISR). An absent
 * device is a driver the board does not list.
 *
 * live (in the tables that have it; optional, NULL = always live): false
 * once the hardware can produce no more events (the unix stdin script hit
 * EOF); the port's can_event then says so and its polls stop, so a program
 * with no timers can exit. */

/* Keyboard (spec input §5). */
typedef struct KbdHal {
    void *ctx;
    const KbdKey *keys; uint16_t nkeys;             /* the key table, indexed by raw key number */
    int  (*open)(void *ctx);                        /* 0 ok: init chip, flush its FIFO, enable INT */
    void (*close)(void *ctx);                       /* optional: disable INT */
    /* True while the chip has events: a pure read of the ISR's flag (no side
     * effects; the source may call it several times per pass). The platform
     * clears the flag only when read returns 0 (empty), never on a read
     * that returned an event or an error; after an error it stays set, so the
     * next pass retries. */
    bool (*pending)(void *ctx);
    int  (*read)(void *ctx, uint16_t *raw, bool *down);   /* 1 event, 0 empty, -1 error */
    bool (*live)(void *ctx);
} KbdHal;

/* Battery and power supply (spec input §5.2). */
typedef struct PowerHal {
    void *ctx;
    int  (*read)(void *ctx, int32_t *mv, int8_t *charging, int8_t *external);   /* 0 ok */
    /* Optional. True: the chip has a new value, so the source calls read
     * now instead of waiting for PeriodMs. Like KbdHal.pending it is a pure
     * read (no side effects, may be called several times per pass); the
     * platform clears its own flag when read consumes the new value. */
    bool (*pending)(void *ctx);
    /* Optional. Called with true after the power port opened and with false
     * when it closed, so a platform that feeds it from a shared source (the
     * unix stdin script) knows when a consumer exists. */
    void (*enable)(void *ctx, bool on);
    bool (*live)(void *ctx);
    /* True: the chip is not polled. The source samples only at open, on a
     * Read, and when pending rises; PeriodMs is ignored and no sample
     * deadline keeps the VM waiting (unix: only `battery` script lines and
     * Read produce events). */
    bool event_driven;
} PowerHal;

/* Motion sensor (spec input2 §4). read_fifo is needed only for RateHz > 0
 * and read_gestures only for a non-zero gesture mask: a Config asking for
 * what the table lacks is refused. Polled from the scheduler thread every
 * min(BatchMs, 50) ms; there is no interrupt flag.
 *  - open: starts the sensor at rate_hz (0 = no samples, gestures only;
 *    25, 50, 100 or 200) with the gesture kinds in `gestures` enabled, and
 *    flushes its FIFO; 0 ok.
 *  - close (optional): stops it.
 *  - read_fifo: moves up to cap bytes of whole 12-byte samples (int16 LE
 *    gx gy gz ax ay az) into buf, returns the byte count (0 = empty, -1
 *    error, retried at the next poll); never called when rate_hz is 0.
 *  - read_gestures: the kinds seen since the last call, which it clears;
 *    0 ok, -1 error. */
typedef struct ImuHal {
    void *ctx;
    int  (*open)(void *ctx, uint16_t rate_hz, uint8_t gestures);
    void (*close)(void *ctx);
    int  (*read_fifo)(void *ctx, uint8_t *buf, uint32_t cap);
    int  (*read_gestures)(void *ctx, ImuGestures *g);
    bool (*live)(void *ctx);
} ImuHal;

/* Infrared (spec input2 §3, §4). Timings are microseconds, mark and space
 * alternating, mark first, each 1..32767; a frame holds at most
 * IR_FRAME_MAX values either way.
 * Transmitter:
 *  - open / close (optional, NULL = nothing to do): called when an ir_tx
 *    port opens (after its config is checked; -1 makes the open fail) and
 *    when it closes, so a platform can power the emitter and hold its
 *    resources only while a port is open. carrier_hz and duty_pct are the
 *    port's, the same values every start of that port gets. close may cut
 *    a frame still being sent; the next open starts from idle (busy
 *    false).
 *  - start: starts sending n (1..IR_FRAME_MAX) timings, given as 2n bytes
 *    of uint16 LE, with the carrier at carrier_hz and duty_pct; it copies
 *    (converts) what it needs before returning, so the bytes are not used
 *    afterwards, and it is called only while busy is false; 0 ok, -1 error
 *    (the program gets Done{-1}).
 *  - busy: true while a frame is being sent; a pure read, polled. */
typedef struct IrTxHal {
    void *ctx;
    int  (*open)(void *ctx, uint32_t carrier_hz, uint8_t duty_pct);
    void (*close)(void *ctx);
    int  (*start)(void *ctx, const uint8_t *timings, uint32_t n, uint32_t carrier_hz, uint8_t duty_pct);
    bool (*busy)(void *ctx);
} IrTxHal;

/* Receiver:
 *  - open: a gap longer than gap_us (1..32767) ends a frame; 0 ok.
 *  - read: the oldest complete frame, its values (each clamped to
 *    1..32767 by the caller) into buf: returns its value count, which may
 *    exceed cap (then only cap were stored and the rest are lost), 0 when
 *    no complete frame waits, -1 error (retried at the next poll).
 *    *start_ms holds the poll's time (host->now_ms, low 32 bits) on the
 *    call; a hook that knows when the frame began stores that instead.
 *    Polled every 20 ms while ir_rx is open.
 *  - close: optional. */
typedef struct IrRxHal {
    void *ctx;
    int  (*open)(void *ctx, uint32_t gap_us);
    int  (*read)(void *ctx, uint16_t *buf, uint32_t cap, uint32_t *start_ms);
    void (*close)(void *ctx);
    bool (*live)(void *ctx);
} IrRxHal;

/* Audio (spec audio §3): 16-bit little-endian mono PCM at rate 8000 or
 * 16000. The two sides are half duplex: input/ never has a mic and a
 * speaker port open at once, so a platform may share one codec and one I2S
 * channel pair.
 * Microphone:
 *  - open: starts capture at rate with gain_db (0, 6, 12, 18 or 24) of
 *    extra gain above the platform's normal mic level (0 = normal: speech
 *    at arm's length clearly above the noise floor without clipping; the
 *    platform picks the codec gain for that) and discards the start-up
 *    garbage (the first 2 DMA buffers) itself; 0 ok. Its buffer should
 *    hold at least 300 ms, so a late poll (an SD write stall) loses
 *    nothing.
 *  - read: moves up to cap bytes (cap is even) of captured samples into
 *    buf and returns the byte count, even (0 = nothing waits, -1 error,
 *    retried at the next poll); it may return less than cap with more
 *    waiting (one DMA buffer per call): the caller reads until 0. *lost (0
 *    on the call) is set to the bytes lost since the last call because its
 *    buffer was full. Polled every min(FrameMs, 50) ms while mic is open.
 *  - close: optional. */
typedef struct MicHal {
    void *ctx;
    int  (*open)(void *ctx, uint32_t rate, uint8_t gain_db);
    int  (*read)(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *lost);
    void (*close)(void *ctx);
    bool (*live)(void *ctx);
} MicHal;

/* Speaker:
 *  - open: powers the codec and the amplifier (in the order that does not
 *    pop) at rate and volume (0 mute, 1..100 from 40 dB below the
 *    platform's loudest level up to it); 0 ok.
 *  - write: never blocks; queues up to len bytes (len even, > 0) to play
 *    after what it already has, copying them, and returns how many it took
 *    (0 = its buffer is full, -1 error: that Out gets Done{-1}).
 *  - queued (optional): the bytes written but not played yet (0 = idle); a
 *    pure read, polled; NULL = a hardware that plays what it takes at once
 *    (Done when written).
 *  - close (optional): stops at once, cutting what is queued, and switches
 *    the amplifier off. */
typedef struct SpkHal {
    void *ctx;
    int  (*open)(void *ctx, uint32_t rate, uint8_t volume);
    int  (*write)(void *ctx, const uint8_t *data, uint32_t len);
    uint32_t (*queued)(void *ctx);
    void (*close)(void *ctx);
} SpkHal;

/* The drivers' entry points, for the INPUT_*_DRIVER instances below. */
int  input_kbd_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_kbd_close(struct Vm *vm, Port *p);
bool input_kbd_can_event(const struct Vm *vm, const Port *p);
int  input_power_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_power_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);
void input_power_close(struct Vm *vm, Port *p);
bool input_power_can_event(const struct Vm *vm, const Port *p);
int  input_imu_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_imu_close(struct Vm *vm, Port *p);
bool input_imu_can_event(const struct Vm *vm, const Port *p);
int  input_ir_tx_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_ir_tx_command(struct Vm *vm, const struct Actor *from, Port *p, term msg);
void input_ir_tx_close(struct Vm *vm, Port *p);
bool input_ir_tx_can_event(const struct Vm *vm, const Port *p);
int  input_ir_rx_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_ir_rx_close(struct Vm *vm, Port *p);
bool input_ir_rx_can_event(const struct Vm *vm, const Port *p);
int  input_mic_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_mic_close(struct Vm *vm, Port *p);
bool input_mic_can_event(const struct Vm *vm, const Port *p);
int  input_spk_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void input_spk_command(struct Vm *vm, const struct Actor *from, Port *p, term msg);
void input_spk_close(struct Vm *vm, Port *p);
bool input_spk_can_event(const struct Vm *vm, const Port *p);
/* The command of a port that takes none (imu, ir_rx, mic). */
void input_no_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);

/* A board's driver instance of each device over its HAL, e.g.
 *   static const KbdHal my_kbd = { ... };
 *   const PortDriver my_kbd_driver = INPUT_KBD_DRIVER(&my_kbd); */
#define INPUT_KBD_DRIVER(hal) \
    { PORT_NAME_KBD, TYPE_KBD_CONFIG, TYPE_KBD_KEY, input_kbd_open, NULL, input_kbd_close, input_kbd_can_event, \
      NULL, NULL, NULL, (hal) }
#define INPUT_POWER_DRIVER(hal) \
    { PORT_NAME_POWER, TYPE_POWER_CONFIG, TYPE_POWER_BATTERY, input_power_open, input_power_command, \
      input_power_close, input_power_can_event, NULL, NULL, NULL, (hal) }
#define INPUT_IMU_DRIVER(hal) \
    { PORT_NAME_IMU, TYPE_IMU_CONFIG, TYPE_IMU_GESTURE, input_imu_open, input_no_command, input_imu_close, \
      input_imu_can_event, NULL, NULL, NULL, (hal) }
#define INPUT_IR_TX_DRIVER(hal) \
    { PORT_NAME_IR_TX, TYPE_IR_TX, TYPE_DONE, input_ir_tx_open, NULL, input_ir_tx_close, input_ir_tx_can_event, \
      NULL, input_ir_tx_command, NULL, (hal) }
#define INPUT_IR_RX_DRIVER(hal) \
    { PORT_NAME_IR_RX, TYPE_IR_RX, TYPE_CHUNK, input_ir_rx_open, input_no_command, input_ir_rx_close, \
      input_ir_rx_can_event, NULL, NULL, NULL, (hal) }
#define INPUT_MIC_DRIVER(hal) \
    { PORT_NAME_MIC, TYPE_AUDIO_MIC, TYPE_CHUNK, input_mic_open, input_no_command, input_mic_close, \
      input_mic_can_event, NULL, NULL, NULL, (hal) }
#define INPUT_SPEAKER_DRIVER(hal) \
    { PORT_NAME_SPEAKER, TYPE_AUDIO_SPEAKER, TYPE_DONE, input_spk_open, NULL, input_spk_close, \
      input_spk_can_event, NULL, input_spk_command, NULL, (hal) }

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
