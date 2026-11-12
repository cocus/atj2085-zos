/* zos skins, light variant for modules without room for skin.c (see skin.h): the colours come from
 * the copy SHELL keeps in HAL_SHARED (skc), nothing is read from the disk. Include this file from
 * one module source instead of skin.c; the calls are the same:
 *   skin_load()   once at start (no skin loaded yet: the apps/theme.h colours)
 *   skin_title()  plain bar in the skin's title colour (16 rows like TITLE.ZI) + centred title
 *   skin_footer() bottom line in the skin's dim colour
 * Colours: skc.c.bg ..., or the apps/theme.h names after #include "skcol.h". */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "skin.h"

static const skin_shared_t skin_def = { SKIN_DEFAULTS, 1, 0 };

uint8_t skin_load(void)
{
    if (!skc.ok) skc = skin_def;
    return 0;
}

#define TM_FG skc.c.fg
#define TM_BG skc.c.bar
#include "skintime.h"

void skin_title(const char *title)
{
    uint8_t n = 0, x;
    while (title[n]) n++;
    x = LCD_W / 2 - n * (FONT_W / 2);
    gfx_fill(0, 0, LCD_W, SKIN_TITLE_H, skc.c.bar);
    gfx_text(x, 3, title, 1, skc.c.fg, skc.c.bar);
    tm_place(x, x + n * FONT_W, LCD_W - 2 - TM_W);
    tm_draw();
}

void skin_batt(void) { }                       /* (no battery icon here) */
void skin_time(void) { tm_draw(); }
uint8_t skin_wait(event_t *e, uint16_t timeout)
{
    uint8_t r = ev_wait(e, timeout);
    if (r && e->type == EV_TIME) tm_draw();
    return r;
}
uint8_t skin_get(event_t *e)
{
    uint8_t r = ev_get(e);
    if (r && e->type == EV_TIME) tm_draw();
    return r;
}

void skin_footer(const char *text)
{
    gfx_fill(0, LCD_H - 10, LCD_W, 10, skc.c.bg);
    gfx_text(4, LCD_H - 9, text, 1, skc.c.dim, skc.c.bg);
}
