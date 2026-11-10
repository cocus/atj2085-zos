/* zos kernel: event queue + main loop helpers */
#ifndef ZOS_EVENT_H
#define ZOS_EVENT_H
#include <stdint.h>

enum { EV_NONE, EV_KEY_DOWN, EV_KEY_UP, EV_KEY_REPEAT, EV_SECOND_HALF, EV_QUIT,
       EV_DSP,              /* EV_DSP: code = DSP request (the ISR read + acked it; reply from the main loop) */
       EV_POWER,            /* PLAY held 2 s (rest of that press swallowed): power off. A module
                               leaves like on MENU (audio off, resume point kept) with MOD_POWER_REQ
                               set (core/module.h); SHELL then does the stock standby */
       EV_BATT };           /* battery level / USB power changed (debounced; code 0, state in
                               HAL_BATT): modules showing the battery icon redraw it (skin_batt) */

typedef struct { uint8_t type, code; } event_t;

void ev_post(uint8_t type, uint8_t code);            /* ISR-safe (single producer) */
uint8_t ev_get(event_t *e);                          /* non-blocking, 0 = empty */
/* wait for an event or until `timeout` ticks passed (0 = forever); idles the CPU,
 * kicks the watchdog. Returns 0 on timeout. */
uint8_t ev_wait(event_t *e, uint16_t timeout);
#endif
