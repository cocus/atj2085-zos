#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "strings.h"

void app_sysinfo(void)
{
    event_t e;
    uint16_t halfs = 0, keys = 0;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    gfx_text(4, 20, "ZOS M2 HOSTED", 1, C_FG, C_BG);
    gfx_text(4, 30, "ATJ2085 / LSD501", 1, C_DIM, C_BG);
    gfx_text(4, 46, T(S_TICKS), 1, C_FG, C_BG);
    gfx_text(4, 58, T(S_SECONDS), 1, C_FG, C_BG);
    gfx_text(4, 70, T(S_KEYS), 1, C_FG, C_BG);
    skin_footer(T(S_FOOTER));
    for (;;) {
        e.type = EV_NONE;
        ev_wait(&e, 10);
        if (e.type == EV_SECOND_HALF) halfs++;
        if (e.type == EV_KEY_DOWN) {
            keys++;
            if (e.code == KEY_MENU) return;
        }
        gfx_fill(70, 46, 58, 32, C_BG);
        gfx_num(70, 46, hal_ticks(), 1, C_OK, C_BG);
        gfx_num(70, 58, halfs / 2, 1, C_OK, C_BG);
        gfx_num(70, 70, keys, 1, C_OK, C_BG);
    }
}
