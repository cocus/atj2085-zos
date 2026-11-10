/* MODULES: lists *.ZM on the user disk, loads the chosen one into a RAM page and runs it */
#include <stdint.h>
#include <string.h>
#include "../hal/hal.h"
#include "../core/event.h"
#include "../core/module.h"
#include "../core/fat.h"
#include "theme.h"
#include "apps.h"

#define MAXMODS 20
#define ROWS 8
/* the names live in the page modules' ZRAM1 scratch (HAL_MODRAM, below the games' scores at its
 * last 32 bytes), not in core RAM: a module run may overwrite them, so the disk is read again after
 * every run */
static __at(HAL_MODRAM_END - 32 - MAXMODS * 12) char names[MAXMODS][12];   /* absolute: no storage */
static uint8_t nmods, mounted, top;

static void name83(char *t, const char *n)
{
    uint8_t i, j = 0;
    for (i = 0; i < 8 && n[i] != ' ' && n[i]; i++) t[j++] = n[i];
    t[j] = 0;
}

static void draw_item(uint8_t i, uint8_t sel)
{
    char t[9];
    uint16_t bg = sel ? C_BAR : C_BG;
    uint8_t y = 16 + (i - top) * 12;
    name83(t, names[i]);
    gfx_fill(0, y, LCD_W, 11, bg);
    gfx_text(10, y + 2, t, 1, sel ? C_SEL : C_FG, bg);
}

static void draw(uint8_t sel, const char *msg)
{
    uint8_t i;
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    ui_header("MODULES");
    for (i = top; i < nmods && i < top + ROWS; i++) draw_item(i, i == sel);
    if (!nmods) gfx_text(4, 20, "NO .ZM FILES", 1, C_DIM, C_BG);
    if (msg) gfx_text(4, LCD_H - 20, msg, 1, C_SEL, C_BG);
    ui_footer("PLAY:RUN  MENU:BACK");
}

static void scan(void)
{
    fat_file_t d;
    fat_dirent_t e;
    nmods = top = 0;
    fat_root(&d);
    while (nmods < MAXMODS && fat_readdir(&d, &e)) {
        if ((e.attr & FAT_ATTR_DIR) || memcmp(e.name + 8, "ZM ", 3)) continue;
        memcpy(names[nmods], e.name, 11);
        names[nmods++][11] = 0;
    }
}

void app_modules(void)
{
    event_t e;
    mod_hdr_t hdr;
    uint8_t sel = 0, old, err;
    const char *msg = 0;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    ui_header("MODULES");
    gfx_text(4, 20, "READING DISK...", 1, C_DIM, C_BG);
    if (!mounted) mounted = fat_mount();
    if (!mounted) msg = "NO DISK"; else scan();
    draw(sel, msg);
    for (;;) {
        ev_wait(&e, 0);
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        if (e.code == KEY_MENU) return;
        old = sel;
        if (e.code == KEY_RIGHT && sel + 1 < nmods) sel++;
        if (e.code == KEY_LEFT && sel) sel--;
        if (sel != old) {
            if (sel < top || sel >= top + ROWS) draw(sel, 0);
            else { draw_item(old, 0); draw_item(sel, 1); }
        }
        if (e.code == KEY_PLAY && e.type == EV_KEY_DOWN && nmods) {
            gfx_text(4, LCD_H - 20, "LOADING...", 1, C_SEL, C_BG);
            err = mod_load(names[sel], hal_pages[0], &hdr);
            /* a module may hand over to another one: return MOD_CHAIN with the 8.3 name at
             * MOD_CHAIN_NAME (e.g. MUSIC browser <-> MP3PLAY player) */
            while (err == MOD_OK && mod_run(hal_pages[0], &hdr) == MOD_CHAIN)
                err = mod_load(MOD_CHAIN_NAME, hal_pages[0], &hdr);
            msg = err == MOD_OK ? 0 : mod_errstr[err];
            scan();                              /* (names: see above) */
            if (sel >= nmods) sel = 0;
            draw(sel, msg);
        }
    }
}
