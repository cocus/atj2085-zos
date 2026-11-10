#ifndef ZOS_DRAW_H
#define ZOS_DRAW_H
#include <stdint.h>
void icon16(const uint8_t *bits32, uint8_t x, uint8_t y, uint8_t sc, uint16_t fg);
void spk_icon(uint8_t x, uint8_t y, uint8_t stereo, uint16_t fg, uint16_t bg);
void batt_icon(uint8_t x, uint8_t y, uint16_t fg, uint16_t bg);   /* BATT_ICON_W x BATT_ICON_H */
#define BATT_ICON_W 19
#define BATT_ICON_H 9
#define BATT_ICON_X (LCD_W - BATT_ICON_W - 2)   /* in the 16-row title bar (skin.c skin_batt) */
#define BATT_ICON_Y 3
#endif
