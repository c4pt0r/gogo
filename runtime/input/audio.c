/* The mic and spk ports (spec audio §1, §3): chip-independent audio on top
 * of the board's hooks (InputHw mic_* / spk_*), on the scheduler thread.
 * PCM is 16-bit little-endian mono at Rate 8000 or 16000.
 *
 *  - mic: audio.Mic{Rate, FrameMs, Gain}. FrameMs 0 is 100, else clamped
 *    to 20..500; Gain (dB) is clamped to 0..24 and rounded down to a 6 dB
 *    step. mic_read is polled every min(FrameMs, 50) ms, read until it
 *    returns 0 (at most MIC_READS_PER_POLL reads), and what it gives is
 *    gathered into frames of Rate * FrameMs / 1000 * 2 bytes; each full
 *    frame is one port.Chunk (stream_deliver_chunk). A late poll (the
 *    scheduler held up by an SD write) catches up from the hardware buffer
 *    and loses nothing. Dropped counts whole frames lost: Chunks over the
 *    owner's quota, and bytes the hardware lost to an overflow as
 *    lost / frame bytes rounded up (per report), which also move TimeMs on;
 *    a partly filled frame before the gap, and the bytes after it up to
 *    the next frame boundary, are discarded with them, so a Chunk is
 *    always contiguous and its TimeMs is exact.
 *    TimeMs of a Chunk is its frame's nominal start: the open plus FrameMs
 *    per frame before it (lost ones included). The frame buffer is
 *    allocated on open (VM allocator, charged to the owner through
 *    Port.charge_words) and freed on close; closing (or the owner exiting)
 *    discards a partly filled frame.
 *    Memory: with F = frame bytes / 4 words, the owner pays F for the
 *    buffer and F + MIC_CHUNK_EXTRA for each Chunk waiting in its mailbox
 *    (and again for the one it holds). Open is refused (Port.room_64w)
 *    unless one Chunk fits beside the buffer and the owner's heap, so a
 *    frame no Chunk of which could ever be delivered is ok=false, not
 *    silence. To ride out a stall of S ms the owner needs its heap +
 *    F + (ceil(S / FrameMs) + 1) * (F + MIC_CHUNK_EXTRA) words: at 16 kHz
 *    and 100 ms (F = 800) a 300 ms stall is heap + 4040 words, which a
 *    recorder with a real heap does not have under the MCU default limit
 *    (4096 words), so gogo/sound's recorder spawns with MaxMemory.
 *  - spk: audio.Speaker{Rate, Volume}, Volume clamped to 0..100. Every
 *    command is a port.Out of PCM (stream_out_accept: even, non-empty
 *    length; a bad one, or a third in flight, is Done{-1}). The Outs in
 *    flight are written with spk_write in order, as much as the hardware
 *    takes per call, the queued one right after the head (no gap between
 *    them); a running total of bytes written gives each Out its end mark,
 *    and an Out has played once written - spk_queued() reaches it: then
 *    stream_out_done sends (or owes) Done{Seq}. A write error fails that
 *    Out (Done{-1}, in order). Polled every SPK_POLL_MS while an Out is in
 *    flight or a Done is owed (stream_out_flush on every poll), so a
 *    program that keeps two Outs in flight and sends the next on each Done
 *    plays without gaps given a hardware buffer above SPK_POLL_MS. Closing
 *    cuts at once: it forgets what is in flight (stream_out_close, no Done)
 *    and spk_close stops the hardware and switches the amplifier off.
 *  - Half duplex: one mic or one spk port at a time, never both; a second
 *    open is refused before any hook is called, so the open one keeps
 *    running untouched.
 *
 * Delivery never kills the owner (stream/). */
#include "input_int.h"
#include "platform.h"
#include "stream.h"
#include "alloc.h"
#include <string.h>

enum { RATE_8K = 8000, RATE_16K = 16000, DEF_FRAME_MS = 100, MIN_FRAME_MS = 20, MAX_FRAME_MS = 500,
       MAX_GAIN = 24, GAIN_STEP = 6, MAX_VOLUME = 100, MIC_MAX_POLL = 50, MIC_READS_PER_POLL = 64,
       MIC_CHUNK_EXTRA = 10,   /* a Chunk's words beside its data: struct, an int box, the Bytes header */
       SPK_POLL_MS = 10, SPK_WRITES_PER_POLL = 16 };

static struct {
    const Port *mic;           /* the open mic, NULL = closed */
    uint8_t *buf;              /* one frame */
    StreamIn in;
    uint32_t t0;               /* ms, wrapping: the open */
    uint32_t frames;           /* frames made (delivered, dropped or lost) */
    uint32_t mic_next;         /* ms, wrapping: the next mic_read poll */
    uint16_t cap, fill;        /* frame bytes, bytes in buf */
    uint16_t skip;             /* bytes still to discard after a gap (to a frame boundary) */
    uint16_t frame_ms, poll_ms;
    bool more;                 /* the last poll read something: more may wait */
    Port *spk;                 /* the open spk, NULL = closed; its StreamOut is spk->state */
    uint32_t written;          /* bytes taken by spk_write since the open, wrapping */
    uint32_t end[STREAM_OUT_MAX];   /* Out k fully written: `written` at its end */
    uint32_t cur;              /* bytes of Out `wi` written */
    uint32_t spk_next;         /* ms, wrapping: the next spk poll */
    uint8_t wi;                /* the Out being written; o->n when all are */
    uint8_t bad;               /* bit k: Out k failed to write (Done{-1} when it is the head) */
} au;

static StreamOut *out_of(Port *p) { return (StreamOut *)(void *)p->state; }

static bool rate_ok(int32_t r) { return r == RATE_8K || r == RATE_16K; }

/* ---- mic ---- */

static int mic_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    const InputHw *hw = platform_input();
    if (input_failed() || au.mic || au.spk || !hw->mic_open || !hw->mic_read || n < 3) return -1;
    int32_t rate = cfg[0], ms = cfg[1], gain = cfg[2];
    if (!rate_ok(rate)) return -1;
    ms = ms == 0 ? DEF_FRAME_MS : ms < MIN_FRAME_MS ? MIN_FRAME_MS : ms > MAX_FRAME_MS ? MAX_FRAME_MS : ms;
    gain = gain < 0 ? 0 : gain > MAX_GAIN ? MAX_GAIN : gain - gain % GAIN_STEP;
    uint32_t cap = (uint32_t)rate * (uint32_t)ms / 1000u * 2u;   /* at most 16000 */
    uint8_t *buf = va_alloc(vm, cap);
    if (!buf) return -1;
    if (hw->mic_open((uint32_t)rate, (uint8_t)gain) != 0) {
        va_free(vm, buf, cap);
        return -1;
    }
    memset(&au.in, 0, sizeof au.in);
    au.buf = buf;
    au.cap = (uint16_t)cap;
    au.fill = 0;
    au.skip = 0;
    au.frames = 0;
    au.frame_ms = (uint16_t)ms;
    au.poll_ms = (uint16_t)(ms < MIC_MAX_POLL ? ms : MIC_MAX_POLL);
    au.t0 = (uint32_t)platform_now_ms();
    au.mic_next = au.t0 + au.poll_ms;
    au.more = true;                            /* the hardware may hold data from the start */
    au.mic = p;
    p->charge_words = (uint16_t)(cap / 4u);    /* the owner pays; port_open refuses one that cannot */
    p->room_64w = (uint8_t)((cap / 4u + MIC_CHUNK_EXTRA + 63u) / 64u);   /* ... or that could never take a Chunk */
    return 0;
}

static void mic_close(struct Vm *vm, Port *p) {
    if (au.mic != p) return;                   /* a refused open's cleanup */
    const InputHw *hw = platform_input();
    if (hw->mic_close) hw->mic_close();
    va_free(vm, au.buf, au.cap);
    au.buf = NULL;
    au.mic = NULL;
}

static void mic_command(Port *p, uint32_t type, const int32_t *f, uint32_t n) { (void)p; (void)type; (void)f; (void)n; }

static bool hw_live(void) {
    const InputHw *hw = platform_input();
    return !hw->live || hw->live();
}

static bool mic_can_event(const Port *p) {
    (void)p;
    return hw_live() || au.more;               /* dead hardware may still hold captured data */
}

const PortDriver input_mic_driver = {
    "mic", TYPE_AUDIO_MIC, TYPE_CHUNK, mic_open, mic_command, mic_close, mic_can_event, NULL, NULL, NULL
};

/* ---- spk ---- */

static bool pcm_ok(const Port *p, const uint8_t *d, uint32_t len) { (void)p; (void)d; return len % 2u == 0; }

static void spk_reset(void) {
    au.written = au.cur = 0;
    au.wi = au.bad = 0;
}

static int spk_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)index;
    const InputHw *hw = platform_input();
    if (input_failed() || au.mic || au.spk || !hw->spk_open || !hw->spk_write || n < 2) return -1;
    int32_t rate = cfg[0], vol = cfg[1];
    if (!rate_ok(rate)) return -1;
    vol = vol < 0 ? 0 : vol > MAX_VOLUME ? MAX_VOLUME : vol;
    if (hw->spk_open((uint32_t)rate, (uint8_t)vol) != 0) return -1;
    memset(out_of(p), 0, sizeof(StreamOut));
    au.spk = p;
    spk_reset();
    return 0;
}

static void spk_close(struct Vm *vm, Port *p) {
    (void)vm;
    if (au.spk != p) return;                   /* a refused open's cleanup */
    stream_out_close(NULL, p, out_of(p));      /* port_close releases the held data */
    au.spk = NULL;
    spk_reset();
    const InputHw *hw = platform_input();
    if (hw->spk_close) hw->spk_close();
}

static void spk_command(Vm *vm, const Actor *from, Port *p, term msg) {
    if (au.spk != p) return;
    stream_out_accept(vm, from, p, out_of(p), msg, pcm_ok);
    au.spk_next = (uint32_t)platform_now_ms();   /* write it (or flush a Done{-1}) at once */
}

static bool spk_can_event(const Port *p) {
    const StreamOut *o = (const StreamOut *)(const void *)p->state;
    return o->n || o->owed_n;                  /* Dones are coming */
}

const PortDriver input_spk_driver = {
    "spk", TYPE_AUDIO_SPEAKER, TYPE_DONE, spk_open, NULL, spk_close, spk_can_event, NULL, spk_command, NULL
};

/* ---- scheduler side ---- */

bool audio_is_open(void) { return au.mic || au.spk; }

static bool spk_waits(void) {
    const StreamOut *o = au.spk ? out_of(au.spk) : NULL;
    return o && (o->n || o->owed_n);
}

bool audio_work(uint64_t now) {
    return (au.mic && reached(now, au.mic_next)) || (spk_waits() && reached(now, au.spk_next));
}

bool audio_next_deadline(uint64_t now, bool live, uint64_t *d) {
    bool any = false;
    if (au.mic && (live || au.more)) { *d = deadline_abs(now, au.mic_next); any = true; }
    if (spk_waits()) {                         /* playing finishes on dead hardware too */
        uint64_t t = deadline_abs(now, au.spk_next);
        if (!any || t < *d) *d = t;
        any = true;
    }
    return any;
}

static void mic_drain(Vm *vm, uint64_t now) {
    const InputHw *hw = platform_input();
    au.mic_next = (uint32_t)now + au.poll_ms;
    bool got = false;
    for (int k = 0; k < MIC_READS_PER_POLL; k++) {
        uint32_t at = au.fill, room = (uint32_t)(au.cap - at), lost = 0;
        int r = hw->mic_read(au.buf + at, room, &lost);
        if (lost) {                            /* a gap before this read's bytes */
            /* The partly filled frame goes with it (a Chunk never spans a
             * gap), and so does the rest of the last frame the gap reaches,
             * from the bytes after it (skip): the next Chunk starts on a
             * frame boundary, so its TimeMs is its first sample's. */
            uint32_t gap = au.fill + lost;
            uint32_t f = gap / au.cap + (gap % au.cap != 0);
            au.in.dropped_since += f;
            au.frames += f;
            au.skip = (uint16_t)(f * au.cap - gap);
            au.fill = 0;
        }
        if (r <= 0 || (uint32_t)r > room) break;   /* drained, an error (retried), or a broken hook */
        got = true;
        uint32_t n = (uint32_t)r, s = au.skip < n ? au.skip : n;
        au.skip = (uint16_t)(au.skip - s);
        n -= s;
        if (n && at + s != au.fill) memmove(au.buf + au.fill, au.buf + at + s, n);
        au.fill = (uint16_t)(au.fill + n);
        if (au.fill < au.cap) continue;        /* a short read: more may wait */
        uint32_t t = au.t0 + au.frames * au.frame_ms;
        au.frames++;
        au.fill = 0;                           /* sent or dropped: never kept for later */
        if (!stream_deliver_chunk(vm, au.mic, &au.in, t, au.buf, au.cap)) dropped_inc();
    }
    au.more = got;
}

/* The head is gone (done or failed): the marks of the queued one move up. */
static void spk_shift(void) {
    for (uint32_t k = 1; k < STREAM_OUT_MAX; k++) au.end[k - 1u] = au.end[k];
    au.bad = (uint8_t)(au.bad >> 1);
    if (au.wi) au.wi--;
}

/* Finishes the heads that failed or have played. */
static void spk_retire(Vm *vm, Port *p, StreamOut *o) {
    const InputHw *hw = platform_input();
    uint32_t played = au.written - (hw->spk_queued ? hw->spk_queued() : 0u);
    while (o->n && au.wi > 0) {                /* the head is all written */
        if (au.bad & 1u) stream_out_fail(vm, p, o);
        else if ((int32_t)(played - au.end[0]) >= 0) stream_out_done(vm, p, o);
        else return;                           /* still playing */
        spk_shift();
    }
}

/* Writes the Outs in flight in order, from the cursor, while the hardware
 * takes them. */
static void spk_write_ahead(Vm *vm, Port *p, StreamOut *o) {
    const InputHw *hw = platform_input();
    for (int writes = 0; au.wi < o->n && writes < SPK_WRITES_PER_POLL; writes++) {
        uint32_t len;
        const uint8_t *d = stream_out_at(vm, o, au.wi, &len);
        int r = hw->spk_write(d + au.cur, len - au.cur);
        if (r == 0) return;                    /* its buffer is full: the next poll */
        if (r < 0 || (uint32_t)r > len - au.cur) {   /* an error (or a broken hook): not played */
            au.cur = 0;
            if (au.wi == 0) {
                stream_out_fail(vm, p, o);     /* the head: Done{-1} now */
                spk_shift();
            } else {
                au.bad = (uint8_t)(au.bad | 1u << au.wi);   /* Done{-1} in its turn */
                au.end[au.wi++] = au.written;
            }
            continue;
        }
        au.written += (uint32_t)r;
        au.cur += (uint32_t)r;
        if (au.cur == len) {
            au.end[au.wi++] = au.written;
            au.cur = 0;
        }
    }
}

static void spk_drain(Vm *vm, uint64_t now) {
    Port *p = au.spk;
    StreamOut *o = out_of(p);
    stream_out_flush(vm, p, o);
    au.spk_next = (uint32_t)now + SPK_POLL_MS;
    spk_retire(vm, p, o);
    spk_write_ahead(vm, p, o);
    spk_retire(vm, p, o);                      /* a hardware that played at once */
}

void audio_drain(Vm *vm, uint64_t now) {
    if (au.mic && reached(now, au.mic_next)) mic_drain(vm, now);
    if (spk_waits() && reached(now, au.spk_next)) spk_drain(vm, now);
}
