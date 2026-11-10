#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/draw.h"
#include "strings.h"
#include "regs.h"

SFR(SI_BATT, P_BATT);
SFR(SI_USBST, P_USB_STAT);

/* battery: raw ADC level (0.90 V + 0.04 V/step), USB power, and the debounced state the icon uses
 * (HAL_BATT: level, U = USB power; "-" = not sampled yet) - for tuning targets/<t>/board.h */
static void batt_line(void)
{
    uint8_t s = HAL_BATT;
    gfx_fill(70, 82, 58, 20, C_BG);
    gfx_num(70, 82, SI_BATT & BATT_LEVEL, 1, C_OK, C_BG);
    gfx_text(88, 82, SI_USBST & USBST_VBUS ? "USB" : "BAT", 1, C_OK, C_BG);
    if (s) {
        gfx_num(70, 92, s & HAL_BATT_LEVEL, 1, C_OK, C_BG);
        if (s & HAL_BATT_VBUS) gfx_text(88, 92, "USB", 1, C_OK, C_BG);
    } else gfx_text(70, 92, "-", 1, C_DIM, C_BG);
    batt_icon(70, 104, C_FG, C_BG);
}

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
    gfx_text(4, 82, T(S_BATT), 1, C_FG, C_BG);
    gfx_text(4, 92, T(S_BICON), 1, C_FG, C_BG);
    skin_footer(T(S_FOOTER));
    for (;;) {
        e.type = EV_NONE;
        skin_wait(&e, 10);
        if (e.type == EV_SECOND_HALF) { halfs++; batt_line(); }
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
