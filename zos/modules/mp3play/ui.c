/* MP3PLAY screen drawing (section _CODE2: ZRAM2 is full) - see main.c. */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../music/shared.h"
#include "mp3.h"
#include "../common/draw.h"
#include "board.h"
#define ZS_EXTERN
#include "strings.h"
#include "ui.h"
#include "regs.h"


char ti[ID3_W + 1], ar[ID3_W + 1];
uint8_t volmode, scrub;
uint16_t target;
char big[12];
uint16_t tot_shown;
uint8_t bar_w, vol_shown, paused_shown, mode_shown;

/* the 6-argument gfx calls cost ~25 bytes each in SDCC: everything goes through these */
void txt(uint8_t x, uint8_t y, const char *s, uint16_t fg) { gfx_text(x, y, s, 1, fg, C_BG); }
void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c) { gfx_fill(x, y, w, h, c); }
/* the inside of the title bar's battery icon (MUSIC drew the whole icon before chaining here): the
 * charge and the USB bolt, as common/draw.c batt_icon (no room for that one here) */
static const uint8_t bolt[] = { 9, 1, 2,  8, 2, 2,  7, 3, 2,  5, 4, 7,  9, 5, 2,  8, 6, 2,  7, 7, 2 };
/* called on every update() pass: redraws only when the level or USB power changed
 * (the core's debounced HAL_BATT; a core without EV_BATT leaves it 0: then the ports) */
uint8_t batt_shown = 0xFF;
void batt(void)
{
    uint8_t s = HAL_BATT, l, e = BATT_EMPTY_BAT, f = BATT_FULL_BAT, n, i;
    if (!ZCFG_BATT) return;
    if (!(s & HAL_BATT_VALID)) s = (BATT & BATT_LEVEL) | (USB_STAT & USBST_VBUS ? HAL_BATT_VBUS : 0);
    s &= HAL_BATT_LEVEL | HAL_BATT_VBUS;
    if (s == batt_shown) return;
    batt_shown = s;
    l = s & HAL_BATT_LEVEL;
    if (s & HAL_BATT_VBUS) { e = BATT_EMPTY_USB; f = BATT_FULL_USB; }
    n = l < e ? 0 : l >= f ? 13 : (uint8_t)((uint8_t)((l - e + 1) * 13) / (uint8_t)(f - e + 1));
    fill(BATT_ICON_X + 1, BATT_ICON_Y + 1, 15, 7, C_BAR);
    fill(BATT_ICON_X + 2, BATT_ICON_Y + 2, n, 5, n <= 4 ? RGB(255, 60, 40) : n <= 8 ? RGB(255, 200, 40) : RGB(80, 220, 80));
    if (s & HAL_BATT_VBUS)
        for (i = 0; i < sizeof bolt; i += 3)
            fill(BATT_ICON_X + bolt[i], BATT_ICON_Y + bolt[i + 1], bolt[i + 2], 1, RGB(255, 255, 255));
}
void bar(uint8_t x, uint8_t y, uint8_t w, const uint16_t *rows)
{
    uint8_t r;
    for (r = 0; r < BAR_H; r++) fill(x, y + r, w, 1, rows[r]);
}

#ifndef FOOTER_IN_MAIN                 /* section: wherever the player has room */
void footer(void)
{
    fill(0, LCD_H - 10, LCD_W, 10, C_BG);
    txt(4, LCD_H - 9, T(volmode ? S_FT_VOL : S_FT_PLAY), C_DIM);
    vol_shown = 0xFF;
}
#endif
#ifdef FMT_IN_UI
#include "fmtline.inc"
#endif



/* title / artist: SCR_W chars shown; longer ones scroll together (one step per 300 ms, a pause at
 * both ends) */
static uint8_t soff, spause;
static uint16_t stick;
static void line(uint8_t y, const char *s, uint16_t c)
{
    char b[SCR_W + 1];
    uint8_t n = (uint8_t)strlen(s), o = soff;
    if (n > SCR_W && o > n - SCR_W) o = n - SCR_W;
    if (n <= SCR_W) o = 0;
    memcpy(b, s + o, SCR_W);
    b[SCR_W] = 0;
    txt(4, y, b, c);
}
static void lines(void) { line(18, ti, C_SEL); line(30, ar, C_DIM); }

void scroll(void)
{
    uint8_t n = (uint8_t)strlen(ti), m = (uint8_t)strlen(ar);
    if (m > n) n = m;
    if (n <= SCR_W || (uint16_t)(hal_ticks() - stick) < SCR_TICKS) return;
    stick = hal_ticks();
    if (spause) { spause--; return; }
    if (++soff + SCR_W > n) { soff = 0; spause = SCR_PAUSE; }
    else if (soff + SCR_W == n) spause = SCR_PAUSE;
    lines();
}


/* everything below the title bar: MUSIC drew the bar before chaining here (music/shared.h); only the
 * battery icon is refreshed, as skin_title() draws it */
void screen(void)
{
    fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, C_BG);
    batt_shown = 0xFF;
    batt();
    soff = 0; spause = SCR_PAUSE; stick = hal_ticks();
    lines();
    txt(4, 78, "/", C_DIM);
    txt(4, 102, "VOL", C_DIM);
    bar(4, PB_Y, 120, S->bar[BAR_POFF]);
    memset(big, 0, sizeof big);                        /* (and kb) */
    tot_shown = 0xFFFF; bar_w = 0; paused_shown = mode_shown = 0xFF;
    footer();
}


void loading(void)                                     /* while a track opens / seeks */
{
    const char *s = T(S_LOADING);
    fill(0, 48, LCD_W, 28, C_BG);
    gfx_text((LCD_W - (uint8_t)strlen(s) * 12) / 2, 54, s, 2, C_SEL, C_BG);
}
