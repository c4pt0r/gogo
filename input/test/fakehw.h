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

#endif
