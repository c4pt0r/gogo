/* A scripted InputHw for the input tests: a board with a few keys, a
 * queue of raw events the kbd_read hook returns, and call counters. */
#ifndef GOGO_FAKEHW_H
#define GOGO_FAKEHW_H

#include "input.h"

/* Raw key numbers of the fake board (codes in fakehw.c). */
enum {
    R_A, R_SEMI, R_1, R_C, R_SHIFT, R_CTRL, R_FN, R_OPT,
    R_B, R_D, R_E, R_F, R_G, R_H, R_I, R_J, R_COUNT
};

void fake_reset(void);                 /* clears the queue and counters, installs a keyboard */
void fake_no_keyboard(void);           /* keys == NULL */
void fake_push(uint16_t raw, bool down);
void fake_fail_next_read(void);        /* the next kbd_read returns -1 */
void fake_fail_all_reads(bool on);   /* every kbd_read returns -1 while set */
int fake_kbd_reads(void);              /* kbd_read calls */
void fake_fail_read_after(int n);   /* the (n+1)th kbd_read from now returns -1 */
int fake_opens(void), fake_closes(void);
void fake_install_once(void);          /* input_install() once per test binary */

/* Power: fake_reset() installs a power chip at 3700 mV, not charging, no external. */
void fake_no_power(void);              /* power_read == NULL */
void fake_power_set(int32_t mv, int8_t charging, int8_t external);
void fake_power_fail(bool fail);       /* power_read returns -1 while set */
void fake_power_new(void);             /* raises the "new value" flag (power_pending true until read) */
int fake_power_reads(void);            /* power_read calls (failed ones too) */

/* The optional hooks, off after fake_reset(): fake_live_hook installs live()
 * (returning `live`) and power_enable(); fake_set_live changes the answer. */
void fake_live_hook(bool live);
void fake_set_live(bool live);
void fake_event_driven(bool on);       /* power_event_driven: no periodic sampling */
int fake_power_enabled(void);          /* opens - closes seen by power_enable */

/* IMU: fake_reset() installs one. The FIFO holds whole 12-byte samples. */
void fake_no_imu(void);                /* imu hooks NULL */
void fake_imu_no_fifo(void);           /* imu_read_fifo NULL: a gesture-only chip */
void fake_imu_push(int n, int16_t first);   /* n samples; sample i has all six values first + i */
int fake_imu_fifo_len(void);           /* samples waiting */
int fake_imu_fifo_reads(void);         /* imu_read_fifo calls */
void fake_imu_fifo_fail(bool on);      /* imu_read_fifo returns -1 while set */
void fake_imu_gesture(uint8_t bits, uint8_t orient, uint16_t steps);   /* seen at the next imu_read_gestures */
int fake_imu_gesture_reads(void);
void fake_imu_open_fail(bool on);
int fake_imu_opens(void), fake_imu_closes(void);
int fake_imu_rate(void), fake_imu_mask(void);   /* the arguments of the last imu_open */

/* IR: fake_reset() installs a transmitter and a receiver. */
void fake_no_ir(void);                 /* every ir hook NULL */
void fake_no_ir_rx_close(void);        /* ir_rx_close NULL (optional) */
void fake_no_ir_tx_open_close(void);   /* ir_tx_open / ir_tx_close NULL (optional) */
void fake_ir_tx_open_fail(bool on);    /* ir_tx_open returns -1 while set */
const char *fake_ir_tx_log(void);      /* the tx hook calls in order: O open, S start, C close */
uint32_t fake_ir_tx_open_carrier(void);
int fake_ir_tx_open_duty(void);
int fake_ir_tx_starts(void);           /* successful ir_tx_start calls */
int fake_ir_tx_n(void);                /* value count of the last start */
uint16_t fake_ir_tx_value(int i);      /* value i of the last start (copied by the hook) */
uint32_t fake_ir_tx_carrier(void);
int fake_ir_tx_duty(void);
void fake_ir_tx_set_busy(bool on);     /* ir_tx_busy's answer */
void fake_ir_tx_fail(bool on);         /* ir_tx_start returns -1 while set */
bool fake_ir_tx_started_while_busy(void);
void fake_ir_rx_push(const uint16_t *v, int n);   /* one frame, any length up to 400 */
void fake_ir_rx_start(uint32_t ms);    /* ir_rx_read reports frames as begun at ms */
int fake_ir_tx_fails(void);            /* failed ir_tx_start calls */
void fake_ir_rx_fail(bool on);         /* ir_rx_read returns -1 while set */
void fake_ir_rx_open_fail(bool on);
int fake_ir_rx_opens(void), fake_ir_rx_closes(void), fake_ir_rx_reads(void);
uint32_t fake_ir_rx_gap(void);         /* gap_us of the last ir_rx_open */

#endif
