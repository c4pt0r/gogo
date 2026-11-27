/* The "ble" port (ble.h). */
#include "ble.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "bytes.h"
#include "opcodes.h"
#include "host.h"

#include <string.h>

/* ---- the Config ---- */

static bool take_uuid(const uint8_t *d, uint32_t len, uint32_t *at, BleUuid *u) {
    if (*at >= len) return false;
    uint8_t n = d[(*at)++];
    if ((n != 2u && n != 16u) || len - *at < n) return false;
    u->len = n;
    memcpy(u->b, d + *at, n);
    *at += n;
    return true;
}

int ble_config_decode(const uint8_t *d, uint32_t len, BleConfig *c) {
    uint32_t at = 0;
    memset(c, 0, sizeof *c);
    if (!d || len < 1u) return GOGO_E_BADARG;
    uint32_t nl = d[at++];
    if (nl < 1u || nl > BLE_NAME_MAX || len - at < nl) return GOGO_E_BADARG;
    for (uint32_t i = 0; i < nl; i++)
        if (d[at + i] == 0) return GOGO_E_BADARG;
    memcpy(c->name, d + at, nl);
    at += nl;
    if (at >= len) return GOGO_E_BADARG;
    uint32_t ns = d[at++];
    if (ns < 1u) return GOGO_E_BADARG;
    if (ns > BLE_MAX_SERVICES) return GOGO_E_TOOMANY;
    for (uint32_t s = 0; s < ns; s++) {
        if (!take_uuid(d, len, &at, &c->svc[s]) || at >= len) return GOGO_E_BADARG;
        uint32_t nc = d[at++];
        if (nc < 1u) return GOGO_E_BADARG;
        if (nc > BLE_MAX_PER_SVC || c->nchr + nc > BLE_MAX_CHARS) return GOGO_E_TOOMANY;
        c->first[s] = c->nchr;
        for (uint32_t k = 0; k < nc; k++) {
            if (!take_uuid(d, len, &at, &c->chr[c->nchr]) || at >= len) return GOGO_E_BADARG;
            uint8_t f = d[at++];
            if (f < 1u || f > BLE_F_ALL) return GOGO_E_BADARG;
            c->flags[c->nchr++] = f;
        }
    }
    if (at != len) return GOGO_E_BADARG;
    c->nsvc = (uint8_t)ns;
    c->first[ns] = c->nchr;
    return 0;
}

/* ---- the queue ---- */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Where n contiguous bytes go, or -1: at head, or at 0 after a skip
 * mark; head never catches up with tail (that would read as empty). */
static int32_t fit(const BleBoard *b, uint32_t n) {
    uint32_t h = b->head, t = b->tail;
    if (h >= t) {
        if (n < BLE_QUEUE_BYTES - h || (n == BLE_QUEUE_BYTES - h && t != 0)) return (int32_t)h;
        return n < t ? 0 : -1;
    }
    return n < t - h ? (int32_t)h : -1;
}

bool ble_post(BleBoard *b, uint32_t ev, uint32_t arg, const uint8_t *data, uint32_t len) {
    if (!b->accepting || ev == 0 || ev > 255u || len > BLE_VALUE_MAX) return false;
    uint32_t n = BLE_REC_BYTES(len);
    int32_t at = fit(b, ev == BLE_EV_WRITTEN ? n + BLE_QUEUE_SPARE : n);
    if (at < 0) {
        b->lost++;
        return false;
    }
    if ((uint32_t)at != b->head) b->ring[b->head] = 0;   /* skip mark: the rest of the ring */
    uint8_t *r = b->ring + at;
    r[0] = (uint8_t)ev;
    r[1] = 0;
    r[2] = (uint8_t)len;
    r[3] = (uint8_t)(len >> 8);
    put32(r + 4, arg);
    if (len) memcpy(r + 8, data, len);
    b->head = ((uint32_t)at + n) % BLE_QUEUE_BYTES;
    return true;
}

uint32_t ble_lost(const BleHal *hal) {
    hal->lock(hal->ctx);
    uint32_t n = hal->board->lost;
    hal->unlock(hal->ctx);
    return n;
}

/* The record at tail (skipping a mark), or NULL when empty. Under the
 * lock; its bytes stay put until ble_pop (the radio side writes only past
 * head), so the caller reads them unlocked. */
static const uint8_t *peek(BleBoard *b) {
    if (b->tail != b->head && b->ring[b->tail] == 0) b->tail = 0;
    return b->tail == b->head ? NULL : b->ring + b->tail;
}

static void pop(BleBoard *b, const uint8_t *r) {
    uint32_t len = (uint32_t)r[2] | (uint32_t)r[3] << 8;
    b->tail = (b->tail + BLE_REC_BYTES(len)) % BLE_QUEUE_BYTES;
}

/* ---- events ---- */

static BleBoard *board_of(const Port *p) { return ((const BleHal *)p->drv->hw)->board; }
static const BleHal *hal_of(const Port *p) { return (const BleHal *)p->drv->hw; }
static StreamOut *out_of(Port *p) { return (StreamOut *)(void *)p->state; }

/* An int field: immediate, or a K_INT box at d[*at]. */
static term put_int(uint32_t *d, uint32_t *at, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    d[*at] = hdr_make(2, K_INT);
    d[*at + 1u] = (uint32_t)v;
    term t = term_from_ref(*at);
    *at += 2u;
    return t;
}

/* Sends p's owner port.Post{From: p, Kind: kind, Data: data} (inline
 * Bytes), without ever marking it; once more after collecting a waiting
 * owner's heap slack. False if it did not fit (the caller keeps it). */
static bool deliver(struct Vm *vm, const Port *p, int32_t kind, const uint8_t *data, uint32_t len) {
    Actor *o = port_owner(vm, p);
    if (!o) return true;                  /* nobody to tell: done with it */
    uint32_t words = 5u + (term_int_fits(kind) ? 0u : 2u) + (len ? 2u + (len + 3u) / 4u : 0u);
    Msg *m = msg_new(vm, words);
    if (!m) return false;
    uint32_t *d = m->data, at = 5;
    d[0] = hdr_make(5, K_STRUCT);
    d[1] = TYPE_POST;
    d[2] = p->id;
    d[3] = put_int(d, &at, kind);
    if (len) {
        uint32_t bw = 2u + (len + 3u) / 4u;
        d[at] = hdr_make(bw, K_BYTES);
        d[at + 1u] = len;
        d[at + bw - 1u] = 0;              /* padding */
        memcpy(&d[at + 2u], data, len);
        d[4] = term_from_ref(at);
    } else {
        d[4] = T_EMPTYBYTES;
    }
    for (int k = 0; k < 2; k++) {
        if (msg_try_enqueue(vm, o, m)) return true;
        if (k || !heap_reclaim_waiting(vm, o)) break;
    }
    msg_free(vm, m);
    return false;
}

/* Tells the owner why the Notify of characteristic c is about to get
 * Done{-1} (BLE_EV_NOTIFY_FAILED; best effort: the Done is what counts). */
static void notify_failed(struct Vm *vm, const Port *p, uint32_t c, uint32_t reason) {
    (void)deliver(vm, p, (int32_t)(BLE_EV_NOTIFY_FAILED | (c & 0xFFu) << 8 | (reason & 0xFFu) << 16), NULL, 0);
}

/* The head Notify failed with reason: NotifyFailed, then its Done{-1}. */
static void fail_head(struct Vm *vm, Port *p, BleBoard *b, uint32_t reason) {
    notify_failed(vm, p, b->chr[0], reason);
    b->chr[0] = b->chr[1];
    (void)stream_out_fail(vm, p, out_of(p));
}

/* The connection ended: every Notify in flight fails. */
static void fail_all(struct Vm *vm, Port *p) {
    BleBoard *b = board_of(p);
    while (out_of(p)->n) fail_head(vm, p, b, GOGO_E_CLOSED);
}

/* What an event says about the connection, once per record. */
static void apply(struct Vm *vm, Port *p, BleBoard *b, uint32_t ev, uint32_t arg) {
    switch (ev) {
    case BLE_EV_CONNECTED:
        b->connected = true;
        b->subs = 0;
        b->mtu = (uint16_t)(arg >= BLE_MTU_MIN && arg <= 0xFFFFu ? arg : BLE_MTU_MIN);
        break;
    case BLE_EV_DISCONNECTED:
        b->connected = false;
        b->subs = 0;
        b->mtu = BLE_MTU_MIN;
        fail_all(vm, p);
        break;
    case BLE_EV_SUBSCRIBED:
        if ((arg & 0xFFu) < BLE_MAX_CHARS) {
            uint16_t bit = (uint16_t)(1u << (arg & 0xFFu));
            b->subs = (uint16_t)(arg >> 8 & 1u ? b->subs | bit : b->subs & ~bit);
        }
        break;
    case BLE_EV_MTU:
        if (arg >= BLE_MTU_MIN && arg <= 0xFFFFu) b->mtu = (uint16_t)arg;
        break;
    default:
        break;
    }
}

/* ---- notifications ---- */

/* Hands the Notifies in flight to the stack, head first, while it takes
 * them; one it has no buffer for waits for BLE_RETRY_MS, for as long as
 * the connection lasts. */
static void push(struct Vm *vm, Port *p, BleBoard *b, uint64_t now) {
    const BleHal *hal = hal_of(p);
    StreamOut *o = out_of(p);
    while (o->n) {
        uint32_t len;
        const uint8_t *d = stream_out_head(vm, o, &len);
        int r = d ? hal->notify(hal->ctx, b->chr[0], d, len) : GOGO_E_BADARG;
        if (r == BLE_AGAIN) {
            if (!b->retry_at || b->retry_at > now + BLE_RETRY_MS) b->retry_at = now + BLE_RETRY_MS;
            return;
        }
        if (r == 0) {
            b->chr[0] = b->chr[1];
            (void)stream_out_done(vm, p, o);
        } else {
            fail_head(vm, p, b, r > 0 ? (uint32_t)r : GOGO_E_IO);   /* the stack refused it */
        }
    }
}

/* A Done the owner had no room for is owed (stream/): the source tries
 * it again in BLE_RETRY_MS. */
static void arm_owed(Port *p, BleBoard *b, uint64_t now) {
    if (out_of(p)->owed_n && (!b->retry_at || b->retry_at > now + BLE_RETRY_MS)) b->retry_at = now + BLE_RETRY_MS;
}

/* 0 if a Notify of characteristic c with len bytes can go now, else why
 * not (BLE_EV_NOTIFY_FAILED's reasons). */
static uint32_t notify_check(const BleBoard *b, uint32_t c, uint32_t len) {
    if (c >= b->cfg.nchr || !(b->cfg.flags[c] & BLE_F_NOTIFY) || len < 1u || len > BLE_VALUE_MAX) return GOGO_E_BADARG;
    if (!b->connected) return GOGO_E_CLOSED;
    if (!(b->subs >> c & 1u)) return GOGO_E_REFUSED;
    if (len + 3u > b->mtu) return GOGO_E_BADARG;
    return 0;
}

/* ---- the event source (registered while the port is open; ctx: the Port) ---- */

static bool queued(const BleHal *hal) {
    hal->lock(hal->ctx);
    bool any = hal->board->head != hal->board->tail;
    hal->unlock(hal->ctx);
    return any;
}

static bool src_pending(const struct Vm *vm, void *ctx) {
    const Port *p = ctx;
    const BleBoard *b = board_of(p);
    if (b->retry_at) return vm_now(vm) >= b->retry_at;
    return queued(hal_of(p));
}

static void drain_queue(struct Vm *vm, Port *p, BleBoard *b, uint64_t now);

static void src_drain(struct Vm *vm, void *ctx, uint64_t now) {
    Port *p = ctx;
    const BleHal *hal = hal_of(p);
    BleBoard *b = hal->board;
    if (b->retry_at) {
        if (now < b->retry_at) return;
        b->retry_at = 0;
    }
    if (!stream_out_flush(vm, p, out_of(p))) b->retry_at = now + BLE_RETRY_MS;
    drain_queue(vm, p, b, now);
    /* After the events: a Notify waiting for a buffer fails, not goes out,
     * once the phone is gone. */
    push(vm, p, b, now);
    arm_owed(p, b, now);
}

/* The queue, in order, while the owner takes the events. */
static void drain_queue(struct Vm *vm, Port *p, BleBoard *b, uint64_t now) {
    const BleHal *hal = hal_of(p);
    for (int k = 0; k < 16; k++) {
        hal->lock(hal->ctx);
        const uint8_t *r = peek(b);
        hal->unlock(hal->ctx);
        if (!r) return;
        uint32_t ev = r[0], len = (uint32_t)r[2] | (uint32_t)r[3] << 8, arg = get32(r + 4);
        if (!b->seen) {
            apply(vm, p, b, ev, arg);
            b->seen = true;
        }
        if (!deliver(vm, p, (int32_t)(ev | arg << 8), r + 8, len)) {
            if (!b->retry_at || b->retry_at > now + BLE_RETRY_MS) b->retry_at = now + BLE_RETRY_MS;
            return;
        }
        hal->lock(hal->ctx);
        pop(b, r);
        hal->unlock(hal->ctx);
        b->seen = false;
    }
    /* more may wait: pending says so, and the scheduler comes back */
}

static bool src_any_live(const struct Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }

static bool src_deadline(const struct Vm *vm, void *ctx, uint64_t *d) {
    (void)vm;
    const BleBoard *b = board_of(ctx);
    if (!b->retry_at) return false;
    *d = b->retry_at;
    return true;
}

static const PortSource ble_source = { src_pending, src_drain, src_any_live, src_deadline, NULL };

/* ---- the driver ---- */

int ble_open(struct Vm *vm, const struct Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)index;
    const BleHal *hal = hal_of(p);
    BleBoard *b = hal->board;
    const uint32_t *o = obj_of(vm, owner, cfg);   /* a port.Host: port_open checked */
    int32_t fmt;
    const uint8_t *d;
    uint32_t len;
    if (hdr_words(o[0]) != 4u || !int_val(vm, owner, o[2], &fmt) || !bytes_view(vm, owner, o[3], &d, &len) ||
        fmt != BLE_CFG_FORMAT)
        return GOGO_E_BADARG;
    if (b->port) return GOGO_E_BUSY;      /* one port per board */
    BleConfig *c = &b->cfg;
    int r = ble_config_decode(d, len, c);
    if (r) return r;
    if (!port_add_source(vm, &ble_source, p)) return GOGO_E_BUSY;
    hal->lock(hal->ctx);
    b->head = b->tail = 0;
    b->accepting = true;
    hal->unlock(hal->ctx);
    r = hal->start(hal->ctx, c);
    if (r) {
        hal->lock(hal->ctx);
        b->accepting = false;
        b->head = b->tail = 0;
        hal->unlock(hal->ctx);
        port_remove_source(vm, &ble_source);
        return r > 0 ? r : GOGO_E_IO;
    }
    b->port = p;
    b->mtu = BLE_MTU_MIN;
    b->subs = 0;
    b->connected = b->seen = false;
    b->retry_at = 0;
    memset(out_of(p), 0, sizeof(StreamOut));
    /* The largest event, a Written of BLE_VALUE_MAX bytes, must fit. */
    p->room_64w = (uint8_t)((5u + 2u + 2u + BLE_VALUE_MAX / 4u + 63u) / 64u);
    return 0;
}

void ble_command(struct Vm *vm, const struct Actor *from, Port *p, term msg) {
    BleBoard *b = board_of(p);
    const BleHal *hal = hal_of(p);
    const uint32_t *o = obj_of(vm, from, msg);
    int32_t kind;
    const uint8_t *d;
    uint32_t len;
    if (b->port != p || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_POST || hdr_words(o[0]) != 5u ||
        !int_val(vm, from, o[3], &kind) || !bytes_view(vm, from, o[4], &d, &len))
        return;
    uint32_t cmd = (uint32_t)kind & 0xFFu, c = (uint32_t)kind >> 8;
    if (cmd == BLE_CMD_SET_VALUE) {
        if (c < b->cfg.nchr && len <= BLE_VALUE_MAX) (void)hal->set_value(hal->ctx, c, d, len);
    } else if (cmd == BLE_CMD_NOTIFY) {
        StreamOut *so = out_of(p);
        uint32_t n = so->n;
        uint32_t why = kind < 0 ? GOGO_E_BADARG : notify_check(b, c, len);
        if (!why && !stream_out_has_room(so)) why = GOGO_E_TOOMANY;
        if (why) notify_failed(vm, p, c, why);
        uint64_t now = vm_now(vm);
        int r = stream_out_accept_data(vm, p, so, o[4], why ? NULL : d, len);
        if (r >= 0) b->chr[n] = (uint8_t)c;
        if (r == 1) push(vm, p, b, now);
        arm_owed(p, b, now);
    }
}

void ble_close(struct Vm *vm, Port *p) {
    const BleHal *hal = hal_of(p);
    BleBoard *b = hal->board;
    if (b->port != p) return;
    hal->stop(hal->ctx);
    hal->lock(hal->ctx);
    b->accepting = false;
    b->head = b->tail = 0;
    hal->unlock(hal->ctx);
    stream_out_close(vm, p, out_of(p));
    port_remove_source(vm, &ble_source);
    b->port = NULL;
    b->connected = false;
    b->retry_at = 0;
}

bool ble_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    return board_of(p)->port == p;
}
