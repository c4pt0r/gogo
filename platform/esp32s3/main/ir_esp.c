/* Infrared on the RMT peripheral (spec input2 §3, §4): the IrTxHal and
 * IrRxHal hooks for runtime/input/ir.c. Pins in board.h: StickS3 TX G46, RX G42; Cardputer
 * ADV TX G44 (an LED on the pin through 22 ohms, no receiver). Every hook
 * runs on the scheduler thread; only the RMT callbacks run in an ISR.
 *
 * Transmitter (both boards): ir_tx_open makes an RMT TX channel at 1 MHz
 * (1 tick = 1 us) with one 48-symbol memory block, the copy encoder, the
 * carrier from ir.TX (rmt_apply_carrier; ir_tx_start re-applies it only if
 * it differs) and a 512-byte symbol buffer (IR_FRAME_MAX / 2 symbols,
 * internal RAM); the pin's drive strength is set to the maximum (the LED
 * hangs on it through 22 ohms on the Cardputer). ir_tx_start converts the
 * timings (uint16 LE us, mark first) into that buffer, one symbol per
 * mark/space pair (mark = level 1), before it returns: the encoder reads the
 * buffer while the frame goes out, never the program's Bytes. ir_tx_busy is
 * a flag the done callback clears. ir_tx_close waits up to 100 ms for a
 * running frame (an NEC frame is 68 ms), then disables and frees the
 * channel, the encoder and the buffer.
 *
 * Frames longer than one memory block (about 47 symbols, 94 values) are fed
 * to the hardware by ping-pong refills from the RMT interrupt. That
 * interrupt is not cache-safe (RMT_*_ISR_CACHE_SAFE off, wherever the
 * handler lives), so during a /flash write it is held off and such a frame
 * can be garbled (stale half-blocks replayed); a received long frame can be
 * cut the same way. Frames that fit one block, NEC included, are not
 * affected (their Done may come later).
 *
 * Receiver (HAS_IR_RX, StickS3): RMT RX channel at 1 MHz, input inverted
 * (the receiver module's output is low during a burst, so a mark reads as
 * level 1) with the internal pull-up on (the module's output is open
 * collector), signal_range_min_ns 1000 (shorter pulses are glitches),
 * signal_range_max_ns GapUs * 1000 (ir.c caps GapUs at 32767, the
 * hardware's idle limit at 1 MHz). RMT receive is one-shot, and the hook is
 * only polled every 20 ms, so the receiver keeps going on its own: three
 * frame slots of IR_FRAME_MAX / 2 + 1 symbols (one more than a frame may
 * keep, so a longer frame shows as one; 1548 B, internal RAM, allocated at
 * open, freed at close). The done callback records the slot's symbol count
 * and time and re-arms rmt_receive on the next free slot; ir_rx_read hands
 * out the oldest full slot, frees it, and re-arms if the callback found no
 * free slot (frames that end while all three are full are lost; the
 * hardware cannot hold them). The frame's start time is the done time minus
 * the gap and the frame's length.
 *
 * StickS3 power (pm1.c): the IR LED and receiver hang on EXT_5V (PM1
 * PWR_CFG 0x06 bit 3), off after M5's init. It is reference-counted over
 * the open IR ports: the first open turns it on (if it was off), the last
 * close turns it off again, but only if gogo turned it on. While it is on,
 * the Grove/Hat 5 V pins are outputs: nothing may feed 5 V into them then
 * (M5 docs). While ir_rx is open the speaker amplifier (PM1 GPIO3) is held
 * off, as M5 requires for IR reception (pm1_spk_amp_block): a speaker open
 * then fails, one already open goes silent until ir_rx closes.
 *
 * Sources: docs.m5stack.com StickS3 pin map and ir_nec example, M5Cardputer
 * UserDemo CardputerADV hal_config.h, ESP-IDF v6.1.1 esp_driver_rmt
 * (rmt_rx.c: the done callback runs after the channel is back in the enable
 * state, so it may call rmt_receive). */
#include "platform_esp.h"
#include "board.h"

#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#if HAS_IR_RX
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#endif

#define IR_RES_HZ 1000000u
#define IR_MEM_SYMBOLS 48         /* one RMT memory block: ping-pong refills the rest */

/* EXT_5V on boards that switch it (StickS3), counted over the open IR
 * ports. The count assumes the PM1 keeps PWR_CFG for the whole boot (it
 * does not reset on its own short of a power loss, which resets us too). */
static uint8_t ir_users;
static bool ext5v_ours;                     /* gogo turned it on: turn it off at the end */

static int ir_power_get(void) {
#if HAS_PM1
    if (ir_users == 0) {
        bool was;
        if (pm1_ext5v(true, &was) != 0) return -1;
        ext5v_ours = !was;
    }
#endif
    ir_users++;
    return 0;
}

static void ir_power_put(void) {
    if (ir_users == 0 || --ir_users != 0) return;
#if HAS_PM1
    if (ext5v_ours) pm1_ext5v(false, NULL);
#endif
    ext5v_ours = false;
}

/* ---- transmitter ---- */

static rmt_channel_handle_t tx_chan;
static rmt_encoder_handle_t tx_enc;
static rmt_symbol_word_t *tx_syms;          /* IR_FRAME_MAX / 2 symbols */
static volatile bool tx_busy;
static uint32_t tx_carrier;
static uint8_t tx_duty;

static bool tx_done(rmt_channel_handle_t ch, const rmt_tx_done_event_data_t *e, void *user) {
    (void)ch;
    (void)e;
    (void)user;
    tx_busy = false;
    return false;
}

static void tx_free(void) {
    if (tx_chan) {
        rmt_disable(tx_chan);
        rmt_del_channel(tx_chan);
    }
    if (tx_enc) rmt_del_encoder(tx_enc);
    heap_caps_free(tx_syms);
    tx_chan = NULL;
    tx_enc = NULL;
    tx_syms = NULL;
    tx_busy = false;
}

static int tx_carrier_set(uint32_t carrier_hz, uint8_t duty_pct) {
    rmt_carrier_config_t cc = {.frequency_hz = carrier_hz, .duty_cycle = (float)duty_pct / 100.0f};
    if (rmt_apply_carrier(tx_chan, &cc) != ESP_OK) return -1;
    tx_carrier = carrier_hz;
    tx_duty = duty_pct;
    return 0;
}

int ir_esp_tx_open(void *ctx, uint32_t carrier_hz, uint8_t duty_pct) {
    (void)ctx;
    if (tx_chan || ir_power_get() != 0) return -1;
    tx_syms = heap_caps_malloc(IR_FRAME_MAX / 2 * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rmt_tx_channel_config_t tc = {
        .gpio_num = IR_PIN_TX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RES_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .trans_queue_depth = 1,
    };
    rmt_copy_encoder_config_t ec = {};
    rmt_tx_event_callbacks_t cb = {.on_trans_done = tx_done};
    if (!tx_syms || rmt_new_tx_channel(&tc, &tx_chan) != ESP_OK) {
        tx_chan = NULL;
        goto fail;
    }
    if (rmt_tx_register_event_callbacks(tx_chan, &cb, NULL) != ESP_OK || rmt_new_copy_encoder(&ec, &tx_enc) != ESP_OK) {
        tx_enc = NULL;
        goto fail;
    }
    /* After the channel claimed the pin: the most current the pad gives
     * (GPIO_DRIVE_CAP_3, about 40 mA); the default is CAP_2. */
    if (gpio_set_drive_capability(IR_PIN_TX, GPIO_DRIVE_CAP_3) != ESP_OK ||
        tx_carrier_set(carrier_hz, duty_pct) != 0 || rmt_enable(tx_chan) != ESP_OK)
        goto fail;
    return 0;
fail:
    tx_free();
    ir_power_put();
    platform_esp_log("gogo: ir_tx: RMT setup failed");
    return -1;
}

void ir_esp_tx_close(void *ctx) {
    (void)ctx;
    if (!tx_chan) return;
    if (tx_busy) rmt_tx_wait_all_done(tx_chan, 100);   /* finish a short frame; a longer one is cut */
    tx_free();
    ir_power_put();
}

int ir_esp_tx_start(void *ctx, const uint8_t *timings, uint32_t n, uint32_t carrier_hz, uint8_t duty_pct) {
    (void)ctx;
    if (n == 0 || n > IR_FRAME_MAX || tx_busy || !tx_chan) return -1;
    if ((carrier_hz != tx_carrier || duty_pct != tx_duty) && tx_carrier_set(carrier_hz, duty_pct) != 0) return -1;
    uint32_t ns = (n + 1) / 2;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t k = 2 * i;
        uint16_t mark = (uint16_t)(timings[2 * k] | timings[2 * k + 1] << 8);
        uint16_t space = k + 1 < n ? (uint16_t)(timings[2 * k + 2] | timings[2 * k + 3] << 8) : 0;
        /* An odd count ends on a mark: duration1 0 is the end marker. */
        tx_syms[i] = (rmt_symbol_word_t){.duration0 = mark, .level0 = 1, .duration1 = space, .level1 = 0};
    }
    rmt_transmit_config_t xc = {.loop_count = 0, .flags.eot_level = 0};
    tx_busy = true;
    if (rmt_transmit(tx_chan, tx_enc, tx_syms, ns * sizeof(rmt_symbol_word_t), &xc) != ESP_OK) {
        tx_busy = false;
        return -1;
    }
    return 0;
}

bool ir_esp_tx_busy(void *ctx) { (void)ctx; return tx_busy; }

/* ---- receiver ---- */

#if HAS_IR_RX

#define RX_SLOTS 3
#define RX_SYMS (IR_FRAME_MAX / 2 + 1)

static rmt_channel_handle_t rx_chan;
static rmt_symbol_word_t *rx_mem;           /* RX_SLOTS * RX_SYMS */
static rmt_receive_config_t rx_cfg;
static uint32_t rx_gap_us;
static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
/* Under rx_mux: the full slots are rx_head .. rx_head + rx_count - 1 (mod
 * RX_SLOTS); while rx_armed the receiver fills slot rx_head + rx_count. */
static uint8_t rx_head, rx_count;
static bool rx_armed, rx_stop;
static uint16_t rx_nsym[RX_SLOTS];
static uint32_t rx_done_ms[RX_SLOTS];

/* Starts receiving into the next free slot, if there is one (rx_mux held). */
static void rx_arm_locked(void) {
    if (rx_armed || rx_stop || rx_count >= RX_SLOTS) return;
    uint8_t slot = (uint8_t)((rx_head + rx_count) % RX_SLOTS);
    rx_armed = rmt_receive(rx_chan, rx_mem + slot * RX_SYMS, RX_SYMS * sizeof(rmt_symbol_word_t), &rx_cfg) == ESP_OK;
}

static bool rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *user) {
    (void)ch;
    (void)user;
    portENTER_CRITICAL_ISR(&rx_mux);
    if (rx_armed) {
        uint8_t slot = (uint8_t)((rx_head + rx_count) % RX_SLOTS);
        rx_nsym[slot] = (uint16_t)e->num_symbols;
        rx_done_ms[slot] = (uint32_t)platform_esp_ms();
        rx_count++;
        rx_armed = false;
        rx_arm_locked();
    }
    portEXIT_CRITICAL_ISR(&rx_mux);
    return false;
}

static void rx_free(void) {
    if (rx_chan) {
        rmt_disable(rx_chan);
        rmt_del_channel(rx_chan);
        rx_chan = NULL;
    }
    heap_caps_free(rx_mem);
    rx_mem = NULL;
}

int ir_esp_rx_open(void *ctx, uint32_t gap_us) {
    (void)ctx;
    if (rx_chan || ir_power_get() != 0) return -1;
#if HAS_PM1
    if (pm1_spk_amp_block(true) != 0) {
        pm1_spk_amp_block(false);
        ir_power_put();
        return -1;
    }
#endif
    rx_mem = heap_caps_malloc(RX_SLOTS * RX_SYMS * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rmt_rx_channel_config_t rc = {
        .gpio_num = IR_PIN_RX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RES_HZ,
        .mem_block_symbols = IR_MEM_SYMBOLS,
        .flags.invert_in = 1,
    };
    rmt_rx_event_callbacks_t cb = {.on_recv_done = rx_done};
    if (!rx_mem || rmt_new_rx_channel(&rc, &rx_chan) != ESP_OK) {
        rx_chan = NULL;
        goto fail;
    }
    /* The receiver module's output is open-collector: without a pull-up G42
     * floats low (measured on a StickS3: 0 without, 1 with the internal
     * pull-up, 5 V rail at 5.05 V) and nothing is received. The RMT driver
     * does not pull it up, so do it here. */
    if (gpio_pullup_en(IR_PIN_RX) != ESP_OK) goto fail;
    if (rmt_rx_register_event_callbacks(rx_chan, &cb, NULL) != ESP_OK || rmt_enable(rx_chan) != ESP_OK) goto fail;
    if (gap_us > 32767) gap_us = 32767;
    rx_gap_us = gap_us;
    rx_cfg = (rmt_receive_config_t){.signal_range_min_ns = 1000, .signal_range_max_ns = gap_us * 1000u};
    portENTER_CRITICAL(&rx_mux);
    rx_head = rx_count = 0;
    rx_armed = rx_stop = false;
    rx_arm_locked();
    bool ok = rx_armed;
    portEXIT_CRITICAL(&rx_mux);
    if (ok) return 0;
fail:
    rx_free();
#if HAS_PM1
    pm1_spk_amp_block(false);
#endif
    ir_power_put();
    platform_esp_log("gogo: ir_rx: RMT setup failed");
    return -1;
}

void ir_esp_rx_close(void *ctx) {
    (void)ctx;
    portENTER_CRITICAL(&rx_mux);
    rx_stop = true;
    portEXIT_CRITICAL(&rx_mux);
    rx_free();
#if HAS_PM1
    pm1_spk_amp_block(false);
#endif
    ir_power_put();
}

/* Slot s as timings into buf (at most cap): marks and spaces alternate,
 * mark first, the trailing gap (duration 0) ends it. Returns the value
 * count, which may exceed cap; *us is the frame's length. */
static uint32_t rx_convert(uint8_t s, uint16_t *buf, uint32_t cap, uint32_t *us) {
    const rmt_symbol_word_t *sym = rx_mem + s * RX_SYMS;
    uint32_t n = 0, total = 0;
    int last = -1;                                  /* level of value n - 1 */
    for (uint32_t i = 0; i < rx_nsym[s]; i++) {
        uint32_t d[2] = {sym[i].duration0, sym[i].duration1};
        int l[2] = {sym[i].level0, sym[i].level1};
        for (int h = 0; h < 2; h++) {
            if (d[h] == 0) goto done;
            if (n == 0 && l[h] == 0) continue;      /* a space before the first mark */
            total += d[h];
            if (l[h] == last) {                     /* same level split in two: one value */
                if (n <= cap) {
                    uint32_t v = (uint32_t)buf[n - 1] + d[h];
                    buf[n - 1] = (uint16_t)(v > 32767 ? 32767 : v);
                }
                continue;
            }
            if (n < cap) buf[n] = (uint16_t)d[h];
            n++;
            last = l[h];
        }
    }
done:
    *us = total;
    return n;
}

int ir_esp_rx_read(void *ctx, uint16_t *buf, uint32_t cap, uint32_t *start_ms) {
    (void)ctx;
    if (!rx_chan) return -1;
    for (;;) {
        portENTER_CRITICAL(&rx_mux);
        uint8_t count = rx_count, slot = rx_head;
        if (!count) rx_arm_locked();                /* an earlier re-arm failed: try again */
        portEXIT_CRITICAL(&rx_mux);
        if (!count) return 0;
        uint32_t us;
        uint32_t n = rx_convert(slot, buf, cap, &us);   /* the slot is ours until it is released */
        uint32_t done_ms = rx_done_ms[slot];
        portENTER_CRITICAL(&rx_mux);
        rx_head = (uint8_t)((rx_head + 1) % RX_SLOTS);
        rx_count--;
        rx_arm_locked();
        portEXIT_CRITICAL(&rx_mux);
        if (n == 0) continue;                       /* noise: no mark */
        *start_ms = done_ms - (us + rx_gap_us) / 1000u;
        return (int)n;
    }
}

#endif /* HAS_IR_RX */
