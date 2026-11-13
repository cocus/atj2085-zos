/* The OS wall clock, module side: see rtc.h. Years 2000..2099 (every 4th year is a leap year). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "regs.h"
#include "rtc.h"


static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

uint8_t cal_dim(uint8_t y, uint8_t m)
{
    return mdays[m - 1] + (m == 2 && !(y & 3));
}

uint16_t cal_day(uint8_t y, uint8_t m, uint8_t d)
{
    uint16_t n = (uint16_t)y * 365 + ((y + 3) >> 2) + d - 1;     /* + the leap days of the years before */
    uint8_t i;
    for (i = 1; i < m; i++) n += cal_dim(y, i);
    return n;
}

void cal_ymd(uint16_t day, uint8_t *y, uint8_t *m, uint8_t *d)
{
    uint8_t yy = 0, mm = 1;
    uint16_t n;
    for (;;) {
        n = 365 + !(yy & 3);
        if (day < n) break;
        day -= n;
        yy++;
    }
    while (day >= (n = cal_dim(yy, mm))) { day -= n; mm++; }
    *y = yy; *m = mm; *d = (uint8_t)day + 1;
}

uint8_t cal_dow(uint16_t day)                    /* 2000-01-01 was a Saturday */
{
    return (uint8_t)((day + 5) % 7) + 1;
}

uint32_t clk_counter(void)
{
    RTCCTL |= RTCCTL_LATCH;
    return ((uint32_t)RTCT_HI << 16) | ((uint16_t)RTCT_MI << 8) | RTCT_LO;
}

static uint32_t to_secs(const clk_t *t)
{
    return (uint32_t)t->day * 86400UL + (uint32_t)t->hour * 3600 + (uint16_t)t->min * 60 + t->sec;
}

static void base_set(volatile hal_clock_t *c, const clk_t *t)   /* the counter's time base */
{
    uint32_t n = clk_counter();
    c->cnt0 = (uint8_t)n; c->cnt1 = (uint8_t)(n >> 8); c->cnt2 = (uint8_t)(n >> 16);
    c->bday = t->day; c->bhour = t->hour; c->bmin = t->min; c->bsec = t->sec;
}

static void put_time(volatile hal_clock_t *c, const clk_t *t)
{
    uint8_t src = c->src;
    c->src = 0;                                  /* the core does not tick while src is 0 */
    c->half = 0; c->sec = t->sec; c->min = t->min; c->hour = t->hour; c->day = t->day;
    c->src = src;
}

void clk_set(const clk_t *t, uint8_t src)
{
    volatile hal_clock_t *c = HAL_CLK;
    if (c->core != HAL_CLK_CORE) return;
    c->src = 0;
    base_set(c, t);
    c->err = 0;
    c->half = 0; c->sec = t->sec; c->min = t->min; c->hour = t->hour; c->day = t->day;
    c->src = src;
}

/* the counter's time vs the software tick (see hal.h): a late software clock is corrected (`big`: any
 * amount, after standby; else up to 2 minutes), a clock that is ahead of the counter = the counter was
 * cleared / does not count: re-base on the software time and count it in err. Returns 1 if corrected. */
uint8_t clk_sync(uint8_t big)
{
    volatile hal_clock_t *c = HAL_CLK;
    clk_t b, s;
    uint32_t e, cur;
    int32_t d;
    if (!CLK_OK) return 0;
    b.day = c->bday; b.hour = c->bhour; b.min = c->bmin; b.sec = c->bsec;
    clk_get(&s);
    e = ((clk_counter() - ((uint32_t)c->cnt2 << 16 | (uint16_t)c->cnt1 << 8 | c->cnt0)) & 0xFFFFFFUL) >> 1;
    cur = to_secs(&s);
    d = (int32_t)(to_secs(&b) + e - cur);
    if (d >= -3 && d <= 3) return 0;
    if (d > 0 && (big || d < 120)) {
        clk_add(&b, e);
        put_time(c, &b);
        return 1;
    }
    base_set(c, &s);                             /* counter cleared / not counting: start over from the software time */
    if (c->err != 255) c->err++;
    return 0;
}

void clk_add(clk_t *t, uint32_t secs)
{
    secs += (uint32_t)t->hour * 3600 + (uint16_t)t->min * 60 + t->sec;
    t->day += (uint16_t)(secs / 86400);
    secs %= 86400;
    t->hour = (uint8_t)(secs / 3600);
    secs %= 3600;
    t->min = (uint8_t)(secs / 60);
    t->sec = (uint8_t)(secs % 60);
}
