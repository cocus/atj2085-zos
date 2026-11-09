/* zos graphics: rectangles and text streamed straight into the LCD (its RAM is the framebuffer) */
#ifndef ZOS_GFX_H
#define ZOS_GFX_H
#include <stdint.h>
#include "../hal/hal.h"
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define FONT_W 6
#define FONT_H 8
void gfx_fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c);
void gfx_char(uint8_t x, uint8_t y, char ch, uint8_t scale, uint16_t fg, uint16_t bg);
void gfx_text(uint8_t x, uint8_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg);
void gfx_num(uint8_t x, uint8_t y, uint16_t n, uint8_t scale, uint16_t fg, uint16_t bg);
#endif
