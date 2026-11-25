/* Keyboard and power ports (spec input §5): chip-independent code. A
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
} InputHw;

/* The platform's hardware; never NULL, fields NULL when absent. A weak
 * default (no hardware) is in source.c. */
const InputHw *platform_input(void);

extern const PortDriver input_kbd_driver, input_power_driver;

/* Registers the input source (call once at startup). */
void input_install(void);
/* If the source could not be registered (every slot taken), a line is logged
 * and the kbd and power ports refuse to open, as on absent hardware. */
void input_uninstall(void);   /* tests: removes the source so input_install can run again */

/* Key codes are USB HID usages; Fn has no usage, gogo uses 0xF0. */
enum { KEY_CTRL = 0xE0, KEY_SHIFT = 0xE1, KEY_ALT = 0xE2, KEY_OPT = 0xE3, KEY_FN = 0xF0 };
enum { MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_OPT = 8, MOD_FN = 16 };

/* The ASCII character of key `code` under mods (US layout), 0 if none. */
uint8_t input_key_char(uint8_t code, uint32_t mods);

uint32_t input_dropped(void);       /* events dropped (owner gone, or over its quota) */
uint32_t input_read_errors(void);   /* kbd_read failures */

#endif
