/* measures LCD throughput: full-screen fills and text, timed with the 10 ms tick */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "strings.h"

void app_speedtest(void)
{
    static const uint16_t cols[] = { RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255) };
    uint16_t t0, fill_t, text_t;
    uint8_t i;
    event_t e;
    skin_load();
    t0 = hal_ticks();
    for (i = 0; i < 16; i++) gfx_fill(0, 0, LCD_W, LCD_H, cols[i & 3]);
    fill_t = hal_ticks() - t0;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    t0 = hal_ticks();
    for (i = 0; i < 12; i++) gfx_text(0, i * 9 + 4, "THE QUICK BROWN FOX", 1, C_FG, C_BG);
    text_t = hal_ticks() - t0;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    gfx_text(4, 20, T(S_FILLS), 1, C_FG, C_BG);
    gfx_num(4, 30, fill_t, 2, C_OK, C_BG);
    gfx_text(70, 34, "X10MS", 1, C_DIM, C_BG);
    gfx_text(4, 52, T(S_GLYPHS), 1, C_FG, C_BG);
    gfx_num(4, 62, text_t, 2, C_OK, C_BG);
    gfx_text(70, 66, "X10MS", 1, C_DIM, C_BG);
    skin_footer(T(S_BACK));
    do ev_wait(&e, 0); while (e.type != EV_KEY_DOWN);
}
