/* FILES: browse the user disk with zos' FAT (long names, folders). PLAY on a folder enters it,
 * PLAY on a file reads it completely (checksum + speed), MENU goes up / back. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/fat.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "strings.h"

#define ROWS 9
#define TOP_Y (SKIN_TITLE_H + 1)
#define MAXE 64
static fat_dirent_t ents[MAXE];          /* lives in the module page */
static uint8_t n, top, sel, depth;
static fat_file_t dirs[8];               /* folder stack: [depth] = current */
static uint8_t buf[512];

static void label(const fat_dirent_t *e, char *t)
{
    uint8_t i, j = 0;
    if (e->lfn[0]) { strncpy(t, e->lfn, 20); t[20] = 0; }
    else {
        for (i = 0; i < 8 && e->name[i] != ' '; i++) t[j++] = e->name[i];
        if (e->name[8] != ' ') { t[j++] = '.'; for (i = 8; i < 11 && e->name[i] != ' '; i++) t[j++] = e->name[i]; }
        t[j] = 0;
    }
    for (i = 0; t[i]; i++) if (t[i] >= 'a' && t[i] <= 'z') t[i] -= 32;    /* font is upper case */
}

static void load(void)
{
    fat_file_t d = dirs[depth];
    n = 0;
    while (n < MAXE && fat_readdir(&d, &ents[n]))
        if (!(ents[n].name[0] == '.' && ents[n].name[1] == ' ')) n++;     /* hide "." */
    top = sel = 0;
}

/* as the MUSIC browser: the skin's selection bar and folder icon (or plain colours + ">") */
static void row(uint8_t i)
{
    char t[24];
    uint8_t y = TOP_Y + (i - top) * 11, s = i == sel, dir = ents[i].attr & FAT_ATTR_DIR;
    uint16_t bg = s ? C_SELBG : C_BG;
    label(&ents[i], t);
    t[19] = 0;                                     /* 12 + 19 x 6 px */
    if (s && skin_draw("SELBAR  ZI ", 0, y, 0)) {  /* gradient bar, transparent text */
        if (dir) skin_draw("ICONSEL ZI ", 1, y + 1, ICON_FOLDER);
        skin_text(12, y + 2, t, C_SELFG);
        return;
    }
    gfx_fill(0, y, LCD_W, 11, bg);
    if (dir && !skin_draw("ICONS   ZI ", 1, y + 1, ICON_FOLDER))
        gfx_text(2, y + 2, ">", 1, s ? C_SELFG : C_SEL, bg);
    gfx_text(12, y + 2, t, 1, s ? C_SELFG : C_FG, bg);
}

static void draw(void)
{
    uint8_t i;
    gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, C_BG);
    for (i = top; i < n && i < top + ROWS; i++) row(i);
    if (!n) gfx_text(4, TOP_Y + 4, T(S_EMPTY), 1, C_DIM, C_BG);
    skin_footer(T(S_FOOTER));
}

uint16_t sum_acc;                                  /* sum.s */
void sum_add(const uint8_t *p, uint16_t n);

static void readall(const fat_dirent_t *e)
{
    fat_file_t f;
    uint32_t total = 0;
    uint16_t sum, k, t0, dt;
    event_t ev;
    char t[24];
    gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, C_BG);
    label(e, t);
    gfx_text(4, 18, t, 1, C_SEL, C_BG);
    gfx_text(4, 30, T(S_READING), 1, C_DIM, C_BG);
    fat_open(&f, e);
    sum_acc = 0;
    t0 = hal_ticks();
    while ((k = fat_read(&f, buf, sizeof buf))) {
        sum_add(buf, k);
        total += k;
        hal_watchdog();
    }
    dt = hal_ticks() - t0;
    sum = sum_acc;
    gfx_fill(0, 30, LCD_W, 10, C_BG);
    gfx_text(4, 30, "SIZE", 1, C_DIM, C_BG);  gfx_num(40, 30, (uint16_t)(e->size >> 16), 1, C_FG, C_BG);
    gfx_num(80, 30, (uint16_t)e->size, 1, C_FG, C_BG);
    gfx_text(4, 42, "READ", 1, C_DIM, C_BG);  gfx_num(40, 42, (uint16_t)(total >> 16), 1, total == e->size ? C_OK : C_SEL, C_BG);
    gfx_num(80, 42, (uint16_t)total, 1, total == e->size ? C_OK : C_SEL, C_BG);
    gfx_text(4, 54, "SUM16", 1, C_DIM, C_BG); gfx_num(40, 54, sum, 1, C_FG, C_BG);
    gfx_text(4, 66, "X10MS", 1, C_DIM, C_BG); gfx_num(40, 66, dt, 1, C_FG, C_BG);
    gfx_text(4, 78, "KB/S", 1, C_DIM, C_BG);
    gfx_num(40, 78, dt ? (uint16_t)(((total >> 10) * 100UL) / dt) : 0, 1, C_OK, C_BG);
    skin_footer(T(S_BACK));
    do ev_wait(&ev, 0); while (ev.type != EV_KEY_DOWN);
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t old;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(T(S_TITLE));
    if (!fat_mount()) { gfx_text(4, 20, T(S_NODISK), 1, C_SEL, C_BG); ev_wait(&e, 200); return 1; }
    depth = 0;
    fat_root(&dirs[0]);
    load();
    draw();
    for (;;) {
        ev_wait(&e, 0);
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        old = sel;
        if (e.code == KEY_RIGHT && sel + 1 < n) sel++;
        if (e.code == KEY_LEFT && sel) sel--;
        if (sel != old) {
            if (sel < top || sel >= top + ROWS) { top = sel < top ? sel : sel - ROWS + 1; draw(); }
            else { row(old); row(sel); }
        }
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) {
            if (!depth) return 0;
            depth--; load(); draw();
        } else if (e.code == KEY_PLAY && n) {
            if (ents[sel].attr & FAT_ATTR_DIR) {
                if (ents[sel].name[0] == '.') { if (depth) depth--; }      /* ".." */
                else if (depth < 7) fat_open(&dirs[++depth], &ents[sel]);
                load();
            } else readall(&ents[sel]);
            skin_title(T(S_TITLE));
            draw();
        }
    }
}
