/* small shared drawing helpers for modules: include this file from one module source.
 *   icon16()  1-bit 16x16 icon (build: icons/*.bmp -> out/icons.h), set pixels only, scaled
 *   spk_icon() one speaker (mono) or two facing apart (stereo), 11x8 */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "regs.h"
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

/* battery from the free-running battery ADC (power.md), 15x7. Fill colour by charge: red up to
 * 1/3, yellow up to 2/3, green above. USB power: a lightning bolt over it (the ADC then reads the
 * charger, so the fill shows full) */
SFR(DRAW_BATT, P_BATT);
SFR(DRAW_USBST, P_USB_STAT);
#define BATT_SEGS 9
static const uint8_t bolt_rects[] = { 7, 1, 2, 1,  6, 2, 2, 1,  4, 3, 5, 1,  5, 4, 2, 1,  4, 5, 2, 1 };
void batt_icon(uint8_t x, uint8_t y, uint16_t fg, uint16_t bg)
{
    uint8_t l = DRAW_BATT & BATT_LEVEL, n, i;
    const uint8_t *r = bolt_rects;
    n = l < BATT_EMPTY ? 0 : l >= BATT_FULL ? BATT_SEGS :
        (uint8_t)((l - BATT_EMPTY + 1) * BATT_SEGS / (BATT_FULL - BATT_EMPTY + 1));
    gfx_fill(x, y, 13, 7, fg);                 /* outline */
    gfx_fill(x + 1, y + 1, 11, 5, bg);
    gfx_fill(x + 13, y + 2, 2, 3, fg);         /* nub */
    gfx_fill(x + 2, y + 2, n, 3, n <= BATT_SEGS / 3 ? RGB(255, 60, 40) :
                                 n <= BATT_SEGS * 2 / 3 ? RGB(255, 200, 40) : RGB(80, 220, 80));
    if (DRAW_USBST & USBST_VBUS)               /* charging */
        for (i = 0; i < sizeof bolt_rects / 4; i++, r += 4)
            gfx_fill(x + r[0], y + r[1], r[2], r[3], RGB(255, 255, 255));
}
