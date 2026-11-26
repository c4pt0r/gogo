/* The "screen" port (screen_port.h). */
#include "screen_port.h"
#include "actor.h"
#include "alloc.h"
#include "bytes.h"
#include "heap.h"
#include "opcodes.h"
#include "stream.h"

#include <string.h>

#define INT31_MAX 0x7FFFFFFFu

typedef struct ScreenPort {
    const ScreenHw *hw;
    ScreenGrid g;
    uint32_t seq;                     /* frames drawn (the last Done's Seq) */
    int32_t owed[SCREEN_OWED_MAX];    /* Dones owed, oldest first */
    uint8_t owed_n;
    uint32_t bytes;                   /* this allocation, for va_free */
    uint32_t cells[];
} ScreenPort;

static ScreenPort *sp(const Port *p) {
    ScreenPort *s;
    memcpy(&s, p->state, sizeof s);
    return s;
}

const ScreenGrid *screen_port_grid(const Port *p) {
    ScreenPort *s = p && p->drv && p->drv->command_term == screen_port_command ? sp(p) : NULL;
    return s ? &s->g : NULL;
}

int screen_port_open(struct Vm *vm, Port *p, const int32_t *cfg, uint32_t n, const ScreenHw *hw) {
    if (!hw || !hw->present || n < 2) return -1;
    int32_t cols = cfg[0], rows = cfg[1];
    if (cols == 0 && rows == 0) cols = hw->def_cols, rows = hw->def_rows;
    if (cols < 1 || rows < 1 || cols > hw->max_cols || rows > hw->max_rows || cols > (int32_t)SCREEN_MAX_COLS ||
        rows > (int32_t)SCREEN_MAX_ROWS)
        return -1;
    for (unsigned i = 0; i < GOGO_MAX_PORTS; i++) {   /* one screen per Vm */
        const Port *q = &vm->ports[i];
        if (q != p && q->drv && q->drv->command_term == screen_port_command) return -1;
    }
    uint32_t bytes = (uint32_t)sizeof(ScreenPort) + (uint32_t)cols * (uint32_t)rows * 4u;
    ScreenPort *s = va_alloc(vm, bytes);
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    s->hw = hw;
    s->bytes = bytes;
    screen_init(&s->g, s->cells, (uint8_t)cols, (uint8_t)rows);
    if (hw->open && hw->open((uint8_t)cols, (uint8_t)rows) != 0) {
        va_free(vm, s, bytes);
        return -1;
    }
    memcpy(p->state, &s, sizeof s);
    p->charge_words = (uint16_t)((bytes + 3u) / 4u);   /* <= 8 KWords: fits */
    hw->present(&s->g, screen_take_dirty(&s->g));
    uint8_t info[4] = { (uint8_t)cols, (uint8_t)rows, 16, (uint8_t)(hw->attrs & SCREEN_A_ALL) };
    StreamIn in = { 0, 0 };
    stream_deliver_chunk(vm, p, &in, (uint32_t)vm_now(vm), info, sizeof info);
    return 0;
}

/* Sends the owed Dones, oldest first, until one does not fit; true when
 * none is owed. */
static bool flush(struct Vm *vm, Port *p, ScreenPort *s) {
    while (s->owed_n) {
        if (!port_deliver_ints(vm, p, TYPE_DONE, &s->owed[0], 1, 0)) return false;
        memmove(&s->owed[0], &s->owed[1], (size_t)(s->owed_n - 1u) * sizeof s->owed[0]);
        s->owed_n--;
    }
    return true;
}

static void done(struct Vm *vm, Port *p, ScreenPort *s, int32_t seq) {
    if (flush(vm, p, s) && port_deliver_ints(vm, p, TYPE_DONE, &seq, 1, 0)) return;
    if (s->owed_n < SCREEN_OWED_MAX) s->owed[s->owed_n++] = seq;
}

void screen_port_command(struct Vm *vm, const struct Actor *from, Port *p, term msg) {
    ScreenPort *s = sp(p);
    if (!s) return;
    const uint32_t *o = obj_of(vm, from, msg);
    if (!o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_OUT || hdr_words(o[0]) != 3u) return;
    const uint8_t *d;
    uint32_t len;
    flush(vm, p, s);
    if (s->owed_n >= SCREEN_OWED_MAX) return;          /* not drawn; its Done{-1} dropped */
    if (!bytes_view(vm, from, o[2], &d, &len) || len == 0 || !screen_ops_check(d, len)) {
        done(vm, p, s, -1);
        return;
    }
    screen_ops_apply(&s->g, d, len);
    uint32_t dirty = screen_take_dirty(&s->g);
    if (dirty) s->hw->present(&s->g, dirty);
    s->seq = s->seq >= INT31_MAX ? 1u : s->seq + 1u;
    done(vm, p, s, (int32_t)s->seq);
}

void screen_port_close(struct Vm *vm, Port *p) {
    ScreenPort *s = sp(p);
    if (!s) return;
    if (s->hw->close) s->hw->close();
    memset(p->state, 0, sizeof p->state);
    va_free(vm, s, s->bytes);
}

bool screen_port_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    (void)p;
    return false;   /* Dones answer Outs; an owed one waits for the next Out */
}

void screen_port_ignore(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)vm, (void)p, (void)type, (void)f, (void)n;
}
