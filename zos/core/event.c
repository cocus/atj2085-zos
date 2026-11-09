#include "event.h"
#include "../hal/hal.h"

#define QLEN 32                        /* power of two */
static event_t q[QLEN];
static volatile uint8_t head, tail;    /* head: producer (ISR), tail: consumer */

void ev_post(uint8_t type, uint8_t code)
{
    uint8_t h = head, n = (h + 1) & (QLEN - 1);
    if (n == tail) return;             /* full: drop */
    q[h].type = type; q[h].code = code;
    head = n;
}

uint8_t ev_get(event_t *e)
{
    uint8_t t = tail;
    if (t == head) return 0;
    *e = q[t];
    tail = (t + 1) & (QLEN - 1);
    return 1;
}

uint8_t ev_wait(event_t *e, uint16_t timeout)
{
    uint16_t t0 = hal_ticks();
    for (;;) {
        hal_watchdog();
        if (ev_get(e)) return 1;
        if (timeout && (uint16_t)(hal_ticks() - t0) >= timeout) return 0;
        hal_idle();
    }
}
