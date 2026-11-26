/* The power port: samples the battery chip on the scheduler thread and
 * reports Battery events (spec input §5.2, §5.5). MilliVolts is the average
 * of the last 4 good samples; Percent comes from it. A Battery event goes
 * out on open, on Read, at once when Charging or External changes, and when
 * Percent moves 2 or more from the last reported one. The state is cleared
 * on open and close. */
#include "input_int.h"
#include <string.h>

enum { AVG = INPUT_PWR_AVG, DEF_PERIOD = 10000, MIN_PERIOD = 1000 };

typedef InputPower Power;

static bool power_due(const Vm *vm, uint64_t now) {
    const InputHw *hw = input_hw(vm);
    Power *w = &vm->input->pwr;
    if (!w->port) return false;
    bool pend = hw->power_pending && hw->power_pending();
    if (w->backoff && !w->fail_pend && pend) w->backoff = false;   /* the flag rose again: a new value */
    if (w->start) return true;
    if (!hw->power_event_driven && reached(now, w->next_at)) return true;
    return !w->backoff && (w->read_req || pend);
}

static int percent(uint32_t mv) {
    if (mv <= 3300u) return 0;
    uint32_t p = (mv - 3300u) * 100u / 800u;
    return (int)(p > 100u ? 100u : p);
}

static int power_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    if (input_failed(vm)) return -1;
    Power *w = &vm->input->pwr;
    if (w->port || !input_hw(vm)->power_read) return -1;
    memset(w, 0, sizeof *w);
    int32_t per = n > 0 ? cfg[0] : 0;
    w->period = (uint16_t)(per <= 0 ? DEF_PERIOD : per < MIN_PERIOD ? MIN_PERIOD : per > 65535 ? 65535 : per);
    w->port = p;                /* next_at = 0: the first sample is due now */
    w->start = true;
    if (input_hw(vm)->power_enable) input_hw(vm)->power_enable(true);
    return 0;
}

static void power_close(struct Vm *vm, Port *p) {
    Power *w = vm->input ? &vm->input->pwr : NULL;
    if (!w || w->port != p) return;                /* a refused open's cleanup */
    memset(w, 0, sizeof *w);
    if (input_hw(vm)->power_enable) input_hw(vm)->power_enable(false);
}

static void power_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)f; (void)n;
    Power *w = &vm->input->pwr;
    if (w->port != p || type != TYPE_POWER_READ) return;
    w->read_req = true;
    /* A Read always gets one attempt, even after a dropped event or a failed
     * read (event-driven mode has no next_at to end the backoff). It cannot
     * spin: a failed attempt sets backoff again. */
    w->backoff = false;
}

static bool power_can_event(const struct Vm *vm, const Port *p) { (void)vm;
    (void)p;
    const InputHw *hw = input_hw(vm);
    return !hw->live || hw->live();
}

const PortDriver input_power_driver = {
    PORT_NAME_POWER, TYPE_POWER_CONFIG, TYPE_POWER_BATTERY, power_open, power_command, power_close, power_can_event, NULL, NULL, NULL
};

/* ---- scheduler side ---- */

bool power_work(const Vm *vm, uint64_t now) { return power_due(vm, now); }

bool power_next_deadline(const Vm *vm, uint64_t now, uint64_t *d) {
    const Power *w = &vm->input->pwr;
    if (!w->port || input_hw(vm)->power_event_driven) return false;
    *d = w->start ? now : deadline_abs(now, w->next_at);
    return true;
}

void power_drain(Vm *vm, uint64_t now) {
    int32_t mv;
    int8_t ch, ext;
    if (!power_due(vm, now)) return;
    Power *w = &vm->input->pwr;
    bool forced = w->read_req;                     /* stays set until an event is delivered */
    w->next_at = (uint32_t)now + w->period;           /* a failure or drop retries then, not sooner */
    w->backoff = true;
    w->start = false;
    const InputHw *hw = input_hw(vm);
    w->fail_pend = true;                           /* until proven otherwise (a drop below) */
    if (hw->power_read(&mv, &ch, &ext) != 0) {
        /* Retry at the period, or earlier when the flag rises again. A flag
         * that was already set stays stuck (a failed read does not consume
         * it), so it must not retry at once: that would spin. */
        w->fail_pend = hw->power_pending && hw->power_pending();
        return;
    }
    uint16_t s = (uint16_t)(mv < 0 ? 0 : mv > 65535 ? 65535 : mv);
    if (!w->have) {                                /* the first sample fills the average */
        for (uint8_t i = 0; i < AVG; i++) w->mv[i] = s;
        w->have = true;
    } else {
        w->mv[w->idx] = s;
        w->idx = (uint8_t)((w->idx + 1u) % AVG);
    }
    uint32_t avg = ((uint32_t)w->mv[0] + w->mv[1] + w->mv[2] + w->mv[3]) / AVG;
    int pct = percent(avg);
    int diff = pct - w->last_pct;
    if (!(forced || !w->reported || ch != w->last_ch || ext != w->last_ext || diff >= 2 || diff <= -2)) {
        w->backoff = false;
        return;
    }
    int32_t v[4] = { (int32_t)avg, pct, ch, ext };
    if (!port_deliver_ints(vm, w->port, TYPE_POWER_BATTERY, v, 4, 0)) {
        dropped_inc(vm);                            /* not reported: the next sample tries again */
        w->fail_pend = hw->power_pending && hw->power_pending();   /* the read consumed the flag: a new one ends the backoff */
        return;
    }
    w->backoff = false;
    w->read_req = false;
    w->reported = true;
    w->last_pct = (int8_t)pct; w->last_ch = ch; w->last_ext = ext;
}
