/* zos skins, module side (draft) - see skin.h. Images stream straight from the disk to the LCD in
 * 512-byte pieces (the .ZI pixel format is the LCD's); DMA2 is the next step (LCDDMA: 5-9x faster). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "../../apps/theme.h"
#include "../../ui/font5x7.h"
#include "skin.h"
#include "draw.h"
#include "../../core/config.h"

skin_colors_t sk = SKIN_DEFAULTS;
uint8_t skin_ok;
static fat_file_t skdir;
static uint8_t skbuf[512];

static uint8_t skin_open(const char *name11, fat_file_t *f)
{
    fat_dirent_t e;
    fat_file_t d = skdir;
    if (!skin_ok || !fat_find(&d, name11, &e)) return 0;
    fat_open(f, &e);
    return 1;
}

static void skin_find(void)
{
    fat_dirent_t e;
    fat_file_t f;
    skin_ok = 0;
    if (!fat_mount()) return;
    fat_root(&f);
    if (!fat_find(&f, "SKIN       ", &e) || !(e.attr & FAT_ATTR_DIR)) return;
    fat_open(&skdir, &e);
    skin_ok = 1;
    if (skin_open("COLORS  ZT ", &f)) fat_read(&f, &sk, sizeof sk);
}

uint8_t skin_load(void)
{
    skin_find();
    skc.c = sk;                                  /* for skinlite.c modules and MP3PLAY */
    skc.ok = 1;
    return skin_ok;
}

uint8_t skin_draw(const char *name11, uint8_t x, uint8_t y, uint8_t frame)
{
    fat_file_t f;
    uint8_t hdr[5];
    uint16_t px, n;
    if (!skin_open(name11, &f) || fat_read(&f, hdr, 5) != 5 || hdr[0] != 'Z' || hdr[1] != 'I' ||
        frame >= hdr[4])
        return 0;
    px = (uint16_t)hdr[2] * hdr[3];
    if (frame && !fat_seek(&f, 5 + (uint32_t)px * 2 * frame)) return 0;
    hal_lcd_window(x, y, hdr[2], hdr[3]);
    while (px) {
        n = px > 256 ? 256 : px;
        if (fat_read(&f, skbuf, n * 2) != n * 2) return 0;
        hal_lcd_blit(skbuf, n);
        px -= n;
    }
    return 1;
}

#ifdef SKIN_ICON_BIG
/* one frame of an icon strip, each pixel drawn as an sc x sc block (magenta-free: the strip's
 * transparent pixels were already flattened to the skin bg by rbskin.py) */
uint8_t skin_icon_big(const char *name11, uint8_t frame, uint8_t x, uint8_t y, uint8_t sc)
{
    fat_file_t f;
    uint8_t hdr[5], r, c;
    uint16_t row;
    if (!skin_open(name11, &f) || fat_read(&f, hdr, 5) != 5 || hdr[0] != 'Z' || hdr[1] != 'I' ||
        frame >= hdr[4] || hdr[2] > 128)
        return 0;
    row = (uint16_t)hdr[2] * 2;
    if (frame && !fat_seek(&f, 5 + (uint32_t)row * hdr[3] * frame)) return 0;
    for (r = 0; r < hdr[3]; r++) {
        if (fat_read(&f, skbuf, row) != row) return 0;
        for (c = 0; c < hdr[2]; c++) {
            hal_lcd_window(x + c * sc, y + r * sc, sc, sc);
            hal_lcd_fill(((uint16_t)skbuf[c * 2] << 8) | skbuf[c * 2 + 1], sc * sc);
        }
    }
    return 1;
}
#endif

/* 5x7 font, only the set pixels: one vertical run per column segment */
void skin_text(uint8_t x, uint8_t y, const char *s, uint16_t fg)
{
    uint8_t c, col, bits, r, r0;
    for (; *s && x < LCD_W - 5; s++, x += FONT_W) {
        c = *s;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 90) c = '?';
        for (col = 0; col < 5; col++) {
            bits = font5x7[c - 32][col];
            for (r = 0; r < 8; ) {
                if (!(bits & (1 << r))) { r++; continue; }
                for (r0 = r; r < 8 && (bits & (1 << r)); r++) ;
                hal_lcd_window(x + col, y + r0, 1, r - r0);
                hal_lcd_fill(fg, r - r0);
            }
        }
    }
}

void skin_title(const char *title)
{
    uint8_t w = (uint8_t)strlen(title) * FONT_W;
    if (!skin_draw("TITLE   ZI ", 0, 0, 0)) ui_header(title);     /* (no skin: the theme bar, 12 rows) */
    else
        skin_text((LCD_W - w) / 2, 3, title, sk.fg);
    skin_batt();
}

/* the title bar's battery icon only (EV_BATT: the level or USB power changed) */
void skin_batt(void)
{
    if (ZCFG_BATT) batt_icon(BATT_ICON_X, BATT_ICON_Y, sk.fg, sk.bar);
}

uint8_t skin_wait(event_t *e, uint16_t timeout)
{
    uint8_t r = ev_wait(e, timeout);
    if (r && e->type == EV_BATT) skin_batt();
    return r;
}

uint8_t skin_get(event_t *e)
{
    uint8_t r = ev_get(e);
    if (r && e->type == EV_BATT) skin_batt();
    return r;
}

void skin_footer(const char *text)
{
    gfx_fill(0, LCD_H - 10, LCD_W, 10, sk.bg);
    gfx_text(4, LCD_H - 9, text, 1, sk.dim, sk.bg);
}
