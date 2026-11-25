#include "fakehw.h"
#include "platform.h"
#include <string.h>

static const KbdKey keys[R_COUNT] = {
    [R_A] = { 0x04, 0 },  [R_SEMI] = { 0x33, 0x52 }, [R_1] = { 0x1E, 0x3A }, [R_C] = { 0x06, 0 },
    [R_SHIFT] = { 0xE1, 0 }, [R_CTRL] = { 0xE0, 0 }, [R_FN] = { 0xF0, 0 }, [R_OPT] = { 0xE3, 0 },
    [R_B] = { 0x05, 0 }, [R_D] = { 0x07, 0 }, [R_E] = { 0x08, 0 }, [R_F] = { 0x09, 0 },
    [R_G] = { 0x0A, 0 }, [R_H] = { 0x0B, 0 }, [R_I] = { 0x0C, 0 }, [R_J] = { 0x0D, 0 },
};

static struct { uint16_t raw; bool down; } q[64];
static unsigned qh, qt;
static int opens, closes;
static bool fail_read, flag;      /* flag: the ISR flag, an edge: set by an event, cleared by an empty read */
static int fail_after = -1, kreads;
static bool fail_all;

static int kopen(void) { opens++; qh = qt = 0; flag = false; return 0; }   /* flushes the FIFO */
static void kclose(void) { closes++; }
static bool kpending(void) { return flag; }
static int kread(uint16_t *raw, bool *down) {
    kreads++;
    if (fail_all) return -1;
    if (fail_read || fail_after == 0) { fail_read = false; fail_after = -1; return -1; }
    if (fail_after > 0) fail_after--;
    if (qh == qt) { flag = false; return 0; }
    *raw = q[qh % 64].raw;
    *down = q[qh % 64].down;
    qh++;
    return 1;
}

static int32_t p_mv = 3700;
static int8_t p_ch, p_ext;
static bool p_fail, p_flag;
static int p_reads;

static int pread(int32_t *mv, int8_t *ch, int8_t *ext) {
    p_reads++;
    if (p_fail) return -1;
    p_flag = false;
    *mv = p_mv; *ch = p_ch; *ext = p_ext;
    return 0;
}
static bool ppending(void) { return p_flag; }

static struct { uint8_t b[12 * 512]; unsigned h, t; } imu_q;     /* byte ring: h and t count bytes */
static int imu_opens, imu_closes, imu_rate, imu_mask, imu_freads, imu_greads;
static bool imu_fifo_fail, imu_open_fail;
static ImuGestures imu_g;

static int iopen(uint16_t rate, uint8_t mask) {
    if (imu_open_fail) return -1;
    imu_opens++; imu_rate = rate; imu_mask = mask;
    imu_q.h = imu_q.t = 0;
    return 0;
}
static void iclose(void) { imu_closes++; }
static int iread(uint8_t *buf, uint32_t cap) {
    imu_freads++;
    if (imu_fifo_fail) return -1;
    uint32_t n = imu_q.t - imu_q.h;
    if (n > cap) n = cap - cap % 12;
    for (uint32_t i = 0; i < n; i++) buf[i] = imu_q.b[(imu_q.h + i) % sizeof imu_q.b];
    imu_q.h += n;
    return (int)n;
}
static int igest(ImuGestures *g) { imu_greads++; *g = imu_g; imu_g.bits = 0; return 0; }

static struct {
    int starts, fails, n, duty, rx_opens, rx_closes, rx_reads;
    uint16_t v[300];
    uint32_t carrier, gap;
    uint32_t start;
    bool start_set;
    bool busy, fail, rx_fail, rx_open_fail, while_busy, tx_open_fail;
    char log[64];              /* O tx_open, S tx_start, C tx_close */
    int nlog;
    uint32_t open_carrier;
    int open_duty;
    uint16_t q[4][400];
    int qn[4];
    unsigned qh, qt;
} fir;

static void irlog(char c) { if (fir.nlog < 63) fir.log[fir.nlog++] = c; }
static int irtopen(uint32_t carrier, uint8_t duty) {
    if (fir.tx_open_fail) return -1;
    irlog('O');
    fir.open_carrier = carrier;
    fir.open_duty = duty;
    return 0;
}
static void irtclose(void) { irlog('C'); }
static int irstart(const uint8_t *t, uint32_t n, uint32_t carrier, uint8_t duty) {
    if (fir.busy) fir.while_busy = true;
    if (fir.fail) { fir.fails++; return -1; }
    irlog('S');
    fir.starts++;
    fir.n = (int)n;
    for (uint32_t i = 0; i < n && i < 300; i++) fir.v[i] = (uint16_t)(t[2 * i] | t[2 * i + 1] << 8);
    fir.carrier = carrier;
    fir.duty = duty;
    return 0;
}
static bool irbusy(void) { return fir.busy; }
static int irropen(uint32_t gap) {
    if (fir.rx_open_fail) return -1;
    fir.rx_opens++;
    fir.gap = gap;
    fir.qh = fir.qt = 0;
    return 0;
}
static int irread(uint16_t *buf, uint32_t cap, uint32_t *start_ms) {
    fir.rx_reads++;
    if (fir.rx_fail) return -1;
    if (fir.qh == fir.qt) return 0;
    int n = fir.qn[fir.qh % 4];
    if (fir.start_set) *start_ms = fir.start;
    for (int i = 0; i < n && (uint32_t)i < cap; i++) buf[i] = fir.q[fir.qh % 4][i];
    fir.qh++;
    return n;
}
static void irrclose(void) { fir.rx_closes++; }

static struct {
    uint32_t buffer, rate, gain, read_max;
    uint64_t t0, taken;        /* samples taken by mic_read since the open */
    bool on, fail, open_fail;
    int opens, closes, reads;
} fm;

static int mopen(uint32_t rate, uint8_t gain) {
    if (fm.open_fail) return -1;
    fm.opens++; fm.on = true; fm.rate = rate; fm.gain = gain;
    fm.t0 = platform_now_ms(); fm.taken = 0;
    return 0;
}
static int mread(uint8_t *buf, uint32_t cap, uint32_t *lost) {
    fm.reads++;
    if (fm.fail) return -1;
    uint64_t made = (platform_now_ms() - fm.t0) * fm.rate / 1000u, room = fm.buffer / 2u;
    if (made - fm.taken > room) { *lost = (uint32_t)((made - fm.taken - room) * 2u); fm.taken = made - room; }
    uint64_t n = made - fm.taken;
    if (n > cap / 2u) n = cap / 2u;
    if (n > fm.read_max / 2u) n = fm.read_max / 2u;
    for (uint64_t i = 0; i < n; i++) {
        uint16_t v = (uint16_t)(fm.taken + i);
        buf[2 * i] = (uint8_t)(v & 0xFF);
        buf[2 * i + 1] = (uint8_t)(v >> 8);
    }
    fm.taken += n;
    return (int)(n * 2u);
}
static void mclose(void) { fm.closes++; fm.on = false; }

/* The speaker: written - played bytes are queued in a buffer of `size`;
 * played moves by fake_spk_play / fake_spk_set_idle, or in real time on
 * the platform clock (fake_spk_realtime), counting underruns: the buffer
 * ran dry and more came later. */
static struct {
    uint32_t accept, size, rate;
    uint64_t written, played, last_ms;
    int volume, opens, closes, writes, underruns;
    bool amp, fail, open_fail, realtime, dry;
    uint8_t data[65536];
} fs;

static void sadvance(void) {
    uint64_t t = platform_now_ms();
    if (fs.realtime && fs.amp) {
        uint64_t can = (t - fs.last_ms) * fs.rate * 2u / 1000u, q = fs.written - fs.played;
        if (can >= q && q) fs.dry = true;      /* ran dry by this moment */
        fs.played += can < q ? can : q;
    }
    fs.last_ms = t;
}
static int sopen(uint32_t rate, uint8_t volume) {
    if (fs.open_fail) return -1;
    fs.opens++; fs.amp = true; fs.rate = rate; fs.volume = volume;
    fs.written = fs.played = 0; fs.dry = false; fs.last_ms = platform_now_ms();
    return 0;
}
static int swrite(const uint8_t *d, uint32_t len) {
    fs.writes++;
    sadvance();
    if (fs.fail) return -1;
    uint64_t left = fs.size - (fs.written - fs.played);
    uint32_t n = len < fs.accept ? len : fs.accept;
    if (n > left) n = (uint32_t)left;
    if (n && fs.dry) { fs.underruns++; fs.dry = false; }
    for (uint32_t i = 0; i < n; i++) if (fs.written + i < sizeof fs.data) fs.data[fs.written + i] = d[i];
    fs.written += n;
    return (int)n;
}
static uint32_t squeued(void) { sadvance(); return (uint32_t)(fs.written - fs.played); }
static void sclose(void) { fs.closes++; fs.amp = false; }

static InputHw hw;

void fake_reset(void) {
    qh = qt = 0; opens = closes = 0; fail_read = flag = false; fail_after = -1; fail_all = false; kreads = 0;
    hw = (InputHw){ keys, R_COUNT, kopen, kclose, kpending, kread, pread, ppending, 0, 0, false, iopen, iclose, iread, igest,
                    irstart, irbusy, irropen, irread, irrclose, irtopen, irtclose,
                    mopen, mread, mclose, sopen, swrite, squeued, sclose };
    memset(&fm, 0, sizeof fm);
    fm.buffer = 9600;
    fm.read_max = UINT32_MAX;
    memset(&fs, 0, sizeof fs);
    fs.accept = fs.size = UINT32_MAX;
    memset(&fir, 0, sizeof fir);
    imu_q.h = imu_q.t = 0; imu_opens = imu_closes = imu_freads = imu_greads = 0; imu_rate = imu_mask = 0;
    imu_fifo_fail = imu_open_fail = false; imu_g = (ImuGestures){ 0, 0, 0 };
    p_mv = 3700; p_ch = p_ext = 0; p_fail = p_flag = false; p_reads = 0;
}
void fake_no_keyboard(void) { hw.keys = 0; hw.nkeys = 0; }
void fake_push(uint16_t raw, bool down) { q[qt % 64].raw = raw; q[qt % 64].down = down; qt++; flag = true; }
void fake_fail_next_read(void) { fail_read = true; }
void fake_fail_all_reads(bool on) { fail_all = on; }
int fake_kbd_reads(void) { return kreads; }
void fake_fail_read_after(int n) { fail_after = n; }
int fake_opens(void) { return opens; }
int fake_closes(void) { return closes; }

const InputHw *platform_input(void) { return &hw; }

void fake_install_once(void) {
    static bool installed;
    if (!installed) { input_install(); installed = true; }   /* no duplicate check in port_add_source */
}
void fake_no_power(void) { hw.power_read = 0; hw.power_pending = 0; }
void fake_power_set(int32_t mv, int8_t charging, int8_t external) { p_mv = mv; p_ch = charging; p_ext = external; }
void fake_power_fail(bool fail) { p_fail = fail; }
void fake_power_new(void) { p_flag = true; }
int fake_power_reads(void) { return p_reads; }

static bool is_live = true;
static int p_enabled;
static bool live_cb(void) { return is_live; }
static void enable_cb(bool on) { p_enabled += on ? 1 : -1; }
void fake_live_hook(bool live) { hw.live = live_cb; hw.power_enable = enable_cb; is_live = live; p_enabled = 0; }
void fake_set_live(bool live) { is_live = live; }
int fake_power_enabled(void) { return p_enabled; }
void fake_event_driven(bool on) { hw.power_event_driven = on; }

void fake_no_imu(void) { hw.imu_open = 0; hw.imu_close = 0; hw.imu_read_fifo = 0; hw.imu_read_gestures = 0; }
void fake_imu_push(int n, int16_t first) {
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 6; k++) {
            int16_t v = (int16_t)(first + i);
            imu_q.b[imu_q.t++ % sizeof imu_q.b] = (uint8_t)(v & 0xFF);
            imu_q.b[imu_q.t++ % sizeof imu_q.b] = (uint8_t)((uint16_t)v >> 8);
        }
    }
}
int fake_imu_fifo_len(void) { return (int)((imu_q.t - imu_q.h) / 12); }
int fake_imu_fifo_reads(void) { return imu_freads; }
void fake_imu_fifo_fail(bool on) { imu_fifo_fail = on; }
void fake_imu_gesture(uint8_t bits, uint8_t orient, uint16_t steps) { imu_g = (ImuGestures){ bits, orient, steps }; }
int fake_imu_gesture_reads(void) { return imu_greads; }
void fake_imu_open_fail(bool on) { imu_open_fail = on; }
int fake_imu_opens(void) { return imu_opens; }
int fake_imu_closes(void) { return imu_closes; }
int fake_imu_rate(void) { return imu_rate; }
int fake_imu_mask(void) { return imu_mask; }
void fake_imu_no_fifo(void) { hw.imu_read_fifo = 0; }

void fake_no_ir(void) {
    hw.ir_tx_start = 0; hw.ir_tx_busy = 0; hw.ir_rx_open = 0; hw.ir_rx_read = 0; hw.ir_rx_close = 0;
    hw.ir_tx_open = 0; hw.ir_tx_close = 0;
}
void fake_no_ir_tx_open_close(void) { hw.ir_tx_open = 0; hw.ir_tx_close = 0; }
void fake_ir_tx_open_fail(bool on) { fir.tx_open_fail = on; }
const char *fake_ir_tx_log(void) { fir.log[fir.nlog] = 0; return fir.log; }
uint32_t fake_ir_tx_open_carrier(void) { return fir.open_carrier; }
int fake_ir_tx_open_duty(void) { return fir.open_duty; }
void fake_no_ir_rx_close(void) { hw.ir_rx_close = 0; }
int fake_ir_tx_starts(void) { return fir.starts; }
int fake_ir_tx_n(void) { return fir.n; }
uint16_t fake_ir_tx_value(int i) { return fir.v[i]; }
uint32_t fake_ir_tx_carrier(void) { return fir.carrier; }
int fake_ir_tx_duty(void) { return fir.duty; }
void fake_ir_tx_set_busy(bool on) { fir.busy = on; }
void fake_ir_tx_fail(bool on) { fir.fail = on; }
bool fake_ir_tx_started_while_busy(void) { return fir.while_busy; }
void fake_ir_rx_push(const uint16_t *v, int n) {
    unsigned k = fir.qt++ % 4;
    for (int i = 0; i < n && i < 400; i++) fir.q[k][i] = v[i];
    fir.qn[k] = n;
}
void fake_ir_rx_start(uint32_t ms) { fir.start = ms; fir.start_set = true; }
void fake_ir_rx_fail(bool on) { fir.rx_fail = on; }
void fake_ir_rx_open_fail(bool on) { fir.rx_open_fail = on; }
int fake_ir_rx_opens(void) { return fir.rx_opens; }
int fake_ir_rx_closes(void) { return fir.rx_closes; }
int fake_ir_rx_reads(void) { return fir.rx_reads; }
uint32_t fake_ir_rx_gap(void) { return fir.gap; }
int fake_ir_tx_fails(void) { return fir.fails; }

void fake_no_mic(void) { hw.mic_open = 0; hw.mic_read = 0; hw.mic_close = 0; }
void fake_no_spk(void) { hw.spk_open = 0; hw.spk_write = 0; hw.spk_queued = 0; hw.spk_close = 0; }
void fake_no_spk_queued(void) { hw.spk_queued = 0; }
void fake_mic_buffer(uint32_t bytes) { fm.buffer = bytes; }
void fake_mic_read_max(uint32_t bytes) { fm.read_max = bytes; }
void fake_mic_fail(bool on) { fm.fail = on; }
void fake_mic_open_fail(bool on) { fm.open_fail = on; }
int fake_mic_opens(void) { return fm.opens; }
int fake_mic_closes(void) { return fm.closes; }
int fake_mic_reads(void) { return fm.reads; }
uint32_t fake_mic_rate(void) { return fm.rate; }
int fake_mic_gain(void) { return (int)fm.gain; }
bool fake_mic_on(void) { return fm.on; }
void fake_spk_accept(uint32_t max_per_write) { fs.accept = max_per_write; }
void fake_spk_size(uint32_t bytes) { fs.size = bytes; }
void fake_spk_play(uint32_t bytes) {
    uint64_t q = fs.written - fs.played;
    fs.played += bytes < q ? bytes : q;
}
void fake_spk_set_idle(void) { fs.played = fs.written; }
void fake_spk_realtime(bool on) { fs.realtime = on; fs.last_ms = platform_now_ms(); }
int fake_spk_underruns(void) { return fs.underruns; }
uint32_t fake_spk_played(void) { sadvance(); return (uint32_t)fs.played; }
void fake_spk_fail(bool on) { fs.fail = on; }
void fake_spk_open_fail(bool on) { fs.open_fail = on; }
int fake_spk_opens(void) { return fs.opens; }
int fake_spk_closes(void) { return fs.closes; }
int fake_spk_writes(void) { return fs.writes; }
uint32_t fake_spk_rate(void) { return fs.rate; }
int fake_spk_volume(void) { return fs.volume; }
bool fake_spk_amp(void) { return fs.amp; }
uint32_t fake_spk_written(void) { return (uint32_t)fs.written; }
const uint8_t *fake_spk_data(void) { return fs.data; }
