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

#ifndef SKIN_NO_TIME                              /* (a module can opt out when it has no room) */
#define TM_FG sk.fg
#define TM_BG sk.bar
#define TM_ROWS 10                                /* rows 2..11 of the bar are restored under the time */
static uint16_t tm_bg[TM_ROWS];
#include "skintime.h"
/* the TITLE.ZI colour of those rows at the middle of the time slot (one pixel column; the bar is a
 * vertical gradient), read once per skin_title(): no disk access on EV_TIME. No image: the theme bar */
static void tm_sample(void)
{
    fat_file_t f;
    uint8_t hdr[5], px[2], r;
    uint16_t o;
    for (r = 0; r < TM_ROWS; r++) tm_bg[r] = C_BAR;
    if (!tm_x || !skin_open("TITLE   ZI ", &f) || fat_read(&f, hdr, 5) != 5 || hdr[3] < TM_ROWS + 2 ||
        tm_x + TM_W / 2 >= hdr[2])
        return;
    for (r = 0; r < TM_ROWS; r++) {
        o = (uint16_t)(r + 2) * hdr[2] + tm_x + TM_W / 2;
        if (!fat_seek(&f, 5 + (uint32_t)o * 2) || fat_read(&f, px, 2) != 2) return;
        tm_bg[r] = ((uint16_t)px[0] << 8) | px[1];
    }
}
#else
#define tm_place(a, b, c)
#define tm_sample()
#define tm_draw()
#endif

void skin_title(const char *title)
{
    uint8_t w = (uint8_t)strlen(title) * FONT_W, t0 = 4;
    if (!skin_draw("TITLE   ZI ", 0, 0, 0)) ui_header(title);     /* (no skin: the theme bar, 12 rows) */
    else {
        t0 = (LCD_W - w) / 2;
        skin_text(t0, 3, title, sk.fg);
    }
    tm_place(t0, t0 + w, (ZCFG_BATT ? BATT_ICON_X - 3 : LCD_W - 2) - 5 * FONT_W);
    tm_sample();
    skin_batt();
    tm_draw();
}

/* the title bar's HH:MM only (EV_TIME: the minute changed) */
void skin_time(void)
{
    tm_draw();
}

/* the title bar's battery icon only (EV_BATT: the level or USB power changed) */
void skin_batt(void)
{
    if (ZCFG_BATT) batt_icon(BATT_ICON_X, BATT_ICON_Y, sk.fg, sk.bar);
}

/* EV_BATT (core), or on the 2 Hz tick a change the core did not report (a core without EV_BATT):
 * USB power plugged / unplugged, or the level moved by 2+ steps from what is drawn */
static void batt_check(const event_t *e)
{
    uint8_t s, d;
    if (e->type == EV_TIME) { skin_time(); return; }
    if (e->type == EV_SECOND_HALF) {
        s = batt_state();
        d = (s & HAL_BATT_LEVEL) - (batt_drawn & HAL_BATT_LEVEL);
        if ((int8_t)d < 0) d = -d;
        if (!((s ^ batt_drawn) & HAL_BATT_VBUS) && d < 2) return;
    } else if (e->type != EV_BATT) return;
    skin_batt();
}

uint8_t skin_wait(event_t *e, uint16_t timeout)
{
    uint8_t r = ev_wait(e, timeout);
    if (r) batt_check(e);
    return r;
}

uint8_t skin_get(event_t *e)
{
    uint8_t r = ev_get(e);
    if (r) batt_check(e);
    return r;
}

void skin_footer(const char *text)
{
    gfx_fill(0, LCD_H - 10, LCD_W, 10, sk.bg);
    gfx_text(4, LCD_H - 9, text, 1, sk.dim, sk.bg);
}
