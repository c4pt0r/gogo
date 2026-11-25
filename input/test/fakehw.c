#include "fakehw.h"

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

static InputHw hw;

void fake_reset(void) {
    qh = qt = 0; opens = closes = 0; fail_read = flag = false; fail_after = -1; fail_all = false; kreads = 0;
    hw = (InputHw){ keys, R_COUNT, kopen, kclose, kpending, kread, pread, ppending, 0, 0, false };
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
