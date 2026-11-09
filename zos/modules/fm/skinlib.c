#include "../common/skin.c"
#include "../common/draw.c"
#include "../common/vu.c"
#include "../common/beep.c"

/* scaled glyph drawn column by column (runs of equal pixels as filled rectangles), pump() between
 * columns: gfx_char's scaled path writes pixel by pixel (~2 ms at scale 3), longer than the ADC
 * FIFO lasts while USB OUT streams (~375 us) */
void pump(void);                           /* main.c */
void big_char(uint8_t x, uint8_t y, char c, uint8_t sc, uint16_t fg, uint16_t bg)
{
    uint8_t col, bits, r, r0, on;
    if (c < 32 || c > 90) c = '?';
    for (col = 0; col < FONT_W; col++) {
        bits = col < 5 ? font5x7[c - 32][col] : 0;
        for (r = 0; r < FONT_H; ) {
            on = (bits >> r) & 1;
            for (r0 = r; r < FONT_H && ((bits >> r) & 1) == on; r++) ;
            hal_lcd_window(x + col * sc, y + r0 * sc, sc, (r - r0) * sc);
            hal_lcd_fill(on ? fg : bg, (uint16_t)(r - r0) * sc * sc);
        }
        pump();
    }
}
