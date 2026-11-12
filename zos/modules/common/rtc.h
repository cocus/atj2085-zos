/* The OS wall clock, module side (the clock itself ticks in the core: hal/hal.h HAL_CLK, EV_TIME in
 * core/event.h). A date is a day count since 2000-01-01 (the core keeps only that + hh:mm:ss: no
 * calendar maths there); the calendar conversions (years 2000..2099) live here. Include rtc.c from one
 * module source (SHELL, SETTINGS, SYSINFO). Reading the time only needs the header (clk_ok / clk_get),
 * also in modules without room for rtc.c. */
#ifndef ZOS_RTC_H
#define ZOS_RTC_H
#include <stdint.h>
#include "../../hal/hal.h"

typedef struct { uint16_t day; uint8_t hour, min, sec; } clk_t;

/* a running clock: this core has one (home() marks it) and a source set the time. An older core never
 * sets the marker: modules then show no time and nothing else changes. */
#define CLK_OK (HAL_CLK->core == HAL_CLK_CORE && HAL_CLK->src)

/* consistent snapshot (the interrupt may tick while reading); static: usable without rtc.c */
static void clk_get(clk_t *t)
{
    volatile hal_clock_t *c = HAL_CLK;
    do {
        t->day = c->day; t->hour = c->hour; t->min = c->min; t->sec = c->sec;
    } while (t->sec != c->sec || t->min != c->min || t->hour != c->hour || t->day != c->day);
}
/* set the time of day / date (seconds, half-second phase 0; re-bases the counter) and the source */
void clk_set(const clk_t *t, uint8_t src);
/* the counter-based time (hal.h): correct the software clock by it, see rtc.c; 1 = it was corrected.
 * SHELL: every start, each EV_TIME, and after standby (big = 1: the gap may be hours) */
uint8_t clk_sync(uint8_t big);
/* t + secs */
void clk_add(clk_t *t, uint32_t secs);
/* the 24-bit MCU RTC counter (2 Hz): latched now */
uint32_t clk_counter(void);

uint8_t  cal_dim(uint8_t y, uint8_t m);                       /* days in month m (1..12) of year 2000 + y */
uint16_t cal_day(uint8_t y, uint8_t m, uint8_t d);            /* -> days since 2000-01-01 */
void     cal_ymd(uint16_t day, uint8_t *y, uint8_t *m, uint8_t *d);
uint8_t  cal_dow(uint16_t day);                               /* 1 = Monday .. 7 = Sunday */
#endif
