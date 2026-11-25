/* The kbd port: raw key events -> Key messages (spec input §5.3). The state
 * is one small struct; it is cleared on open and close. */
#include "input_int.h"
#include <string.h>

#define MAX_HELD 8u

typedef struct { uint8_t raw, code; } Held;

static struct {
    const Port *port;          /* the open port, NULL = closed */
    Held held[MAX_HELD];       /* raw key -> the Code reported at its press */
    uint8_t nheld;
    bool rep_on;               /* rep_raw (held) repeats at rep_at */
    uint8_t rep_raw;
    uint16_t delay, period;    /* ms */
    uint32_t rep_at;           /* ms, wrapping */
    uint32_t retry_at;         /* ms, wrapping: no kbd_read before this (after repeated errors) */
    uint8_t errs;              /* consecutive kbd_read failures */
} k;

static uint32_t dropped, read_errors;
uint32_t input_dropped(void) { return dropped; }
void dropped_inc(void) { dropped++; }
uint32_t input_read_errors(void) { return read_errors; }

static uint32_t mod_of(uint8_t code) {
    switch (code) {
    case KEY_CTRL:  return MOD_CTRL;
    case KEY_SHIFT: return MOD_SHIFT;
    case KEY_ALT:   return MOD_ALT;
    case KEY_OPT:   return MOD_OPT;
    case KEY_FN:    return MOD_FN;
    default:        return 0;
    }
}

static uint32_t held_mods(void) {
    uint32_t m = 0;
    for (uint8_t i = 0; i < k.nheld; i++) m |= mod_of(k.held[i].code);
    return m;
}

static Held *find(uint8_t raw) {
    for (uint8_t i = 0; i < k.nheld; i++) {
        if (k.held[i].raw == raw) return &k.held[i];
    }
    return NULL;
}

static void send(Vm *vm, uint8_t code, uint32_t mods, bool down) {
    int32_t v[4] = { code, input_key_char(mod_of(code) ? 0 : code, mods), (int32_t)mods, down };
    if (!port_deliver_ints(vm, k.port, TYPE_KBD_KEY, v, 4, 8u)) dropped++;
}

static void key_event(Vm *vm, const KbdKey *tab, uint16_t raw, bool down, uint64_t now) {
    uint8_t code = tab[raw].code;
    Held *h = find((uint8_t)raw);
    if (down) {
        if (h || k.nheld == MAX_HELD) return;     /* already held, or the 9th */
        if ((held_mods() & MOD_FN) && tab[raw].fn_code && !mod_of(code)) code = tab[raw].fn_code;
        k.held[k.nheld].raw = (uint8_t)raw;
        k.held[k.nheld++].code = code;
        uint32_t mods = held_mods();
        if (!mod_of(code)) {                      /* the last pressed key repeats */
            k.rep_on = k.delay != 0;
            k.rep_raw = (uint8_t)raw;
            k.rep_at = (uint32_t)now + k.delay;
        }
        send(vm, code, mods, true);
    } else {
        if (h) {                                  /* else a stray release: normal code */
            code = h->code;
            *h = k.held[--k.nheld];
        }
        if (k.rep_on && k.rep_raw == raw) k.rep_on = false;
        send(vm, code, held_mods(), false);
    }
}

static void reset(const Port *p) {
    memset(&k, 0, sizeof k);
    k.port = p;
}

static int kbd_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    const InputHw *hw = platform_input();
    (void)index;
    if (input_failed() || k.port || !hw->keys || !hw->kbd_open || !hw->kbd_pending || !hw->kbd_read ||
        hw->kbd_open() != 0) return -1;
    reset(p);
    int32_t d = n > 0 ? cfg[0] : 0, r = n > 1 ? cfg[1] : 0;
    k.delay = (uint16_t)(d < 0 ? 0 : d > 65535 ? 65535 : d);
    k.period = (uint16_t)(r <= 0 ? 50 : r > 65535 ? 65535 : r);
    return 0;
}

static void kbd_close(Port *p) {
    if (k.port != p) return;                      /* a refused open's cleanup */
    const InputHw *hw = platform_input();
    if (hw->kbd_close) hw->kbd_close();
    reset(NULL);
}

static bool kbd_can_event(const Port *p) {
    (void)p;
    const InputHw *hw = platform_input();
    return !hw->live || hw->live();
}

const PortDriver input_kbd_driver = {
    "kbd", TYPE_KBD_CONFIG, TYPE_KBD_KEY, kbd_open, NULL, kbd_close, kbd_can_event, NULL, NULL,
};

/* ---- scheduler side ---- */

static bool rep_due(uint64_t now) { return k.rep_on && reached(now, k.rep_at); }
static bool backing_off(uint64_t now) { return k.errs >= 2 && !reached(now, k.retry_at); }

bool kbd_is_open(void) { return k.port != NULL; }

bool kbd_work(uint64_t now) {
    return k.port && ((platform_input()->kbd_pending() && !backing_off(now)) || rep_due(now));
}

bool kbd_next_deadline(uint64_t now, uint64_t *d) {
    if (!k.port) return false;
    bool have = false;
    if (k.rep_on) { *d = deadline_abs(now, k.rep_at); have = true; }
    if (backing_off(now) && platform_input()->kbd_pending()) {   /* wake for the retry */
        uint64_t r = deadline_abs(now, k.retry_at);
        if (!have || r < *d) *d = r;
        have = true;
    }
    return have;
}

void kbd_drain(Vm *vm, uint64_t now) {
    const InputHw *hw = platform_input();
    uint16_t raw;
    bool down;
    if (!k.port) return;
    if (hw->kbd_pending() && !backing_off(now)) {
        for (uint32_t i = 0; i < 32u; i++) {      /* the chip's FIFO holds 10 */
            int r = hw->kbd_read(&raw, &down);
            if (r < 0) {
                read_errors++;
                /* The first failure retries at once; consecutive ones back
                 * off 10, 20, ... 640 ms so a dead chip or bus cannot spin
                 * the scheduler in I2C timeouts. */
                if (k.errs < 255) k.errs++;
                if (k.errs >= 2) k.retry_at = (uint32_t)now + (10u << (k.errs - 2 > 6 ? 6 : k.errs - 2));
                break;
            }
            k.errs = 0;
            if (r == 0) break;
            if (raw < hw->nkeys && raw < 256u && hw->keys[raw].code) key_event(vm, hw->keys, raw, down, now);
            if (!k.port) return;
        }
    }
    if (rep_due(now)) {                           /* one repeat per pass; no catch-up burst */
        const Held *h = find(k.rep_raw);
        if (h) send(vm, h->code, held_mods(), true);   /* Mods and Char as of now */
        k.rep_at += k.period;
        if (reached(now, k.rep_at)) k.rep_at = (uint32_t)now + k.period;
    }
}
