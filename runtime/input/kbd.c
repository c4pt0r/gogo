/* The kbd port: raw key events -> Key messages (spec input §5.3). The state
 * is InputState.kbd (vm->input); it is cleared on open and close. */
#include "input_int.h"
#include <string.h>

#define MAX_HELD INPUT_MAX_HELD

typedef InputHeld Held;
typedef InputKbd Kbd;

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

static uint32_t held_mods(const Kbd *k) {
    uint32_t m = 0;
    for (uint8_t i = 0; i < k->nheld; i++) m |= mod_of(k->held[i].code);
    return m;
}

static Held *find(Kbd *k, uint8_t raw) {
    for (uint8_t i = 0; i < k->nheld; i++) {
        if (k->held[i].raw == raw) return &k->held[i];
    }
    return NULL;
}

static void send(Vm *vm, uint8_t code, uint32_t mods, bool down) {
    InputState *in = vm->input;
    int32_t v[4] = { code, input_key_char(mod_of(code) ? 0 : code, mods), (int32_t)mods, PORT_BOOL(down) };
    if (!port_deliver_ints(vm, in->kbd.port, TYPE_KBD_KEY, v, 4, PORT_RAW(3))) in->dropped++;
}

static void key_event(Vm *vm, const KbdKey *tab, uint16_t raw, bool down, uint64_t now) {
    Kbd *k = &vm->input->kbd;
    uint8_t code = tab[raw].code;
    Held *h = find(k, (uint8_t)raw);
    if (down) {
        if (h || k->nheld == MAX_HELD) return;     /* already held, or the 9th */
        if ((held_mods(k) & MOD_FN) && tab[raw].fn_code && !mod_of(code)) code = tab[raw].fn_code;
        k->held[k->nheld].raw = (uint8_t)raw;
        k->held[k->nheld++].code = code;
        uint32_t mods = held_mods(k);
        if (!mod_of(code)) {                      /* the last pressed key repeats */
            k->rep_on = k->delay != 0;
            k->rep_raw = (uint8_t)raw;
            k->rep_at = (uint32_t)now + k->delay;
        }
        send(vm, code, mods, true);
    } else {
        if (h) {                                  /* else a stray release: normal code */
            code = h->code;
            *h = k->held[--k->nheld];
        }
        if (k->rep_on && k->rep_raw == raw) k->rep_on = false;
        send(vm, code, held_mods(k), false);
    }
}

static void reset(Kbd *k, const Port *p) {
    memset(k, 0, sizeof *k);
    k->port = p;
}

static int kbd_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    const InputHw *hw = input_hw(vm);
    (void)index;
    if (input_failed(vm)) return -1;
    Kbd *k = &vm->input->kbd;
    if (k->port || !hw->keys || !hw->kbd_open || !hw->kbd_pending || !hw->kbd_read ||
        hw->kbd_open() != 0) return -1;
    reset(k, p);
    int32_t d = n > 0 ? cfg[0] : 0, r = n > 1 ? cfg[1] : 0;
    k->delay = (uint16_t)(d < 0 ? 0 : d > 65535 ? 65535 : d);
    k->period = (uint16_t)(r <= 0 ? 50 : r > 65535 ? 65535 : r);
    return 0;
}

static void kbd_close(struct Vm *vm, Port *p) {
    Kbd *k = vm->input ? &vm->input->kbd : NULL;
    if (!k || k->port != p) return;                      /* a refused open's cleanup */
    const InputHw *hw = input_hw(vm);
    if (hw->kbd_close) hw->kbd_close();
    reset(k, NULL);
}

static bool kbd_can_event(const struct Vm *vm, const Port *p) { (void)vm;
    (void)p;
    const InputHw *hw = input_hw(vm);
    return !hw->live || hw->live();
}

const PortDriver input_kbd_driver = {
    "kbd", TYPE_KBD_CONFIG, TYPE_KBD_KEY, kbd_open, NULL, kbd_close, kbd_can_event, NULL, NULL, NULL
};

/* ---- scheduler side ---- */

static bool rep_due(const Kbd *k, uint64_t now) { return k->rep_on && reached(now, k->rep_at); }
static bool backing_off(const Kbd *k, uint64_t now) { return k->errs >= 2 && !reached(now, k->retry_at); }

/* A pass that failed (a read error, or no progress on a stuck INT): the
 * first retries at once; consecutive ones back off 10, 20, ... 640 ms, so
 * a dead chip, bus or line cannot spin the scheduler. */
static void strike(Kbd *k, uint64_t now) {
    if (k->errs < 255) k->errs++;
    if (k->errs >= 2) k->retry_at = (uint32_t)now + (10u << (k->errs - 2 > 6 ? 6 : k->errs - 2));
}

bool kbd_work(const Vm *vm, uint64_t now) {
    const Kbd *k = &vm->input->kbd;
    return k->port && ((input_hw(vm)->kbd_pending() && !backing_off(k, now)) || rep_due(k, now));
}

bool kbd_next_deadline(const Vm *vm, uint64_t now, uint64_t *d) {
    const Kbd *k = &vm->input->kbd;
    if (!k->port) return false;
    bool have = false;
    if (k->rep_on) { *d = deadline_abs(now, k->rep_at); have = true; }
    if (backing_off(k, now) && input_hw(vm)->kbd_pending()) {   /* wake for the retry */
        uint64_t r = deadline_abs(now, k->retry_at);
        if (!have || r < *d) *d = r;
        have = true;
    }
    return have;
}

void kbd_drain(Vm *vm, uint64_t now) {
    InputState *in = vm->input;
    Kbd *k = &in->kbd;
    const InputHw *hw = input_hw(vm);
    uint16_t raw;
    bool down;
    if (!k->port) return;
    if (hw->kbd_pending() && !backing_off(k, now)) {
        for (uint32_t i = 0; i < 32u; i++) {      /* the chip's FIFO holds 10 */
            int r = hw->kbd_read(&raw, &down);
            if (r < 0) {
                in->read_errors++;
                strike(k, now);
                break;
            }
            if (r == 0) {
                /* Empty at once, yet still pending (INT held low with an
                 * empty FIFO): this pass made no progress. Once is the
                 * race kbd_read allows for (an event between the empty
                 * read and the clear: the next read gets it); twice in a
                 * row is a stuck line, which backs off like an error
                 * instead of spinning the scheduler (PortSource rule). */
                if (i == 0 && hw->kbd_pending()) {
                    if (k->errs >= 1) in->read_errors++;
                    strike(k, now);
                } else {
                    k->errs = 0;
                }
                break;
            }
            k->errs = 0;
            if (raw < hw->nkeys && raw < 256u && hw->keys[raw].code) key_event(vm, hw->keys, raw, down, now);
            if (!k->port) return;
        }
    }
    if (rep_due(k, now)) {                           /* one repeat per pass; no catch-up burst */
        const Held *h = find(k, k->rep_raw);
        if (h) send(vm, h->code, held_mods(k), true);   /* Mods and Char as of now */
        k->rep_at += k->period;
        if (reached(now, k->rep_at)) k->rep_at = (uint32_t)now + k->period;
    }
}
