/* SETTINGS: language, LCD contrast, backlight timeout, battery icon, key beep, FM USB out
 * (core/config.h, ZCFG).
 * << >> pick a row, PLAY next value, VOL previous value, MENU back (saves ZOS.CFG, common/cfg.c).
 * SET CLOCK (PLAY on its row) opens the date / time editor: << >> change the field, VOL next field,
 * PLAY sets the OS clock (hal/hal.h HAL_CLK, seconds = 0) and the DS3231 when one answers (and saves
 * the clock anchor into ZOS.CFG), MENU leaves without setting.
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
#include "../common/rtc.h"
#include "../common/ds3231.h"
#include "strings.h"

enum { R_LANG, R_CONTRAST, R_BACKLIGHT, R_BATT, R_BEEP, R_FMUSB, R_CLOCK, NROWS };
static const uint8_t row_str[NROWS] = { S_LANG, S_CONTRAST, S_BACKLIGHT, S_BATT, S_BEEP, S_FMUSB, S_CLOCK };
static const uint8_t row_flag[NROWS] = { 0, 0, 0, ZF_BATT, ZF_BEEP, ZF_FMUSB, 0 };   /* on/off rows */
static const uint8_t bl_str[NBL] = { S_BL_NEVER, S_BL_10, S_BL_30, S_BL_60 };
#define TOP_Y 22
#define ROW_H 12
#define VAL_X 80

static uint8_t sel;

static void put2(char *b, uint8_t v)                       /* two digits */
{
    uint8_t t = 0;
    while (v >= 10) { v -= 10; t++; }
    b[0] = '0' + t; b[1] = '0' + v;
}

static void fmt_hm(char *b)                                /* HH:MM or --:-- (no clock) */
{
    clk_t t;
    b[2] = ':'; b[5] = 0;
    if (!CLK_OK) { b[0] = b[1] = b[3] = b[4] = '-'; return; }
    clk_get(&t);
    put2(b, t.hour); put2(b + 3, t.min);
}

/* ---- SET CLOCK ----------------------------------------------------------------------------- */
enum { F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MIN, NF };
static const uint8_t f_str[NF] = { S_YEAR, S_MONTH, S_DAY, S_HOUR, S_MINUTE };
static const uint8_t f_max[NF] = { 99, 12, 31, 23, 59 };
static const uint8_t f_min[NF] = { 0, 1, 1, 0, 0 };
static uint8_t fv[NF], fsel, dsst;                         /* field values (year 0..99 = 2000..), selected, DS3231 state */

static void field(uint8_t i)
{
    uint8_t y = 22 + i * 14, s = i == fsel;
    uint16_t bg = s ? sk.sel_bot : sk.bg, fg = s ? sk.sel_text : sk.fg;
    char b[5];
    gfx_fill(0, y, LCD_W, 12, bg);
    gfx_text(4, y + 2, T(f_str[i]), 1, fg, bg);
    if (i == F_YEAR) { b[0] = '2'; b[1] = '0'; put2(b + 2, fv[i]); b[4] = 0; }
    else { put2(b, fv[i]); b[2] = 0; }
    gfx_text(VAL_X, y + 2, b, 1, fg, bg);
}

static void fix_day(void)
{
    uint8_t n = cal_dim(fv[F_YEAR], fv[F_MONTH]);
    if (fv[F_DAY] > n) fv[F_DAY] = n;
}

static void apply_clock(void)
{
    clk_t t;
    ds_time_t d;
    t.day = cal_day(fv[F_YEAR], fv[F_MONTH], fv[F_DAY]);
    t.hour = fv[F_HOUR]; t.min = fv[F_MIN]; t.sec = 0;
    d.year = fv[F_YEAR]; d.month = fv[F_MONTH]; d.date = fv[F_DAY];
    d.hour = t.hour; d.min = t.min; d.sec = 0; d.dow = cal_dow(t.day);
    clk_set(&t, ds_write(&d) ? HAL_CLK_RTC : HAL_CLK_ANCHOR);   /* the DS3231 when it took the time */
    cfg_anchor_force = 1;
    cfg_save();                                                  /* the anchor, right away */
}

/* 1 = leave SETTINGS too (power key) */
static uint8_t clock_edit(void)
{
    event_t e;
    clk_t t;
    uint8_t i, y, m, d;
    ds_time_t dt;
    if (!CLK_OK) return 0;                                 /* an older core: no clock */
    clk_get(&t);
    cal_ymd(t.day, &y, &m, &d);
    fv[F_YEAR] = y; fv[F_MONTH] = m; fv[F_DAY] = d; fv[F_HOUR] = t.hour; fv[F_MIN] = t.min;
    fsel = 0;
    dsst = ds_read(&dt);
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title(T(S_CLKTITLE));
    for (i = 0; i < NF; i++) field(i);
    gfx_text(4, 96, T(dsst == DS_OK ? S_DSYES : dsst == DS_OSF ? S_DSOSF : S_DSNO), 1, sk.dim, sk.bg);
    gfx_text(4, LCD_H - 19, T(S_CLKFT1), 1, sk.dim, sk.bg);
    gfx_text(4, LCD_H - 9, T(S_CLKFT2), 1, sk.dim, sk.bg);
    for (;;) {
        skin_wait(&e, 0);
        if (e.type == EV_KEY_DOWN) beep();
        if (e.type == EV_POWER) { MOD_POWER_REQ = 1; return 1; }
        if (e.type == EV_QUIT) return 1;
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        i = fsel;
        if (e.code == KEY_MENU) return 0;
        if (e.code == KEY_PLAY) { apply_clock(); return 0; }
        if (e.code == KEY_VOL) fsel = fsel + 1 == NF ? 0 : fsel + 1;
        else if (e.code == KEY_RIGHT || e.code == KEY_LEFT) {
            m = i == F_DAY ? cal_dim(fv[F_YEAR], fv[F_MONTH]) : f_max[i];
            if (e.code == KEY_RIGHT) fv[i] = fv[i] >= m ? f_min[i] : fv[i] + 1;
            else fv[i] = fv[i] <= f_min[i] ? m : fv[i] - 1;
            if (i == F_YEAR || i == F_MONTH) { fix_day(); field(F_DAY); }
        }
        field(i);
        if (fsel != i) field(fsel);
    }
}

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
    case R_CLOCK: {                                        /* the time now (no seconds) */
        char b[6];
        fmt_hm(b);
        gfx_text(VAL_X, y + 4, b, 1, fg, bg);
        break;
    }
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
    case R_CLOCK: return;
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
        if (e.type == EV_TIME) row(R_CLOCK);                 /* the clock row shows the time */
        if (e.type == EV_POWER) MOD_POWER_REQ = 1;           /* saved below, SHELL powers off */
        if (e.type == EV_QUIT || e.type == EV_POWER) break;
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        if (e.code == KEY_MENU) break;
        old = sel;
        if (e.code == KEY_RIGHT && sel + 1 < NROWS) sel++;
        else if (e.code == KEY_LEFT && sel) sel--;
        else if (e.code == KEY_PLAY && sel == R_CLOCK) {
            if (clock_edit()) break;                       /* (power key: leave) */
            screen();
        } else if (e.code == KEY_PLAY) change(1);
        else if (e.code == KEY_VOL) change(-1);
        if (sel != old) { row(old); row(sel); }
    }
    cfg_save();
    return 0;
}
