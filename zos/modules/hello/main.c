/* HELLO: smallest module - shows where it runs from (page + address), any key returns.
 * Skinned the cheap way (common/skinlite.c: colours SHELL left in HAL_SHARED, no disk access). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "strings.h"

static uint16_t counter = 1234;     /* initialized data: checks mod_init */
static uint8_t zeroed[4];           /* bss */

uint16_t mod_main(void)
{
    event_t e;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    gfx_text(4, 20, T(S_PAGE), 1, C_DIM, C_BG);
    gfx_num(52, 20, page_get(), 1, C_SEL, C_BG);
    gfx_text(4, 32, T(S_MAIN), 1, C_DIM, C_BG);
    gfx_num(52, 32, (uint16_t)mod_main, 1, C_SEL, C_BG);
    gfx_text(4, 44, T(S_DATA), 1, C_DIM, C_BG);
    gfx_num(52, 44, counter + zeroed[2], 1, counter == 1234 && !zeroed[2] ? C_OK : C_SEL, C_BG);
    skin_footer(T(S_BACK));
    do skin_wait(&e, 0); while (e.type != EV_KEY_DOWN);
    return 0;
}
