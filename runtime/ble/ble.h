/* The "ble" port: a Bluetooth Low Energy peripheral (a GATT server) whose
 * services the program declares when it opens the port (gogo/ble).
 * Chip-independent C99; a board gives it a BleHal (the radio: NimBLE on
 * the ESP32-S3, a local socket in gogovm) and lists BLE_DRIVER(&hal).
 *
 * The port speaks the generic host-port types, so no reserved type or
 * image format is involved (book section 22.14):
 *  - cfg: port.Host{Arg: BLE_CFG_FORMAT, Data: the encoded Config, below};
 *  - commands: port.Post{Kind: BLE_CMD_* | char << 8, Data};
 *  - events: port.Post{From: the port, Kind: BLE_EV_* | arg << 8, Data},
 *    and port.Done{From, Seq} for each Notify (stream/'s rules: at most
 *    two in flight, a Done never lost to a full mailbox).
 * gogo/ble encodes and decodes them; lib/ble/ble.gg is the other half of
 * this header and must change with it.
 *
 * Threads: every driver callback and the event source run on the VM's
 * thread. The radio side (a NimBLE callback, a socket thread) reports with
 * ble_post, under the HAL's lock, and wakes the VM; it reads the values
 * that a phone's read returns from its own store (BleHal.set_value), so a
 * read never wakes the VM. One port per board: the radio is the board's
 * (BleBoard, board storage), not a VM's. */
#ifndef GOGO_BLE_H
#define GOGO_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "port.h"
#include "opcodes.h"
#include "stream.h"

/* ---- limits (gogo/ble documents them) ---- */
#define BLE_MAX_SERVICES  4u
#define BLE_MAX_CHARS     16u    /* in all services together */
#define BLE_MAX_PER_SVC   8u
#define BLE_NAME_MAX      26u    /* bytes: the name goes in the 31-byte advertisement, with the flags */
#define BLE_VALUE_MAX     512u   /* ATT's longest attribute value: a SetValue, a write */
#define BLE_QUEUE_BYTES   2048u  /* the radio side's event queue */
#define BLE_PEER_MAX      17u    /* "aa:bb:cc:dd:ee:ff" */

/* Characteristic flags (gogo/ble Read, Write, WriteNoResp, Notify). */
enum { BLE_F_READ = 1, BLE_F_WRITE = 2, BLE_F_WRITE_NR = 4, BLE_F_NOTIFY = 8, BLE_F_ALL = 15 };

/* port.Host.Arg: the encoding of Data. Data is
 *   name_len u8, name (1..BLE_NAME_MAX bytes, no NUL),
 *   nsvc u8 (1..BLE_MAX_SERVICES), then per service:
 *     uuid_len u8 (2 or 16), uuid (little-endian: the order on air),
 *     nchr u8 (1..BLE_MAX_PER_SVC), then per characteristic:
 *       uuid_len u8, uuid, flags u8 (1..BLE_F_ALL)
 * with BLE_MAX_CHARS characteristics at most, nothing after. Characteristic
 * k is the k-th in this order, from 0 (gogo/ble's Char). */
#define BLE_CFG_FORMAT 1

/* port.Post.Kind of a command, low byte; bits 8..15: the characteristic. */
enum { BLE_CMD_SET_VALUE = 1, BLE_CMD_NOTIFY = 2 };
/* port.Post.Kind of an event, low byte; the rest:
 *   CONNECTED     MTU << 8, Data: the peer's address as text
 *   DISCONNECTED  reason << 8 (an HCI reason code: 0x13 the peer ended it,
 *                 0x08 the link timed out, 0x16 this side ended it)
 *   WRITTEN       char << 8, Data: the bytes written
 *   SUBSCRIBED    char << 8 | on << 16
 *   MTU           MTU << 8: the ATT MTU changed (a Notify carries MTU - 3 bytes) */
enum { BLE_EV_CONNECTED = 1, BLE_EV_DISCONNECTED = 2, BLE_EV_WRITTEN = 3, BLE_EV_SUBSCRIBED = 4, BLE_EV_MTU = 5 };

/* The default ATT MTU before an exchange. */
#define BLE_MTU_MIN 23u

typedef struct { uint8_t len; uint8_t b[16]; } BleUuid;   /* len 2 or 16; b little-endian */

/* A decoded Config: what BleHal.start builds the GATT server from. */
typedef struct BleConfig {
    char name[BLE_NAME_MAX + 1u];              /* NUL-terminated */
    uint8_t nsvc, nchr;
    uint8_t first[BLE_MAX_SERVICES + 1u];      /* service s has characteristics first[s] .. first[s+1]-1 */
    BleUuid svc[BLE_MAX_SERVICES];
    BleUuid chr[BLE_MAX_CHARS];
    uint8_t flags[BLE_MAX_CHARS];
} BleConfig;

/* Decodes a port.Host Data (BLE_CFG_FORMAT). 0, or GOGO_E_BADARG
 * (malformed, a name or a flag or UUID length out of range, no services
 * or characteristics) or GOGO_E_TOOMANY (more services or characteristics
 * than the limits above). */
int ble_config_decode(const uint8_t *d, uint32_t len, BleConfig *c);

/* ---- the board's state ---- */

/* Fixed storage, the board's (one radio, one port at a time): the queue
 * the radio side writes (under BleHal.lock) and the driver reads on the
 * VM thread, then the driver's own state of the open port. */
typedef struct BleBoard {
    /* the queue: records [ev u8, 0, len u16, arg u32, data padded to 4];
     * a record never wraps (ev 0 at the end: the rest is skipped) */
    uint8_t ring[BLE_QUEUE_BYTES];
    uint32_t head, tail;        /* byte offsets; head == tail: empty */
    uint32_t lost;              /* events the radio side could not queue */
    bool accepting;             /* a port is open: ble_post queues, else it drops */
    /* the driver's (VM thread only) */
    Port *port;                 /* the open port, NULL: none; its StreamOut is port->state */
    BleConfig cfg;
    uint16_t mtu;               /* as the events so far said */
    uint16_t subs;              /* bit k: the phone subscribed to characteristic k */
    bool connected;
    bool seen;                  /* the record at tail has updated the state above */
    uint8_t chr[STREAM_OUT_MAX];   /* the characteristic of each Notify in flight */
    uint64_t retry_at;          /* vm_now: the next try of a held event, notify or owed Done (0: none) */
    uint16_t tries;             /* BLE_AGAIN answers to the head Notify so far */
} BleBoard;

/* Bytes a record of len data bytes takes. */
#define BLE_REC_BYTES(len) (8u + (((uint32_t)(len) + 3u) & ~3u))
/* Room kept for the small events: a write is queued only if this much
 * stays free after it, so a Connected, Disconnected or MTU is never lost
 * behind a burst of writes. */
#define BLE_QUEUE_SPARE 128u

/* The radio side: queues event ev (BLE_EV_*) with its argument word
 * (Post.Kind >> 8) and data (len <= BLE_VALUE_MAX). Call with the HAL's
 * lock held, then wake the VM (vm_wake, platform_notify). False if it
 * does not fit (counted in b->lost) or no port is open: a write with a
 * response is then answered with an error, so the phone retries. */
bool ble_post(BleBoard *b, uint32_t ev, uint32_t arg, const uint8_t *data, uint32_t len);

/* ---- the hardware layer ---- */

/* BleHal.notify's "no buffer now, try again shortly". */
#define BLE_AGAIN (-1)

/* A board's radio. Every hook runs on the VM's thread, except where said.
 * One port at a time: start and stop pair up. */
typedef struct BleHal {
    void *ctx;
    BleBoard *board;           /* the board's state */
    /* Starts the GATT server built from cfg (copied: cfg goes away after
     * the call) and advertising; values all empty. 0, or a GOGO_E_* code
     * (> 0): BUSY (the radio is taken: Wi-Fi provisioning is running),
     * NOMEMORY, IO (the stack did not start). */
    int  (*start)(void *ctx, const BleConfig *cfg);
    /* Ends any connection, stops advertising and the stack; drops the
     * values. After it no ble_post comes until the next start. */
    void (*stop)(void *ctx);
    /* The value a phone's read of characteristic chr returns from now on
     * (copied; len <= BLE_VALUE_MAX). 0 or GOGO_E_NOMEMORY. */
    int  (*set_value)(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len);
    /* Sends a notification of characteristic chr to the connected phone
     * (len <= MTU - 3, checked by the driver). 0 when the stack took it,
     * BLE_AGAIN when it has no buffer now (the driver tries again in
     * BLE_RETRY_MS), else a GOGO_E_* code (> 0): not sent. */
    int  (*notify)(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len);
    /* The queue's lock; any thread. */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} BleHal;

/* An owner whose mailbox is full holds the events back (they stay queued,
 * and a phone's writes are then answered with an error); they are tried
 * again this often, as are notifications the stack had no buffer for. */
#define BLE_RETRY_MS 5u
/* A Notify the stack has had no buffer for this many tries (two seconds)
 * fails: Done{Seq: -1}. */
#define BLE_RETRY_MAX 400u

/* The driver (ble.c). */
int  ble_open(struct Vm *vm, const struct Actor *owner, Port *p, uint16_t index, term cfg);
void ble_command(struct Vm *vm, const struct Actor *from, Port *p, term msg);
void ble_close(struct Vm *vm, Port *p);
bool ble_can_event(const struct Vm *vm, const Port *p);

#define BLE_DRIVER(hal) { .name = PORT_NAME_BLE, .cfg_type = TYPE_HOST, .event_type = TYPE_POST, \
    .close = ble_close, .can_event = ble_can_event, .command_term = ble_command, .open_term = ble_open, .hw = (hal) }

/* Events the radio side could not queue since boot (BleBoard.lost). */
uint32_t ble_lost(const BleHal *hal);

#endif
