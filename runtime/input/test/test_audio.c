/* The mic and spk ports against the fake devices (fakehw.c) on a real VM: Config
 * validation and clamping
 * (Rate, FrameMs, Gain, Volume), frame sizing and the charged frame
 * buffer, frames in order, a 300 ms poll stall absorbed by the hardware
 * buffer and a longer one counted in Dropped, over-quota Chunks, close with
 * a partly filled frame, read errors, the poll deadline after EOF; half
 * duplex both ways; an Out larger than the hardware takes at once written
 * in parts with Done only after it played, refused Outs, write errors, and
 * close while playing (no Done, amplifier off). Time is the fake clock;
 * each step runs port_drain, as the scheduler does. */
#include "test.h"
#include "test_host.h"
#include "fakehw.h"
#include "actor.h"
#include "signals.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "evq.h"
#include "bytes.h"
#include "stream.h"
#include <string.h>


static uint64_t now;
static uint64_t clock_fn(void) { return now; }

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;
static Vm vm;
static Actor *owner;
#define ROOMY 20000u

static void mini_image(void) {
    uint32_t *w = img_w;
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
}

static const PortDriver *const drivers[] = {
    &input_kbd_driver, &input_power_driver, &input_imu_driver, &input_ir_tx_driver, &input_ir_rx_driver,
    &input_mic_driver, &input_speaker_driver, NULL
};

static void setup(void) {
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    fake_reset();
    now = 0;
    test_host_set_clock(clock_fn);
    mini_image();
    CHECK_EQ(test_vm_init(&vm, &img, 16), 0);
    port_set_drivers(&vm, drivers);
    fake_install(&vm);
    owner = actor_new(&vm, 2048);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
    owner->max_words = ROOMY;                            /* 16 KB frames and 6 KB Outs, in the MCU config too */
}

static void teardown(void) {
    port_close_all(&vm);
    test_host_set_clock(NULL);
}

static term heap_struct(Actor *a, uint32_t type, uint32_t n, const term *f) {
    uint32_t off = heap_alloc(&vm, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

static term heap_str(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term heap_bytes(Actor *a, const uint8_t *d, uint32_t len) {
    uint32_t words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_BYTES);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, d, len);
    return term_from_ref(off);
}

static term open_mic(Actor *a, int rate, int frame_ms, int gain) {
    a->hp = 0;
    term name = heap_str(a, "mic");
    term f[3] = { term_from_int(rate), term_from_int(frame_ms), term_from_int(gain) };
    return port_open(&vm, a, name, heap_struct(a, TYPE_AUDIO_MIC, 3, f));
}

static term open_spk(Actor *a, int rate, int volume) {
    a->hp = 0;
    term name = heap_str(a, "speaker");
    term f[2] = { term_from_int(rate), term_from_int(volume) };
    return port_open(&vm, a, name, heap_struct(a, TYPE_AUDIO_SPEAKER, 2, f));
}

/* PCM of n bytes: byte i is (i + first) & 0xFF. */
static void send_out(Actor *a, term spk, uint32_t n, uint8_t first) {
    static uint8_t b[16384];
    for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)(i + first);
    a->hp = 0;
    term d = heap_bytes(a, b, n);
    port_command(&vm, a, spk, heap_struct(a, TYPE_OUT, 1, &d));
}

static void step(uint64_t t) { now = t; port_drain(&vm, t); }

static int32_t word_int(const Msg *m, term t) {
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t));
    const uint32_t *o = &m->data[term_ref_off(t)];
    CHECK_EQ(o[0], hdr_make(2, K_INT));
    return (int32_t)o[1];
}

/* Everything the owner received, in order: a Done (len -1, seq) or a
 * Chunk: its samples' first and last value (the fake mic's sample i is
 * (uint16)i) and whether they count up by one. */
typedef struct { int32_t seq, time, dropped; int len; uint16_t first, last; bool counts_up; } Ev;
static Ev got[64];
static int ngot;

static void collect(Actor *a) {
    ngot = 0;
    for (Msg *m = a->mbox_head; m; m = a->mbox_head) {
        Ev *e = &got[ngot < 64 ? ngot : 63];
        memset(e, 0, sizeof *e);
        if (m->data[1] == TYPE_DONE) {
            CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
            CHECK(term_is_pid(m->data[2]));   /* From */
            e->len = -1;
            e->seq = word_int(m, m->data[3]);
        } else {
            CHECK_EQ(m->data[0], hdr_make(7, K_STRUCT));
            CHECK_EQ(m->data[1], TYPE_CHUNK);
            CHECK(term_is_pid(m->data[2]));   /* From */
            e->seq = word_int(m, m->data[3]);
            e->time = word_int(m, m->data[4]);
            e->dropped = word_int(m, m->data[5]);
            CHECK(m->data[6] != T_EMPTYBYTES);
            const uint32_t *b = &m->data[term_ref_off(m->data[6])];
            const uint8_t *s = (const uint8_t *)&b[2];
            e->len = (int)b[1];
            e->first = (uint16_t)(s[0] | s[1] << 8);
            e->counts_up = true;
            for (uint32_t i = 1; i < b[1] / 2u; i++) {
                uint16_t v = (uint16_t)(s[2 * i] | s[2 * i + 1] << 8);
                if (v != (uint16_t)(e->first + i)) e->counts_up = false;
            }
            e->last = (uint16_t)(s[b[1] - 2] | s[b[1] - 1] << 8);
        }
        if (ngot < 64) ngot++;
        a->mbox_head = m->next;
        if (!a->mbox_head) a->mbox_tail = NULL;
        a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(&vm, m));
        msg_free(&vm, m);
    }
}

static Port *port_of(term pid) { return port_at(&vm, term_pid_slot(pid) - PORT_SLOT_BASE); }

/* ---- mic ---- */

TEST(audio_open_without_hw_fails) {
    setup();
    fake_no_mic();
    fake_no_spk();
    CHECK(!test_port_is(open_mic(owner, 16000, 100, 0)));
    CHECK(!test_port_is(open_spk(owner, 16000, 60)));
    teardown();
}

TEST(mic_config_rate_and_frame_size) {
    setup();
    CHECK(!test_port_is(open_mic(owner, 0, 100, 0)));         /* Rate 8000 or 16000 only */
    CHECK(!test_port_is(open_mic(owner, 44100, 100, 0)));
    CHECK(!test_port_is(open_mic(owner, 12000, 100, 0)));
    CHECK_EQ(fake_mic_opens(), 0);
    static const struct { int rate, ms; uint32_t bytes; } c[] = {
        { 16000, 0, 3200 },      /* FrameMs 0: 100 */
        { 16000, -7, 640 },      /* negative: clamped up to 20 */
        { 8000, 20, 320 },       /* Rate x FrameMs / 1000 x 2 */
        { 16000, 5, 640 },       /* clamped up to 20 */
        { 16000, 1000, 16000 },  /* clamped down to 500 */
        { 8000, 250, 4000 },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        uint32_t charged = actor_charged_words(owner);
        size_t used = va_used_bytes(&vm);
        term m = open_mic(owner, c[i].rate, c[i].ms, 0);
        CHECK(test_port_is(m));
        CHECK_EQ(fake_mic_rate(), (uint32_t)c[i].rate);
        CHECK_EQ(actor_charged_words(owner), charged + c[i].bytes / 4u);   /* the frame buffer */
        CHECK(va_used_bytes(&vm) >= used + c[i].bytes);
        port_close(&vm, m);
        CHECK_EQ(actor_charged_words(owner), charged);
        CHECK(va_used_bytes(&vm) <= used);
    }
    teardown();
}

TEST(mic_frame_size_is_the_chunk_size) {
    setup();
    CHECK(test_port_is(open_mic(owner, 8000, 20, 0)));
    step(20);
    step(40);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].len, 320);
    CHECK_EQ(got[1].len, 320);
    CHECK_EQ(got[1].first, 160);
    teardown();
}

TEST(mic_gain_clamped_to_6db_steps) {
    setup();
    static const int in[] = { -5, 0, 5, 7, 13, 18, 23, 24, 30, 1000 };
    static const int want[] = { 0, 0, 0, 6, 12, 18, 18, 24, 24, 24 };
    for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
        term m = open_mic(owner, 16000, 100, in[i]);
        CHECK(test_port_is(m));
        CHECK_EQ(fake_mic_gain(), want[i]);
        port_close(&vm, m);
    }
    teardown();
}

TEST(mic_open_failure_leaves_nothing) {
    setup();
    fake_mic_open_fail(true);
    uint32_t charged = actor_charged_words(owner);
    size_t used = va_used_bytes(&vm);
    CHECK(!test_port_is(open_mic(owner, 16000, 100, 0)));
    CHECK_EQ(actor_charged_words(owner), charged);
    CHECK(va_used_bytes(&vm) <= used + 64);
    fake_mic_open_fail(false);
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));     /* not left half open */
    teardown();
}

TEST(mic_frames_in_order) {
    setup();
    term m = open_mic(owner, 16000, 100, 0);
    CHECK(test_port_is(m));
    CHECK(!test_port_is(open_mic(owner, 16000, 100, 0)));     /* one owner */
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 50u);                                     /* polled every min(FrameMs, 50) ms */
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 0);                                    /* half a frame */
    step(100);
    step(150);
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 2);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(got[i].seq, i + 1);
        CHECK_EQ(got[i].time, i * 100);                  /* the frame's start */
        CHECK_EQ(got[i].dropped, 0);
        CHECK_EQ(got[i].len, 3200);
        CHECK_EQ(got[i].first, i * 1600);
        CHECK(got[i].counts_up);
    }
    CHECK(input_mic_driver.can_event(&vm, port_of(m)));
    teardown();
}

/* The scheduler stalls (an SD write) for 300 ms: the
 * hardware buffer (300 ms) holds it all and the next poll catches up, every
 * frame there and in order, also from a hardware that gives one DMA
 * buffer (960 bytes) per read. A longer stall loses samples to the
 * overflow: the next Chunk's Dropped is the whole frames lost (lost bytes /
 * frame bytes, rounded up) and TimeMs moves past them; nothing else goes
 * wrong. */
static void stall_case(uint32_t read_max) {
    setup();
    fake_mic_buffer(9600);                               /* 300 ms at 16 kHz */
    fake_mic_read_max(read_max);
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    for (uint64_t t = 50; t <= 200; t += 50) step(t);
    step(500);                                           /* 300 ms with no poll */
    collect(owner);
    CHECK_EQ(ngot, 5);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(got[i].seq, i + 1);
        CHECK_EQ(got[i].dropped, 0);
        CHECK_EQ(got[i].time, i * 100);
        CHECK_EQ(got[i].first, i * 1600);
        CHECK(got[i].counts_up);
    }
    step(1200);                                          /* 700 ms: 400 ms over the buffer */
    collect(owner);
    CHECK_EQ(ngot, 3);                                   /* the 300 ms the buffer kept */
    CHECK_EQ(got[0].seq, 6);
    CHECK_EQ(got[0].dropped, 4);                         /* 12800 bytes lost: 4 frames */
    CHECK_EQ(got[0].time, 900);                          /* moved past them */
    CHECK_EQ(got[0].first, 14400);
    for (int i = 1; i < ngot; i++) CHECK_EQ(got[i].dropped, 0);
    for (uint64_t t = 1250; t <= 1400; t += 50) step(t);   /* back to normal */
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, 9);
    CHECK_EQ(got[0].dropped, 0);
    CHECK_EQ(got[0].time, 1200);
    CHECK_EQ(got[0].first, 19200);
    CHECK(owner->state != A_EXITED);
    teardown();
}

TEST(mic_stall_300ms_loses_nothing_longer_counts_dropped) { stall_case(UINT32_MAX); }
TEST(mic_stall_with_one_dma_buffer_per_read) { stall_case(960); }

/* A loss that is not a whole number of frames rounds up. */
TEST(mic_lost_bytes_round_up_to_frames) {
    setup();
    fake_mic_buffer(9600);
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    step(50);
    step(400);                                           /* 50 ms over: 1600 bytes lost */
    collect(owner);
    CHECK(ngot >= 1);
    CHECK_EQ(got[0].dropped, 1);
    CHECK_EQ(got[0].time, 100);                          /* frame 0 lost (the nominal one) */
    CHECK_EQ(got[0].first, 1600);                        /* its first sample is at 100 ms */
    CHECK(got[0].counts_up);                             /* no pre-gap samples in it */
    teardown();
}

/* A gap in the middle of a frame that does not end on a frame boundary:
 * the partial frame before it and the samples after it up to the next
 * boundary are discarded, so the next Chunk is contiguous and its TimeMs
 * is its first sample's; also with one DMA buffer per read. */
static void mid_frame_gap_case(uint32_t read_max) {
    setup();
    fake_mic_buffer(9600);
    fake_mic_read_max(read_max);
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    step(50);                                            /* 1600 bytes in the frame */
    step(380);                                           /* 30 ms over: 960 bytes lost */
    for (uint64_t t = 430; t <= 600; t += 50) step(t);
    collect(owner);
    CHECK(ngot >= 2);
    CHECK_EQ(got[0].dropped, 1);                         /* 0..100 ms: partial frame + gap */
    for (int i = 0; i < ngot; i++) {
        CHECK_EQ(got[i].time, 100 + i * 100);
        CHECK_EQ(got[i].first, 1600 + i * 1600);
        CHECK_EQ(got[i].len, 3200);
        CHECK(got[i].counts_up);
        if (i) CHECK_EQ(got[i].dropped, 0);
    }
    teardown();
}

TEST(mic_gap_mid_frame_keeps_chunks_contiguous) { mid_frame_gap_case(UINT32_MAX); }
TEST(mic_gap_mid_frame_one_dma_buffer_per_read) { mid_frame_gap_case(960); }

/* The MCU default limit (4096 words): the defaults (16 kHz, 100 ms) open,
 * and a receiver with a small heap rides out a 300 ms stall (at most 3
 * Chunks wait, 810 words each, beside the 800-word buffer) with nothing
 * dropped. A frame no Chunk of which could fit is refused at open. */
TEST(mic_defaults_fit_the_mcu_limit) {
    setup();
    Actor *a = actor_new(&vm, 256);
    CHECK(a != NULL);
    a->state = A_WAITING;
    a->max_words = 4096;
    CHECK(!test_port_is(open_mic(a, 16000, 500, 0)));         /* 4000 + 4010 words: never deliverable */
    CHECK(!test_port_is(open_mic(a, 16000, 250, 0)));
    CHECK_EQ(fake_mic_opens(), 2);
    CHECK_EQ(fake_mic_closes(), 2);                      /* undone */
    term m = open_mic(a, 16000, 0, 0);
    CHECK(test_port_is(m));
    step(50);
    step(100);
    collect(a);
    CHECK_EQ(ngot, 1);
    uint32_t lost = input_dropped(&vm);
    step(400);                                           /* 300 ms stall, nothing read meanwhile */
    CHECK_EQ(input_dropped(&vm), lost);                     /* 3 Chunks waiting, all delivered */
    for (uint64_t t = 450; t <= 600; t += 50) step(t);   /* the owner still has not read them */
    CHECK_EQ(input_dropped(&vm), lost + 2u);                /* a 4th does not fit: the margin is 3 */
    collect(a);
    CHECK_EQ(ngot, 3);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(got[i].dropped, 0);
        CHECK_EQ(got[i].seq, i + 2);
        CHECK(got[i].counts_up);
    }
    step(650);
    step(700);
    collect(a);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 7);
    CHECK_EQ(got[0].dropped, 2);                         /* over the quota, counted */
    port_close(&vm, m);
    teardown();
}

/* A Chunk over the owner's quota is dropped and counted, never killing it. */
TEST(mic_over_quota_counts_dropped) {
    setup();
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    owner->state = A_RUNNABLE;                           /* no slack to reclaim */
    owner->max_words = (uint16_t)(actor_charged_words(owner) + 1u);   /* no room for a Chunk */
    step(50);
    step(100);
    step(150);
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 0);
    owner->max_words = ROOMY;
    step(250);
    step(300);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 3);
    CHECK_EQ(got[0].dropped, 2);
    CHECK_EQ(got[0].first, 3200);
    CHECK(owner->state != A_EXITED);
    teardown();
}

/* Closing (or the owner exiting) with a frame partly filled discards it:
 * no Chunk after the close, the buffer and the charge are given back, the
 * hardware is stopped, and a new open starts afresh. */
TEST(mic_close_with_partial_frame) {
    setup();
    uint32_t charged = actor_charged_words(owner);
    term m = open_mic(owner, 16000, 100, 0);
    step(50);
    step(100);
    step(150);                                           /* a frame and a half */
    collect(owner);
    CHECK_EQ(ngot, 1);
    size_t used = va_used_bytes(&vm);
    port_close(&vm, m);
    CHECK_EQ(fake_mic_closes(), 1);
    CHECK(!fake_mic_on());
    CHECK_EQ(actor_charged_words(owner), charged);
    CHECK(va_used_bytes(&vm) + 3200u <= used);              /* the frame buffer */
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    step(200);
    step(1000);
    collect(owner);
    CHECK_EQ(ngot, 0);
    m = open_mic(owner, 16000, 100, 0);
    CHECK(test_port_is(m));
    step(1050);
    step(1100);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK_EQ(got[0].first, 0);
    CHECK_EQ(got[0].time, 1000);
    port_close_owned(&vm, owner->id);                    /* the owner exits mid-frame */
    CHECK_EQ(fake_mic_closes(), 2);
    CHECK_EQ(actor_charged_words(owner), charged);
    teardown();
}

TEST(mic_read_error_retried) {
    setup();
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    fake_mic_fail(true);
    step(50);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_mic_fail(false);
    step(150);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].first, 0);
    CHECK_EQ(got[0].dropped, 0);
    teardown();
}

/* After the hardware is dead (the unix script hit EOF), the mic is still
 * polled while reads bring data (what was captured before), then no more:
 * can_event goes false and the VM may exit. */
TEST(mic_deadline_after_eof) {
    setup();
    fake_live_hook(true);
    term m = open_mic(owner, 16000, 100, 0);
    step(50);
    fake_set_live(false);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));                  /* data came: keep polling */
    CHECK(input_mic_driver.can_event(&vm, port_of(m)));
    fake_mic_fail(true);                                 /* nothing more comes */
    step(100);
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(!input_mic_driver.can_event(&vm, port_of(m)));
    fake_set_live(true);
    CHECK(port_next_deadline(&vm, &d));
    teardown();
}

/* ---- half duplex ---- */

TEST(spk_refused_while_mic_open_mic_keeps_running) {
    setup();
    term m = open_mic(owner, 16000, 100, 0);
    CHECK(test_port_is(m));
    step(50);
    CHECK(!test_port_is(open_spk(owner, 16000, 60)));
    CHECK_EQ(fake_spk_opens(), 0);                       /* the hardware is not touched */
    CHECK(!fake_spk_amp());
    CHECK(fake_mic_on());
    CHECK_EQ(fake_mic_closes(), 0);
    step(100);
    step(150);
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 2);                                   /* nothing lost */
    CHECK_EQ(got[1].first, 1600);
    CHECK(got[1].counts_up);
    Actor *b = actor_new(&vm, 256);                      /* nor for another actor */
    CHECK(!test_port_is(open_spk(b, 16000, 60)));
    port_close(&vm, m);
    term s = open_spk(owner, 16000, 60);
    CHECK(test_port_is(s));
    CHECK(!test_port_is(open_mic(owner, 16000, 100, 0)));     /* and the other way round */
    CHECK_EQ(fake_mic_opens(), 1);
    CHECK(fake_spk_amp());
    port_close(&vm, s);
    CHECK(test_port_is(open_mic(owner, 16000, 100, 0)));
    teardown();
}

/* ---- spk ---- */

TEST(spk_config) {
    setup();
    CHECK(!test_port_is(open_spk(owner, 0, 60)));
    CHECK(!test_port_is(open_spk(owner, 22050, 60)));
    static const int in[] = { -3, 0, 60, 100, 150 };
    static const int want[] = { 0, 0, 60, 100, 100 };
    for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
        term s = open_spk(owner, 8000, in[i]);
        CHECK(test_port_is(s));
        CHECK_EQ(fake_spk_rate(), 8000u);
        CHECK_EQ(fake_spk_volume(), want[i]);
        CHECK(!test_port_is(open_spk(owner, 8000, 50)));      /* one at a time */
        port_close(&vm, s);
        CHECK(!fake_spk_amp());
    }
    fake_spk_open_fail(true);
    CHECK(!test_port_is(open_spk(owner, 16000, 60)));
    fake_spk_open_fail(false);
    CHECK(test_port_is(open_spk(owner, 16000, 60)));
    teardown();
}

/* An Out larger than the hardware takes at once is written
 * in parts, across polls while its buffer is full, in order; its Done
 * comes only once it has all been written and played. */
TEST(spk_large_out_written_in_parts_done_after_played) {
    setup();
    fake_spk_accept(1000);                               /* per spk_write */
    fake_spk_size(2500);                                 /* the hardware buffer */
    term s = open_spk(owner, 16000, 60);
    CHECK(test_port_is(s));
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));                 /* idle: no poll */
    CHECK(!input_speaker_driver.can_event(&vm, port_of(s)));
    send_out(owner, s, 6000, 7);
    CHECK(input_speaker_driver.can_event(&vm, port_of(s)));
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 0u);                                     /* started at once */
    step(0);
    CHECK_EQ(fake_spk_written(), 2500u);                 /* 1000 + 1000 + 500, then full */
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 10u);
    step(10);
    CHECK_EQ(fake_spk_written(), 2500u);                 /* nothing played: no room */
    fake_spk_play(2500);
    step(20);
    CHECK_EQ(fake_spk_written(), 5000u);
    fake_spk_play(2500);
    step(30);
    CHECK_EQ(fake_spk_written(), 6000u);                 /* all written, 1000 still to play */
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_spk_play(999);
    step(40);
    collect(owner);
    CHECK_EQ(ngot, 0);                                   /* 1 byte short */
    fake_spk_play(1);
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, -1);
    CHECK_EQ(got[0].seq, 1);
    const uint8_t *w = fake_spk_data();
    bool same = true;
    for (uint32_t i = 0; i < 6000; i++) same = same && w[i] == (uint8_t)(i + 7);
    CHECK(same);
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(!input_speaker_driver.can_event(&vm, port_of(s)));
    teardown();
}

/* Gapless: a hardware playing in real time with a 100 ms
 * buffer, a program keeping two Outs in flight and sending the next on
 * each Done (as gogo/sound's Play does): the queued Out is written as soon
 * as the head is, so the hardware never runs dry between them (no
 * underrun), and each Done comes within one poll of its Out having played. */
TEST(spk_back_to_back_outs_play_without_gap) {
    setup();
    fake_spk_size(3200);                                 /* 100 ms at 16 kHz */
    fake_spk_realtime(true);
    term s = open_spk(owner, 16000, 60);
    send_out(owner, s, 6400, 1);                         /* 200 ms each */
    send_out(owner, s, 6400, 2);
    int32_t done_at[4] = { 0, 0, 0, 0 };
    int ndone = 0;
    for (uint64_t t = 0; t <= 800; t += 10) {
        step(t);
        collect(owner);
        for (int i = 0; i < ngot; i++) {
            CHECK_EQ(got[i].len, -1);
            CHECK_EQ(got[i].seq, ndone + 1);
            CHECK(fake_spk_played() >= 6400u * (uint32_t)(ndone + 1));   /* played before its Done */
            if (ndone < 4) done_at[ndone] = (int32_t)t;
            ndone++;
            if (ndone == 1) send_out(owner, s, 6400, 3);  /* the next on each Done */
        }
    }
    CHECK_EQ(ndone, 3);
    CHECK_EQ(done_at[0], 200);
    CHECK_EQ(done_at[1], 400);
    CHECK_EQ(done_at[2], 600);
    CHECK_EQ(fake_spk_underruns(), 0);
    CHECK_EQ(fake_spk_written(), 19200u);
    bool order = true;
    const uint8_t *w = fake_spk_data();
    for (uint32_t i = 0; i < 19200; i++) order = order && w[i] == (uint8_t)(i % 6400u + 1u + i / 6400u);
    CHECK(order);
    teardown();
}

/* Two in flight are written back to back, a third is refused; an odd (not
 * 16-bit) or empty Out is refused; a write error fails its Out with
 * Done{-1}, in order behind a head still playing; no spk_queued hook means
 * Done once written. */
TEST(spk_queue_refusals_and_write_errors) {
    setup();
    term s = open_spk(owner, 16000, 60);
    send_out(owner, s, 101, 0);                          /* odd: not PCM16 */
    send_out(owner, s, 0, 0);                            /* empty */
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, -1);
    CHECK_EQ(got[1].seq, -1);
    send_out(owner, s, 400, 0);
    send_out(owner, s, 600, 0);
    send_out(owner, s, 800, 0);                          /* third in flight */
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, -1);
    step(0);
    CHECK_EQ(fake_spk_written(), 1000u);                 /* both, back to back */
    collect(owner);
    CHECK_EQ(ngot, 0);
    fake_spk_play(400);
    step(10);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    fake_spk_set_idle();
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 2);
    fake_spk_fail(true);                                 /* the head fails */
    send_out(owner, s, 200, 0);
    send_out(owner, s, 200, 0);
    step(30);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, -1);
    CHECK_EQ(got[1].seq, -1);
    fake_spk_fail(false);
    send_out(owner, s, 200, 0);
    step(40);                                            /* written, playing */
    fake_spk_fail(true);
    send_out(owner, s, 200, 0);                          /* fails behind it */
    step(50);
    collect(owner);
    CHECK_EQ(ngot, 0);                                   /* its Done{-1} waits its turn */
    fake_spk_fail(false);
    fake_spk_set_idle();
    step(60);
    collect(owner);
    CHECK_EQ(ngot, 2);
    CHECK_EQ(got[0].seq, 3);                             /* failures use no Seq */
    CHECK_EQ(got[1].seq, -1);
    port_close(&vm, s);
    fake_no_spk_queued();                                /* optional: played once taken */
    s = open_spk(owner, 16000, 60);
    CHECK(test_port_is(s));
    send_out(owner, s, 400, 0);
    step(70);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    teardown();
}

/* Closing while playing cuts at once: no Done after the
 * close (nor an owed one), the hardware is stopped and the amplifier is
 * off; a new open starts afresh. The owner exiting is the same close. */
TEST(spk_close_while_playing) {
    setup();
    fake_spk_accept(1000);
    term s = open_spk(owner, 16000, 60);
    send_out(owner, s, 4000, 0);
    send_out(owner, s, 2000, 0);
    step(0);
    CHECK(fake_spk_amp());
    CHECK(fake_spk_written() > 0u);
    port_close(&vm, s);
    CHECK_EQ(fake_spk_closes(), 1);
    CHECK(!fake_spk_amp());
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    fake_spk_set_idle();
    step(10);
    step(100);
    collect(owner);
    CHECK_EQ(ngot, 0);
    /* An owed Done (the mailbox was full) is not sent after the close. */
    s = open_spk(owner, 16000, 60);
    CHECK(fake_spk_amp());
    CHECK_EQ(fake_spk_written(), 0u);                    /* afresh */
    send_out(owner, s, 400, 0);
    step(110);
    owner->state = A_RUNNABLE;
    owner->max_words = (uint16_t)(actor_charged_words(owner) + 1u);
    fake_spk_set_idle();
    step(120);
    CHECK(owner->mbox_head == NULL);                     /* owed */
    CHECK(input_speaker_driver.can_event(&vm, port_of(s)));
    port_close(&vm, s);
    owner->max_words = ROOMY;
    step(130);
    step(200);
    collect(owner);
    CHECK_EQ(ngot, 0);
    CHECK(!fake_spk_amp());
    /* The owner exits mid-Out. */
    s = open_spk(owner, 16000, 60);
    send_out(owner, s, 4000, 0);
    step(210);
    port_close_owned(&vm, owner->id);
    CHECK_EQ(fake_spk_closes(), 3);
    CHECK(!fake_spk_amp());
    teardown();
}

/* An owed Done is retried by later polls. */
TEST(spk_owed_done_flushed) {
    setup();
    term s = open_spk(owner, 16000, 60);
    send_out(owner, s, 400, 0);
    step(0);
    owner->state = A_RUNNABLE;
    owner->max_words = (uint16_t)(actor_charged_words(owner) + 1u);
    fake_spk_set_idle();
    step(10);
    CHECK(owner->mbox_head == NULL);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));                  /* retried */
    owner->max_words = ROOMY;
    step(20);
    collect(owner);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    CHECK(!port_next_deadline(&vm, &d));
    (void)s;
    teardown();
}
