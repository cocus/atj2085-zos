/* Minimal graphics for the 128x128 colour LCD (SSD1789A-compatible, RGB565). */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define SCR_W 128
#define SCR_H 128
void lcd_lock(void);
void lcd_unlock(void);
void lcd_cmd(uint8_t c);
void lcd_data(uint8_t d);
void lcd_fill_words(uint16_t color, uint16_t count);
void gfx_fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color);
void gfx_char(uint8_t x, uint8_t y, char c, uint8_t scale, uint16_t fg, uint16_t bg);
void gfx_text(uint8_t x, uint8_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg);
void gfx_num(uint8_t x, uint8_t y, uint16_t n, uint8_t scale, uint16_t fg, uint16_t bg);
#endif
