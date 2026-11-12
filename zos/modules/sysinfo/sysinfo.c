#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/draw.h"
#include "../common/rtc.h"
#include "../common/ds3231.h"
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

/* the wall clock (hal/hal.h HAL_CLK): date + time with seconds, where the time came from (SRC: DS3231 /
 * ZOS.CFG anchor / build date), the DS3231 probe (DS), the MCU RTC counter at the last re-base (CNT, hex,
 * 2 Hz) and its health: OK, or Ex = x times the counter did not agree with the software tick
 * (cleared, not counting; clk_sync) */
static uint8_t dsst;                          /* ds_read() at start */

static void put2(char *b, uint8_t v)
{
    uint8_t t = 0;
    while (v >= 10) { v -= 10; t++; }
    b[0] = '0' + t; b[1] = '0' + v;
}

static void clock_lines(void)
{
    static const char *const srcn[] = { "NONE", "BUILD", "SAVED", "DS3231" };
    static const char *const dsn[] = { "NONE", "OK", "OSF" };
    char b[20], *h = b + 14;
    clk_t t;
    uint8_t y, m, d, i;
    volatile hal_clock_t *c = HAL_CLK;
    gfx_fill(4, 19, 124, 25, C_BG);
    if (!CLK_OK) { gfx_text(4, 19, "NO CLOCK (OLD CORE)", 1, C_DIM, C_BG); return; }
    clk_get(&t);
    cal_ymd(t.day, &y, &m, &d);
    b[0] = '2'; b[1] = '0'; put2(b + 2, y); b[4] = '-'; put2(b + 5, m); b[7] = '-'; put2(b + 8, d);
    b[10] = ' '; put2(b + 11, t.hour); b[13] = ':';
    put2(b + 14, t.min); b[16] = ':'; put2(b + 17, t.sec); b[19] = 0;
    gfx_text(4, 19, b, 1, C_OK, C_BG);
    gfx_text(4, 28, "SRC", 1, C_FG, C_BG);
    gfx_text(28, 28, srcn[c->src & 3], 1, C_OK, C_BG);
    gfx_text(70, 28, "DS", 1, C_FG, C_BG);
    gfx_text(86, 28, dsn[dsst], 1, dsst == DS_OK ? C_OK : C_DIM, C_BG);
    gfx_text(4, 37, "CNT", 1, C_FG, C_BG);
    for (i = 0; i < 6; i++) {                                   /* 24-bit counter, hex */
        y = (i & 1 ? 0 : 4);
        d = i < 2 ? c->cnt2 : i < 4 ? c->cnt1 : c->cnt0;
        b[i] = "0123456789ABCDEF"[(d >> y) & 15];
    }
    b[6] = 0;
    gfx_text(28, 37, b, 1, C_OK, C_BG);
    if (!c->err) gfx_text(70, 37, "OK", 1, C_OK, C_BG);
    else { gfx_text(70, 37, "E", 1, C_SEL, C_BG); gfx_num(76, 37, c->err, 1, C_SEL, C_BG); }
}

void app_sysinfo(void)
{
    event_t e;
    uint16_t halfs = 0, keys = 0;
    ds_time_t dt;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    dsst = ds_read(&dt);
    clock_lines();
    gfx_text(4, 46, T(S_TICKS), 1, C_FG, C_BG);
    gfx_text(4, 58, T(S_SECONDS), 1, C_FG, C_BG);
    gfx_text(4, 70, T(S_KEYS), 1, C_FG, C_BG);
    gfx_text(4, 82, T(S_BATT), 1, C_FG, C_BG);
    gfx_text(4, 92, T(S_BICON), 1, C_FG, C_BG);
    skin_footer(T(S_FOOTER));
    for (;;) {
        e.type = EV_NONE;
        skin_wait(&e, 10);
        if (e.type == EV_SECOND_HALF) { halfs++; batt_line(); clock_lines(); }
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
