/* The "screen" port (screen_port.h). */
#include "screen_port.h"
#include "actor.h"
#include "alloc.h"
#include "bytes.h"
#include "heap.h"
#include "opcodes.h"
#include "host.h"
#include "stream.h"

#include <string.h>

#define INT31_MAX 0x7FFFFFFFu
#define RETRY_MS 10u   /* an owed Done is tried again this often */

typedef struct ScreenPort {
    const ScreenHw *hw;
    ScreenGrid g;
    uint32_t seq;                     /* frames drawn (the last Done's Seq) */
    int32_t owed[SCREEN_OWED_MAX];    /* Dones owed, oldest first ... */
    uint32_t refused;                 /* ... then this many Done{-1} */
    uint64_t retry_at;                /* vm_now of the next try while owed */
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

static bool owing(const ScreenPort *s) { return s->owed_n || s->refused; }

static bool flush(struct Vm *vm, Port *p, ScreenPort *s);

/* The retry source: while Dones are owed, try them every RETRY_MS (an
 * owner near its quota usually has room again once it waits: its heap
 * slack is collected first, as stream/ does). ctx is the Port. */
static bool src_pending(const struct Vm *vm, void *ctx) {
    const ScreenPort *s = sp(ctx);
    return s && owing(s) && vm_now(vm) >= s->retry_at;
}

static void src_drain(struct Vm *vm, void *ctx, uint64_t now) {
    ScreenPort *s = sp(ctx);
    if (!s || !owing(s) || now < s->retry_at) return;
    if (!flush(vm, ctx, s)) s->retry_at = now + RETRY_MS;
}

static bool src_any_live(const struct Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }

static bool src_deadline(const struct Vm *vm, void *ctx, uint64_t *d) {
    (void)vm;
    const ScreenPort *s = sp(ctx);
    if (!s || !owing(s)) return false;
    *d = s->retry_at;
    return true;
}

static const PortSource retry_source = { src_pending, src_drain, src_any_live, src_deadline, NULL };

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
    /* Without the retry source an owed Done would wait for the next Out
     * (a program waiting for it would hang): no slot, no screen. */
    if (!port_add_source(vm, &retry_source, p)) {
        va_free(vm, s, bytes);
        return -1;
    }
    if (hw->open && hw->open((uint8_t)cols, (uint8_t)rows) != 0) {
        port_remove_source(vm, &retry_source);
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

/* Delivers Done{From: p, Seq: seq}; once more after collecting a waiting
 * owner's heap slack (stream/'s rule). */
static bool send_done(struct Vm *vm, Port *p, int32_t seq) {
    int32_t v[2] = { (int32_t)p->id, seq };
    if (port_deliver_ints(vm, p, TYPE_DONE, v, 2, PORT_RAW(0))) return true;
    struct Actor *o = port_owner(vm, p);
    return o && o->state == A_WAITING && heap_reclaim_waiting(vm, o) &&
           port_deliver_ints(vm, p, TYPE_DONE, v, 2, PORT_RAW(0));
}

/* Sends the owed Dones, oldest first, until one does not fit; true when
 * none is owed. */
static bool flush(struct Vm *vm, Port *p, ScreenPort *s) {
    while (s->owed_n) {
        if (!send_done(vm, p, s->owed[0])) return false;
        memmove(&s->owed[0], &s->owed[1], (size_t)(s->owed_n - 1u) * sizeof s->owed[0]);
        s->owed_n--;
    }
    while (s->refused) {
        if (!send_done(vm, p, -1)) return false;
        s->refused--;
    }
    return true;
}

/* The next Done, in order: now if nothing is owed and it fits, else owed.
 * Owed Done{-1}s are counted, so none is ever lost. */
static void done(struct Vm *vm, Port *p, ScreenPort *s, int32_t seq) {
    if (flush(vm, p, s) && send_done(vm, p, seq)) return;
    if (s->owed_n < SCREEN_OWED_MAX && !s->refused) s->owed[s->owed_n++] = seq;
    else if (s->refused < UINT32_MAX) s->refused++;   /* only refusals get here (the caller's rule) */
    if (s->owed_n + s->refused == 1u) s->retry_at = vm_now(vm) + RETRY_MS;
}

void screen_port_command(struct Vm *vm, const struct Actor *from, Port *p, term msg) {
    ScreenPort *s = sp(p);
    if (!s) return;
    const uint32_t *o = obj_of(vm, from, msg);
    if (!o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_OUT || hdr_words(o[0]) != 3u) return;
    const uint8_t *d;
    uint32_t len;
    flush(vm, p, s);
    /* With SCREEN_OWED_MAX Dones owed, or refusals counted past them
     * (which must keep their place), the Out is not drawn: Done{-1}. */
    if (s->owed_n >= SCREEN_OWED_MAX || s->refused ||
        !bytes_view(vm, from, o[2], &d, &len) || len == 0 || !screen_ops_check(d, len)) {
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
    port_remove_source(vm, &retry_source);
    if (s->hw->close) s->hw->close();
    memset(p->state, 0, sizeof p->state);
    va_free(vm, s, s->bytes);
}

bool screen_port_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    const ScreenPort *s = sp(p);
    return s && owing(s);   /* an owed Done is still to come */
}

void screen_port_ignore(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)vm, (void)p, (void)type, (void)f, (void)n;
}
