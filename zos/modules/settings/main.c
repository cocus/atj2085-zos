/* SETTINGS: language, LCD contrast, backlight timeout, battery icon, key beep, FM USB out
 * (core/config.h, ZCFG).
 * << >> pick a row, PLAY next value, VOL previous value, MENU back (saves ZOS.CFG, common/cfg.c).
 */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../core/config.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/beep.h"
#include "../common/draw.h"
#include "../common/cfg.h"
#include "strings.h"

enum { R_LANG, R_CONTRAST, R_BACKLIGHT, R_BATT, R_BEEP, R_FMUSB, NROWS };
static const uint8_t row_str[NROWS] = { S_LANG, S_CONTRAST, S_BACKLIGHT, S_BATT, S_BEEP, S_FMUSB };
static const uint8_t row_flag[NROWS] = { 0, 0, 0, ZF_BATT, ZF_BEEP, ZF_FMUSB };   /* on/off rows */
static const uint8_t bl_str[NBL] = { S_BL_NEVER, S_BL_10, S_BL_30, S_BL_60 };
#define TOP_Y 22
#define ROW_H 14
#define VAL_X 80

static uint8_t sel;

static void row(uint8_t r)
{
    uint8_t y = TOP_Y + r * ROW_H, s = r == sel;
    uint16_t bg = s ? sk.sel_bot : sk.bg, fg = s ? sk.sel_text : sk.fg;
    gfx_fill(0, y, LCD_W, ROW_H - 2, bg);
    gfx_text(4, y + 2, T(row_str[r]), 1, fg, bg);
    y -= 2;                                                /* values: same baseline */
    switch (r) {
    case R_LANG:      gfx_text(VAL_X, y + 4, T(S_LANGNAME), 1, fg, bg); break;
    case R_CONTRAST:                                       /* number + small bar */
        gfx_num(VAL_X, y + 4, ZCFG->contrast, 1, fg, bg);
        gfx_fill(VAL_X + 16, y + 6, 28, 4, s ? sk.sel_top : sk.bar);
        gfx_fill(VAL_X + 16, y + 6, (uint8_t)(ZCFG->contrast * 28u / CONTRAST_MAX), 4, fg);
        break;
    case R_BACKLIGHT: gfx_text(VAL_X, y + 4, T(bl_str[ZCFG->backlight]), 1, fg, bg); break;
    default:                                               /* on/off rows */
        gfx_text(VAL_X, y + 4, T(ZCFG->flags & row_flag[r] ? S_ON : S_OFF), 1, fg, bg);
        if (r == R_BATT && ZCFG_BATT) batt_icon(VAL_X + 24, y + 2, fg, bg);
        break;
    }
}

static void screen(void)
{
    uint8_t r;
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title(T(S_TITLE));
    for (r = 0; r < NROWS; r++) row(r);
    gfx_text(4, LCD_H - 20, T(S_NOTE), 1, sk.dim, sk.bg);
    gfx_text(4, LCD_H - 9, T(S_FOOTER), 1, sk.dim, sk.bg);
}

static void change(int8_t d)
{
    switch (sel) {
    case R_LANG: ZCFG->lang = ZCFG_LANG == LANG_EN ? LANG_ES : LANG_EN; screen(); return;
    case R_CONTRAST:
        if (d > 0 ? ZCFG->contrast < CONTRAST_MAX : ZCFG->contrast > 0) ZCFG->contrast += d;
        hal_lcd_contrast(ZCFG->contrast);
        break;
    case R_BACKLIGHT: ZCFG->backlight = (ZCFG->backlight + NBL + d) % NBL; break;
    default:
        ZCFG->flags ^= row_flag[sel];
        if (sel == R_BATT) { screen(); return; }           /* the title bar changes too */
        break;
    }
    row(sel);
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t old;
    if (ZCFG->magic != ZCFG_MAGIC) return 0;               /* SHELL sets the defaults */
    if (ZCFG->backlight >= NBL) ZCFG->backlight = BL_ALWAYS;
    skin_load();
    screen();
    for (;;) {
        skin_wait(&e, 0);
        if (e.type == EV_KEY_DOWN) beep();             /* key beep (SETTINGS) */
        if (e.type == EV_POWER) MOD_POWER_REQ = 1;           /* saved below, SHELL powers off */
        if (e.type == EV_QUIT || e.type == EV_POWER) break;
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        if (e.code == KEY_MENU) break;
        old = sel;
        if (e.code == KEY_RIGHT && sel + 1 < NROWS) sel++;
        else if (e.code == KEY_LEFT && sel) sel--;
        else if (e.code == KEY_PLAY) change(1);
        else if (e.code == KEY_VOL) change(-1);
        if (sel != old) { row(old); row(sel); }
    }
    cfg_save();
    return 0;
}
