/* newbrec target ximpro: LCD driver (SSD1789A-compatible 132x132 RGB565 on CE3). Own code; the init
 * table and sequence are those of lisong_rebuild/analysis/boot.md 2.1 (what the stock boot record does),
 * with the scan mode of the zos screen orientation (BC 03: upright on the glass, visible rows 4..131). */
#include "regs.h"
#include "../../core/target.h"
#include "../../core/font5x7.h"
#include "logo.h"

SFR(EMHI, P_EMHI);
SFR(MFP, P_MFP);
SFR(GPCEN, P_GPC_EN);

void lcd_cmd(uint8_t c);
void lcd_data(uint8_t d);
void lcd_fill(uint16_t color, uint16_t count);
void lcd_colors(uint16_t fg, uint16_t bg);
void lcd_glyph(const uint8_t *cols);

/* LCD_WAIT_DIV: divide the controller power-up waits (build option; 1 = the stock values, which are
 * the only ones known to work: there is no evidence shorter waits are safe) */
#ifndef LCD_WAIT_DIV
#define LCD_WAIT_DIV 1
#endif
#define W(ms) 0, ((ms) / 4)          /* wait record: ms in units of 4 ms */
static const uint8_t init_tab[] = {
    1, 0xD1, W(200),                 /* oscillator on */
    1, 0x94, 2, 0x20, 0x0B, W(200),  /* sleep out, power control */
    3, 0x81, 0x21, 0x07, W(200),     /* contrast */
    5, 0xF3, 0x90, 0x05, 0x60, 0x84, 2, 0xFB, 0x02, 3, 0xF2, 0x08, 0x50, W(200),
    4, 0xF7, 0x28, 0x0E, 0x05,
    1, 0xA7,
    4, 0xCA, 0x00, 0x1F, 0x00,
    4, 0xBC, 0x03, 0x01, 0x00,       /* data control: scan mode */
    2, 0xBB, 0x01,
    4, 0xF1, 0xE0, 0x00, 0x00,
    1, 0x94,
    3, 0x15, 0x00, 0x83,             /* column window 0..131 */
    3, 0x75, 0x00, 0x83,             /* row window 0..131 */
    2, 0x44, 0x04,
    2, 0x82, 0x02,
    1, 0xAF,                         /* display on */
    0xFF
};

static void window(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    lcd_cmd(LCD_CASET); lcd_data(x); lcd_data(x + w - 1);
    lcd_cmd(LCD_PASET); lcd_data(y + LCD_ROW0); lcd_data(y + LCD_ROW0 + h - 1);
    lcd_cmd(LCD_RAMWR);
}

void lcd_init(void)
{
    const uint8_t *p = init_tab;
    uint8_t n;
    EMHI = EMHI_CE3_LCD;                 /* the window 0x8000 is the LCD bus from now on */
    MFP = 0x09;                          /* reset released, backlight off */
    GPCEN = (GPCEN & ~0x40) | GPC_LCD_A0;   /* A0 pin = output */
    tgt_delay_ms(10 / LCD_WAIT_DIV + 1);
    MFP &= ~MFP_LCD_RESET;
    tgt_delay_ms(10 / LCD_WAIT_DIV + 1);
    MFP |= MFP_LCD_RESET;
    tgt_delay_ms(100 / LCD_WAIT_DIV);
    while ((n = *p++) != 0xFF) {
        if (!n) { tgt_delay_ms((uint16_t)(*p++) * 4 / LCD_WAIT_DIV); continue; }
        lcd_cmd(*p++);
        while (--n) lcd_data(*p++);
    }
    lcd_cmd(LCD_CASET); lcd_data(0); lcd_data(131);      /* whole 132x132 GRAM black */
    lcd_cmd(LCD_PASET); lcd_data(0); lcd_data(131);
    lcd_cmd(LCD_RAMWR);
    lcd_fill(C_BG, 132 * 132);
}

void lcd_backlight(uint8_t on)
{
    if (on) MFP |= MFP_BACKLIGHT; else MFP &= ~MFP_BACKLIGHT;
}

void lcd_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c)
{
    window(x, y, w, h);
    lcd_fill(c, (uint16_t)w * h);
}

void lcd_text(uint8_t x, uint8_t y, const char *s, uint16_t fg, uint16_t bg)
{
    uint8_t c;
    lcd_colors(fg, bg);
    while ((c = *s++) != 0) {
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 90) c = '?';
        window(x, y, 6, 8);
        lcd_glyph(font5x7[c - 32]);
        x += 6;
    }
}

void lcd_num(uint8_t x, uint8_t y, uint32_t v, uint16_t fg, uint16_t bg)
{
    char t[11];
    uint8_t i = 10;
    t[10] = 0;
    do { t[--i] = '0' + (uint8_t)(v % 10); v /= 10; } while (v);
    lcd_text(x, y, t + i, fg, bg);
}

void lcd_logo(uint8_t x, uint8_t y)
{
    uint8_t r, c, m;
    uint16_t col;
    window(x, y, LOGO_W, LOGO_H);
    for (r = 0; r < LOGO_H; r++) {
        col = logo_col[r];
        for (c = 0; c < LOGO_W; c++) {
            m = logo_mask[r][c >> 3] & (0x80 >> (c & 7));
            lcd_data((m ? col : C_BG) >> 8);
            lcd_data(m ? (uint8_t)col : (uint8_t)C_BG);
        }
    }
}
