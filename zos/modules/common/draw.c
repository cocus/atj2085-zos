/* small shared drawing helpers for modules: include this file from one module source.
 *   icon16()  1-bit 16x16 icon (build: icons/*.bmp -> out/icons.h), set pixels only, scaled
 *   spk_icon() one speaker (mono) or two facing apart (stereo), 11x8 */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "regs.h"
#include "board.h"                             /* battery icon calibration */
#include "draw.h"

#ifndef DRAW_NO_ICON16
void icon16(const uint8_t *b, uint8_t x, uint8_t y, uint8_t sc, uint16_t fg)
{
    uint8_t r, c, c0;
    uint16_t v;
    for (r = 0; r < 16; r++, b += 2) {
        v = ((uint16_t)b[0] << 8) | b[1];
        for (c = 0; c < 16; ) {
            if (!(v & (0x8000u >> c))) { c++; continue; }
            for (c0 = c; c < 16 && (v & (0x8000u >> c)); c++) ;
            hal_lcd_window(x + c0 * sc, y + r * sc, (c - c0) * sc, sc);
            hal_lcd_fill(fg, (uint16_t)(c - c0) * sc * sc);
        }
    }
}
#endif

/* x, y, w, h rectangles: a speaker facing left (box + cone), then one facing right */
static const uint8_t spk_rects[] = { 2, 2, 2, 4,  1, 1, 1, 6,  0, 0, 1, 8,
                                     7, 2, 2, 4,  9, 1, 1, 6,  10, 0, 1, 8 };
void spk_icon(uint8_t x, uint8_t y, uint8_t stereo, uint16_t fg, uint16_t bg)
{
    const uint8_t *r = spk_rects;
    uint8_t n = stereo ? 6 : 3;
    gfx_fill(x, y, 11, 8, bg);
    if (!stereo) x += 3;                       /* mono: one speaker, centred */
    for (; n; n--, r += 4) gfx_fill(x + r[0], y + r[1], r[2], r[3], fg);
}

/* battery, BATT_ICON_W x BATT_ICON_H (19x9): the state the core keeps in HAL_BATT (level + USB power,
 * debounced, EV_BATT on a change; 0 = not sampled yet, the first half second: drawn empty). Fill
 * (13 px) by charge on the board's scale for battery / USB power (targets/<t>/board.h): red up to
 * 1/3, yellow up to 2/3, green above. USB power: a lightning bolt over it */
#ifndef DRAW_NO_BATT                           /* (MP3PLAY/DECPLAY: their own, see mp3play/ui.c) */
#define BATT_SEGS 13
/* x, y, w, h, colour (0 fg, 1 bg, 2 charge, 3 white): outline, inside, nub, charge (w = n), then the
 * bolt (USB power only) */
static const uint8_t batt_rects[] = { 0, 0, 17, 9, 0,  1, 1, 15, 7, 1,  17, 2, 2, 5, 0,  2, 2, 0, 5, 2,
    9, 1, 2, 1, 3,  8, 2, 2, 1, 3,  7, 3, 2, 1, 3,  5, 4, 7, 1, 3,  9, 5, 2, 1, 3,  8, 6, 2, 1, 3,
    7, 7, 2, 1, 3 };
SFR(DRAW_BATT, P_BATT);
SFR(DRAW_USBST, P_USB_STAT);
uint8_t batt_drawn;                            /* level + VBUS of the icon on screen */
uint8_t batt_state(void)                       /* the core's debounced state, else the ports */
{
    uint8_t s = HAL_BATT;
    if (!(s & HAL_BATT_VALID))                 /* not sampled yet, or a core without EV_BATT */
        s = (DRAW_BATT & BATT_LEVEL) | (DRAW_USBST & USBST_VBUS ? HAL_BATT_VBUS : 0);
    return s & (HAL_BATT_LEVEL | HAL_BATT_VBUS);
}
void batt_icon(uint8_t x, uint8_t y, uint16_t fg, uint16_t bg)
{
    uint8_t s = batt_state(), l, e, f, n, i;
    uint16_t c[4];
    const uint8_t *r = batt_rects;
    batt_drawn = s;
    l = s & HAL_BATT_LEVEL;
    if (s & HAL_BATT_VBUS) { e = BATT_EMPTY_USB; f = BATT_FULL_USB; }
    else { e = BATT_EMPTY_BAT; f = BATT_FULL_BAT; }
    n = l < e ? 0 : l >= f ? BATT_SEGS : (uint8_t)((uint8_t)((l - e + 1) * BATT_SEGS) / (uint8_t)(f - e + 1));
    c[0] = fg; c[1] = bg; c[3] = RGB(255, 255, 255);
    c[2] = n <= BATT_SEGS / 3 ? RGB(255, 60, 40) : n <= BATT_SEGS * 2 / 3 ? RGB(255, 200, 40) : RGB(80, 220, 80);
    for (i = s & HAL_BATT_VBUS ? sizeof batt_rects / 5 : 4; i; i--, r += 5)
        gfx_fill(x + r[0], y + r[1], r == batt_rects + 15 ? n : r[2], r[3], c[r[4]]);
}
#endif
