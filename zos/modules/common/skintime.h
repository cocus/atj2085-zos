/* HH:MM in the title bar (24 h, no seconds), shared by skin.c and skinlite.c (the includer defines TM_FG
 * and TM_BG = the bar's text / background colour). The slot is chosen by skin_title() from the title's
 * extent: right-aligned next to the battery icon (right edge without it) when the title leaves room,
 * else at the left edge, else no time. Drawn only while a clock runs (hal/hal.h: HAL_CLK, rtc.h CLK_OK);
 * redrawn alone on EV_TIME (skin_wait / skin_get). */
#ifndef ZOS_SKINTIME_H
#define ZOS_SKINTIME_H
#include "rtc.h"

#define TM_W (5 * FONT_W)
static uint8_t tm_x;                              /* x of the slot, 0 = none */

static void tm_place(uint8_t t0, uint8_t t1, uint8_t xr)   /* the title spans [t0, t1); xr = the right slot */
{
    tm_x = 0;
    if (t1 + 4 <= xr) tm_x = xr;
    else if (t0 >= 2 + TM_W + 4) tm_x = 2;
}

static void tm_draw(void)
{
    char b[6];
    uint8_t h, m, t;
    volatile hal_clock_t *c = HAL_CLK;
    if (!tm_x || !CLK_OK) return;
    do { h = c->hour; m = c->min; } while (h != c->hour || m != c->min);
    for (t = 0; h >= 10; h -= 10) t++;
    b[0] = '0' + t; b[1] = '0' + h; b[2] = ':';
    for (t = 0; m >= 10; m -= 10) t++;
    b[3] = '0' + t; b[4] = '0' + m; b[5] = 0;
#ifdef TM_ROWS                                   /* skin.c: the bar is an image (a gradient): put its own row colours
                                                 * back under the slot, then draw only the digits' set pixels */
    for (t = 0; t < TM_ROWS; t++) gfx_fill(tm_x, 2 + t, TM_W, 1, tm_bg[t]);
    skin_text(tm_x, 3, b, TM_FG);
#else
    gfx_text(tm_x, 3, b, 1, TM_FG, TM_BG);
#endif
}
#endif
