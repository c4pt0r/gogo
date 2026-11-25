/* Audio on the ES8311 codec over I2S (spec audio §3): the InputHw mic_* and
 * spk_* hooks for runtime/input/audio.c, on both boards (pins and amplifier in
 * board.h). Built with CONFIG_GOGO_AUDIO only. Every hook runs on the
 * scheduler thread; only the I2S DMA callbacks run in an ISR.
 *
 * Half duplex (runtime/input/ never opens both): one I2S channel at a time on
 * I2S_NUM_0, simplex, master, created by the open (RX for mic, TX for spk)
 * and deleted by the close, so its DMA buffers (internal RAM, the IDF's
 * DMA-capable allocation) and our staging buffer or ring exist only while a
 * port is open. Every open restarts the codec (reg 0x00 = 0x80) and writes
 * every register its direction uses, the other direction's blocks off, as
 * M5Unified does on each enable, so switching always starts clean.
 *
 * Clocking: the codec runs from BCLK (reg 0x01 bit 7), never from MCLK,
 * like M5Unified on both boards. The IDF sets the I2S clock (std mode,
 * Philips, 16-bit data, mono). At 16 kHz the slots are 16 bits (BCLK 32 fs
 * = 512 kHz) and the codec multiplies by 8 (reg 0x02 = 0x18) to 256 fs; at
 * 8 kHz the slots are 32 bits (BCLK 64 fs = 512 kHz again) and it
 * multiplies by 4 (0x10), as esp_codec_dev does (es8311.c:445-453: its
 * multiplier needs BCLK >= 512 kHz). The dividers (0x03-0x08) are the
 * 256 fs row of the esp_codec_dev table, and the serial port format
 * (0x09 in, 0x0A out) is written as 0x0C (I2S, 16-bit) rather than left at
 * the reset values M5Unified relies on.
 *
 * Power: the analog reference (reg 0x0D) is turned on by the first open and
 * stays on until a reset: powered down at a close, the next open captured
 * about a second of exact zeros (StickS3 and Cardputer ADV; the ES8311
 * warm-up M5Unified's Mic_Class.hpp:112-115 describes), so now only the
 * first mic open after power-up has it. A close turns off only the blocks
 * of its direction (PGA and ADC modulator, or the DAC).
 *
 * The I2S ISR is IRAM-safe (CONFIG_I2S_ISR_IRAM_SAFE, selected by
 * GOGO_AUDIO; the callbacks below are IRAM_ATTR): without it the interrupt
 * is held off while the cache is off (a /flash write) and the GDMA reports
 * only the last of the descriptors that finished meanwhile, so the others
 * would be lost on RX and replayed stale on TX. This covers the cache-off
 * case only: any other hold-off of the I2S interrupt longer than one buffer
 * (30 ms RX, 10 ms TX: a long critical section, a higher-priority ISR)
 * still loses RX buffers uncounted in lost and replays stale TX buffers.
 *
 *  - mic: RX_DESC (11) DMA buffers of 30 ms (960 B at 16 kHz), so 10 wait
 *    in the driver's queue (its depth is desc_num - 1): 300 ms of sound for
 *    a scheduler stall. mic_read takes whole DMA buffers (one per call,
 *    through a one-buffer staging copy, since the IDF drops the rest of a
 *    half-read buffer when its queue is nearly full) and hands them out in
 *    pieces; the first two buffers after the open are discarded (start-up
 *    garbage, as M5Unified's capture task does). A buffer the full queue
 *    pushed out is counted in on_recv_q_ovf and reported as lost. The codec
 *    arms its capture path only through a reg 0x0D write made while BCLK
 *    runs (M5Unified M5Unified.inl:555-560, seen on its StopWatch): the
 *    first mic_read 30 ms or more after the open writes it again. The
 *    high-pass filter is the StopWatch's (reg 0x1C = 0x64, with 0x1B =
 *    0x0A written too): with 0x6A the DC step after an open took about
 *    600 ms to die away, with 0x64 one buffer.
 *    Gain (dB above the platform's normal level, input.h): Gain 0 is the analog PGA at 30 dB (reg 0x14) plus the ADC scale
 *    at 24 dB (reg 0x16 = 4), 54 dB in all, about M5Unified's StickS3 level
 *    (PGA 0 dB, digital +32 dB, then x16 in software); Gain 6 .. 24 adds
 *    digital volume (reg 0x17, 0.5 dB steps from 0xBF = 0 dB). Measured at
 *    16 kHz: a quiet room is RMS about 10, the other board's 1 kHz tone at
 *    full volume next to it about 400 to 550.
 *  - spk: TX_DESC (4) DMA buffers of 10 ms cycle on their own; the on_sent
 *    callback refills the one that just played from a SPK_RING (8 KB)
 *    software ring (the IDF clears it first: auto_clear_before_cb, so a
 *    short fill ends in silence) and counts the bytes of the one that
 *    played: spk_queued() = bytes taken - bytes played, exact. spk_write
 *    copies into the ring and never blocks. The buffers are short because a
 *    fill happens 3 buffers ahead of the sound: with runtime/input/audio.c's two
 *    Outs in flight, Done when played, the next Out must reach the ring
 *    before the DMA wants it; with 30 ms buffers the ring ran dry at each
 *    Out boundary (6 s of 4000-byte Outs played in 7.5 s), with 10 ms none
 *    did (6.1 s). The latency from a write to the sound is at most 40 ms;
 *    the ring (256 ms at 16 kHz) is the stall the speaker rides out. The
 *    IDF's own write path is not used: it hands a half-filled buffer to the
 *    DMA and writes the rest after it played, out of order.
 *    Volume 0..100 is the DAC volume (reg 0x32) from 40 dB below the
 *    board's ceiling (1) to the ceiling (100), in 0.4 dB steps; 0 is the
 *    codec's mute (0x00). The ceiling is AUDIO_SPK_MAX_DB (board.h): 0 dB
 *    on the Cardputer ADV, -4 dB on the StickS3.
 *
 * Amplifier and order (no pop): StickS3: codec rail on (PM1 L3B, a 10 ms
 * wait if it was off), codec programmed, I2S running (silence), then the
 * amplifier on (PM1 GPIO3); close: DAC muted, amplifier off, I2S stopped,
 * DAC off. While the IR receiver is open the amplifier is blocked (pm1.c),
 * so spk_open fails then. Cardputer ADV: the NS4150B has no enable pin we
 * know of (M5Unified's ADV speaker callback drives none, and its speaker
 * works): taken to be on with the board; the codec's mute and DAC
 * power-down silence it.
 *
 * Sources: docs/superpowers/specs/2026-11-25-audio-hw-research.md
 * (M5Unified 96576a1, esp-adf esp_codec_dev 20c1047, ESP-IDF v6.1.1). */
#include "platform_esp.h"
#include "board.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_I2S_ISR_IRAM_SAFE
#error "GOGO_AUDIO needs CONFIG_I2S_ISR_IRAM_SAFE (selected by it in Kconfig.projbuild)"
#endif

#define ES_ADDR 0x18
#define ES_HZ 100000u
#define ES_TIMEOUT_MS 20
#define RX_BUF_MS 30
#define TX_BUF_MS 10
#define RX_DESC 11
#define TX_DESC 4
#define SPK_RING 8192u          /* a power of two */
#define MIC_PGA 0x0A            /* reg 0x14 PGA gain: 30 dB (esp_codec_dev's default) */
#define MIC_SCALE 4             /* reg 0x16 ADC scale: 24 dB */
#define MIC_DISCARD 2
#define MIC_REARM_MS 30

static i2c_master_dev_handle_t es;
static i2s_chan_handle_t ch;            /* the open channel; NULL = closed */
static uint32_t buf_bytes;              /* one DMA buffer */

/* ---- codec ---- */

static int es_attach(void) {
    if (es) return 0;
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return -1;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES_ADDR,
        .scl_speed_hz = ES_HZ,
    };
    return i2c_master_bus_add_device(bus, &dc, &es) == ESP_OK ? 0 : (es = NULL, -1);
}

/* Writes reg = v, retried up to 3 times like M5Unified's bulk writes. */
static int es_wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit(es, b, 2, ES_TIMEOUT_MS) == ESP_OK) return 0;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -1;
}

/* (reg, value) pairs. */
static int es_seq(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2)
        if (es_wr(p[i], p[i + 1]) != 0) return -1;
    return 0;
}

/* Restarts the codec's state machine and programs the clocks and the
 * serial port for rate; clocks = reg 0x01 (0xB5 DAC, 0xBA ADC: BCLK as the
 * clock source). Like M5Unified, no register reset (reg 0x00 bits 4:0): the
 * analog reference stays up between opens (see the close functions). */
static int es_start(uint32_t rate, uint8_t clocks) {
    uint8_t id;
    uint8_t r = 0xFD;                   /* CHIP ID1: 0x83 */
    if (es_attach() != 0 || i2c_master_transmit_receive(es, &r, 1, &id, 1, ES_TIMEOUT_MS) != ESP_OK) {
        platform_esp_log("gogo: audio: no ES8311 at 0x%02x", ES_ADDR);
        return -1;
    }
    const uint8_t seq[] = {
        0x00, 0x80,                     /* CSM on, slave */
        0x01, clocks,
        0x02, rate == 8000 ? 0x10 : 0x18,   /* pre-divider 1, x4 / x8: 256 fs */
        0x03, 0x10, 0x04, 0x10,         /* single speed, ADC / DAC OSR */
        0x05, 0x00,                     /* ADC, DAC clock dividers 1 */
        0x06, 0x03,                     /* BCLK divider (master only) */
        0x07, 0x00, 0x08, 0xFF,         /* LRCK divider 256 */
        0x09, 0x0C, 0x0A, 0x0C,         /* serial in / out: I2S, 16-bit, unmuted */
    };
    return es_seq(seq, sizeof seq);
}

/* ---- I2S ---- */

static void ch_free(void) {
    if (!ch) return;
    i2s_channel_disable(ch);
    i2s_del_channel(ch);
    ch = NULL;
}

/* Makes the channel (tx or rx) for rate with ndesc buffers and its
 * callbacks, not enabled yet. */
static int ch_make(bool tx, uint32_t rate, uint32_t ndesc, const i2s_event_callbacks_t *cbs) {
    uint32_t frames = rate * (tx ? TX_BUF_MS : RX_BUF_MS) / 1000u;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = ndesc;
    cc.dma_frame_num = frames;
    cc.auto_clear_before_cb = tx;
    if (i2s_new_channel(&cc, tx ? &ch : NULL, tx ? NULL : &ch) != ESP_OK) {
        ch = NULL;
        return -1;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = tx ? AUDIO_PIN_DOUT : I2S_GPIO_UNUSED,
            .din = tx ? I2S_GPIO_UNUSED : AUDIO_PIN_DIN,
        },
    };
    if (rate == 8000) {                 /* BCLK 512 kHz for the codec's multiplier */
        sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        sc.slot_cfg.ws_width = 32;
    }
    /* TX: the sample on both slots; RX: the right slot, as M5Unified reads
     * the ES8311 (Mic_Class.hpp input_only_right). */
    sc.slot_cfg.slot_mask = tx ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_RIGHT;
    if (i2s_channel_init_std_mode(ch, &sc) != ESP_OK || i2s_channel_register_event_callback(ch, cbs, NULL) != ESP_OK) {
        ch_free();
        return -1;
    }
    buf_bytes = frames * 2u;
    return 0;
}

#if HAS_PM1
static int codec_power(void) { return pm1_audio_power_on(); }
#else
static int codec_power(void) { return 0; }
#endif

/* ---- mic ---- */

static uint8_t *stage;                  /* one DMA buffer */
static uint32_t stage_pos, stage_len;
static uint8_t discard;
static bool rearmed;
static uint64_t mic_t0;
static volatile uint32_t rx_lost;       /* bytes: written by the ISR only */
static uint32_t rx_lost_seen;

static IRAM_ATTR bool rx_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *user) {
    (void)h;
    (void)user;
    rx_lost += (uint32_t)e->size;
    return false;
}

static void mic_stop(void) {
    ch_free();
    heap_caps_free(stage);
    stage = NULL;
    static const uint8_t off[] = {0x0E, 0x6A};   /* PGA, ADC modulator off; reference on (Power above) */
    if (es) es_seq(off, sizeof off);
}

int audio_esp_mic_open(uint32_t rate, uint8_t gain_db) {
    if (ch) return -1;
    const uint8_t adc[] = {
        0x0D, 0x01,                     /* analog power up */
        0x12, 0x02,                     /* DAC off */
        0x0E, 0x02,                     /* PGA and ADC modulator on */
        0x14, 0x10 | MIC_PGA,           /* Mic1p-Mic1n, PGA gain */
        0x16, MIC_SCALE,                /* ADC scale */
        0x17, (uint8_t)(0xBF + 2u * (gain_db > 24 ? 24 : gain_db)),   /* ADC volume: 0 dB + Gain */
        0x1B, 0x0A,                     /* ADC HPF stage-1 coefficient (esp_codec_dev's) */
        0x1C, 0x64,                     /* ADC EQ bypass, dynamic HPF, stage-2 coefficient 4 */
    };
    i2s_event_callbacks_t cbs = {.on_recv_q_ovf = rx_ovf};
    if (codec_power() != 0 || es_start(rate, 0xBA) != 0 || es_seq(adc, sizeof adc) != 0) goto fail;
    if (ch_make(false, rate, RX_DESC, &cbs) != 0) goto fail;
    stage = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rx_lost = rx_lost_seen = 0;
    stage_pos = stage_len = 0;
    discard = MIC_DISCARD;
    rearmed = false;
    if (!stage || i2s_channel_enable(ch) != ESP_OK) goto fail;
    mic_t0 = platform_esp_ms();
    return 0;
fail:
    mic_stop();
    platform_esp_log("gogo: mic: codec or I2S setup failed");
    return -1;
}

int audio_esp_mic_read(uint8_t *buf, uint32_t cap, uint32_t *lost) {
    if (!ch) return -1;
    uint32_t l = rx_lost;               /* one aligned word, written by the ISR only */
    *lost = l - rx_lost_seen;
    rx_lost_seen = l;
    if (!rearmed && platform_esp_ms() - mic_t0 >= MIC_REARM_MS) {
        rearmed = true;
        es_wr(0x0D, 0x01);              /* arm capture now that BCLK runs */
    }
    while (stage_pos == stage_len) {
        size_t n = 0;
        i2s_channel_read(ch, stage, buf_bytes, &n, 0);   /* one whole buffer or nothing */
        if (n == 0) return 0;
        stage_pos = 0;
        stage_len = (uint32_t)n;
        if (discard) {
            discard--;
            stage_len = 0;
        }
    }
    uint32_t k = stage_len - stage_pos;
    if (k > cap) k = cap & ~1u;         /* runtime/input/audio.c passes an even cap >= 2; the rest stays staged */
    memcpy(buf, stage + stage_pos, k);
    stage_pos += k;
    return (int)k;
}

void audio_esp_mic_close(void) { mic_stop(); }

/* ---- spk ---- */

static uint8_t *ring;                   /* SPK_RING bytes */
/* Byte counts since the open, wrapping: head by spk_write (bytes taken),
 * tail by the ISR (bytes moved into DMA buffers), played by the ISR (bytes
 * of buffers that finished). played <= tail <= head. */
static volatile uint32_t head, tail, played;
static void *tx_buf[TX_DESC];           /* the DMA buffers, learnt from on_sent */
static uint16_t tx_fill[TX_DESC];       /* bytes of sound in each */

static IRAM_ATTR bool tx_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *user) {
    (void)h;
    (void)user;
    uint8_t *b = e->dma_buf;
    int k = 0;
    while (k < TX_DESC && tx_buf[k] && tx_buf[k] != b) k++;
    if (k == TX_DESC) return false;
    tx_buf[k] = b;
    played += tx_fill[k];
    uint32_t t = tail, n = head - t;
    __sync_synchronize();               /* head before the bytes it covers */
    if (n > e->size) n = (uint32_t)e->size;
    for (uint32_t i = 0; i < n; i++) b[i] = ring[(t + i) & (SPK_RING - 1u)];
    tx_fill[k] = (uint16_t)n;
    __sync_synchronize();               /* the bytes read before the space is given back */
    tail = t + n;
    return false;
}

static int amp(bool on) {
#if HAS_PM1
    return pm1_spk_amp(on);
#else
    (void)on;                           /* Cardputer ADV: no amplifier switch */
    return 0;
#endif
}

static void spk_stop(void) {
    if (es) es_wr(0x31, 0x60);          /* DAC mute */
    amp(false);
    ch_free();
    heap_caps_free(ring);
    ring = NULL;
    static const uint8_t off[] = {0x32, 0x00, 0x12, 0x02};   /* DAC volume 0, DAC off; reference on */
    if (es) es_seq(off, sizeof off);
}

int audio_esp_spk_open(uint32_t rate, uint8_t volume) {
    if (ch) return -1;
#if HAS_PM1
    if (pm1_spk_amp_blocked()) return -1;   /* the IR receiver is open */
#endif
    /* 0xBF = 0 dB, 0.5 dB per step: the board's ceiling minus 0.4 dB per
     * step below 100. */
    int top = 0xBF + 2 * AUDIO_SPK_MAX_DB;
    uint8_t vol = volume == 0 ? 0 : (uint8_t)(top - (int)((100u - volume) * 4u / 5u));
    const uint8_t dac[] = {
        0x0D, 0x01,                     /* analog power up */
        0x0E, 0x6A,                     /* PGA and ADC modulator off */
        0x12, 0x00,                     /* DAC on */
        0x13, 0x10,                     /* output to the HP driver */
        0x32, vol,                      /* DAC volume, 0xBF = 0 dB, 0.5 dB steps */
        0x37, 0x08,                     /* DAC EQ bypass */
        0x31, 0x00,                     /* unmuted */
    };
    i2s_event_callbacks_t cbs = {.on_sent = tx_sent};
    if (codec_power() != 0 || es_start(rate, 0xB5) != 0 || es_seq(dac, sizeof dac) != 0) goto fail;
    ring = heap_caps_malloc(SPK_RING, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    head = tail = played = 0;
    memset(tx_buf, 0, sizeof tx_buf);
    memset(tx_fill, 0, sizeof tx_fill);
    if (!ring || ch_make(true, rate, TX_DESC, &cbs) != 0 || i2s_channel_enable(ch) != ESP_OK) goto fail;
    if (amp(true) != 0) goto fail;      /* last: the codec already plays silence */
    return 0;
fail:
    spk_stop();
    platform_esp_log("gogo: spk: codec or I2S setup failed");
    return -1;
}

int audio_esp_spk_write(const uint8_t *data, uint32_t len) {
    if (!ch) return -1;
    uint32_t h = head, room = SPK_RING - (h - tail);
    __sync_synchronize();               /* tail before reusing the space it gave back */
    uint32_t n = (len < room ? len : room) & ~1u;
    for (uint32_t i = 0; i < n; i++) ring[(h + i) & (SPK_RING - 1u)] = data[i];
    __sync_synchronize();               /* the bytes before head */
    head = h + n;
    return (int)n;
}

uint32_t audio_esp_spk_queued(void) { return head - played; }

void audio_esp_spk_close(void) { spk_stop(); }
