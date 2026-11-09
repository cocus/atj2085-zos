#include "gfx.h"
#include "font5x7.h"

void gfx_fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c)
{
    if (!w || !h || x >= LCD_W || y >= LCD_H) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    hal_lcd_window(x, y, w, h);
    hal_lcd_fill(c, (uint16_t)w * h);
}

/* one glyph = one window, streamed row by row (no per-pixel windows) */
void gfx_char(uint8_t x, uint8_t y, char ch, uint8_t scale, uint16_t fg, uint16_t bg)
{
    const uint8_t *g;
    uint8_t row, col, sy, sx, bit;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch > 'Z') ch = '?';
    g = font5x7[ch - 32];
    if (x + FONT_W * scale > LCD_W || y + FONT_H * scale > LCD_H) return;
    hal_lcd_window(x, y, FONT_W * scale, FONT_H * scale);
    if (scale == 1) {                       /* fast path: asm streamer */
        hal_lcd_colors(fg, bg);
        hal_lcd_glyph6x8(g);
        return;
    }
    for (row = 0; row < FONT_H; row++) {
        bit = 1 << row;
        for (sy = 0; sy < scale; sy++) {
            for (col = 0; col < FONT_W; col++) {
                uint16_t c = (col < 5 && (g[col] & bit)) ? fg : bg;
                for (sx = 0; sx < scale; sx++) hal_lcd_pixel(c);
            }
        }
    }
}

void gfx_text(uint8_t x, uint8_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg)
{
    while (*s) { gfx_char(x, y, *s++, scale, fg, bg); x += FONT_W * scale; }
}

void gfx_num(uint8_t x, uint8_t y, uint16_t n, uint8_t scale, uint16_t fg, uint16_t bg)
{
    char buf[6];
    int8_t i = 5;
    buf[5] = 0;
    do { buf[--i] = '0' + n % 10; n /= 10; } while (n && i);
    gfx_text(x, y, buf + i, scale, fg, bg);
}
