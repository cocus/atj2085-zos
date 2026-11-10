/* newbrec: the interface the board-independent core uses. One implementation per board in
 * newbrec/target/<board>/ (first and only: ximpro). Nothing here depends on zos. */
#ifndef NEWBREC_TARGET_H
#define NEWBREC_TARGET_H
#include <stdint.h>

#define LCD_W 128
#define LCD_H 128
enum { KEY_NONE, KEY_MENU, KEY_LEFT, KEY_RIGHT, KEY_PLAY };

/* colours RGB565 */
#define C_BG    0x0000
#define C_FG    0xFFFF
#define C_DIM   0x8410
#define C_ACC   0x4DDF
#define C_OK    0x5FEA
#define C_WARN  0xFDA0
#define C_SELBG 0x0A5F

/* system */
void tgt_init(void);                    /* clocks, watchdog, key pins, RTC tick: call first */
void tgt_wdt(void);                     /* kick the watchdog */
void tgt_delay_ms(uint16_t ms);
void tgt_reset(void);                   /* watchdog reset (never returns) */
uint16_t tgt_ticks(void);               /* 10 ms ticks, advances while polled */
uint8_t tgt_key(void);                  /* key held now (KEY_*, 0 = none) */
uint8_t tgt_vbus(void);                 /* USB cable present */
void tgt_usb_clock(void);               /* 48 MHz clock for the USB engine */

/* display */
void lcd_init(void);                    /* reset + init table; screen black, backlight still off */
void lcd_backlight(uint8_t on);
void lcd_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c);
void lcd_text(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg);
void lcd_num(uint8_t x, uint8_t y, uint32_t v, uint16_t fg, uint16_t bg);
void lcd_logo(uint8_t x, uint8_t y);    /* the 20x36 emblem */

/* NAND (CE1), request block shared with nand.s */
typedef struct { uint8_t c0, c1, r0, r1, r2; uint16_t buf, len; } nand_req_t;
extern nand_req_t nreq;
uint8_t nand_id(uint8_t *id4);
uint8_t nand_read(void);
uint8_t nand_erase(void);
uint8_t nand_prog(void);
uint32_t tgt_nand_blocks(const uint8_t *id4);   /* 0 = chip not supported */

#endif
