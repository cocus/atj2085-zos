#ifndef ZOS_DRAW_H
#define ZOS_DRAW_H
#include <stdint.h>
void icon16(const uint8_t *bits32, uint8_t x, uint8_t y, uint8_t sc, uint16_t fg);
void spk_icon(uint8_t x, uint8_t y, uint8_t stereo, uint16_t fg, uint16_t bg);
void batt_icon(uint8_t x, uint8_t y, uint16_t fg, uint16_t bg);   /* 15x7 */
#endif
