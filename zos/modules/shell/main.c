/* SHELL: the zos launcher (started by the core, apps/home.c). A carousel of top-level items like
 * the stock main menu (<< >> pick, PLAY open); GAMES and TOOLS open lists of the modules found on
 * the disk. Running a module = return MOD_CHAIN with its name; when it returns, the core starts
 * SHELL again, which comes back where it was (state in SHELL_STATE). REBOOT returns MOD_REBOOT.
 * First run after boot: settings from ZOS.CFG (modules/common/cfg.c); REBOOT saves them first.
 * PLAY acts on release: held 2 s it is the power button instead (core EV_POWER): settings saved,
 * then the stock standby (standby.s); a module that gets EV_POWER leaves with MOD_POWER_REQ set
 * and SHELL powers off when it comes back. Wake-up: PLAY held 1 s (or a PC on USB: USB module).
 * Icons: icons/<NAME>.bmp (build -> icons.h, icons/README.md), by category / module name, DEFAULT otherwise. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "../../core/config.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/beep.h"
#include "../common/draw.h"
#include "../common/cfg.h"
#include "icons.h"
#include "strings.h"
#include "regs.h"

SFR(GPC_D, P_GPC_D);
SFR(WDT, P_WDT);

/* ---- what is on the disk -------------------------------------------------------------------- */
enum { CAT_MUSIC, CAT_FM, CAT_GAMES, CAT_TOOLS, CAT_USB, CAT_SETTINGS, CAT_REBOOT, NCAT, CAT_HIDDEN = 0xFF };
static const uint8_t cat_str[NCAT] = { S_MUSIC, S_FM, S_GAMES, S_TOOLS, S_USB, S_SETTINGS, S_REBOOT };
static const char *const cat_icon[NCAT] = { "MUSIC", "FM", "GAMES", "TOOLS", "USB", "SETTINGS", "REBOOT" };

/* modules with a fixed place; anything else on the disk goes to TOOLS */
static const struct { char name[9]; uint8_t cat; } known[] = {
    { "MUSIC", CAT_MUSIC }, { "FM", CAT_FM }, { "USB", CAT_USB }, { "SETTINGS", CAT_SETTINGS },
    { "SNAKE", CAT_GAMES }, { "SHELL", CAT_HIDDEN }, { "MP3PLAY", CAT_HIDDEN } };

#define MAXMOD 24
typedef struct { char name11[12]; char label[9]; uint8_t cat; } mod_t;
static mod_t mods[MAXMOD];
static uint8_t nmods;

static uint8_t category(const char *label)
{
    uint8_t i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(known[i].name, label)) return known[i].cat;
    return CAT_TOOLS;
}

static void scan(void)
{
    fat_file_t d;
    fat_dirent_t e;
    uint8_t i;
    nmods = 0;
    fat_root(&d);
    while (nmods < MAXMOD && fat_readdir(&d, &e)) {
        if ((e.attr & FAT_ATTR_DIR) || memcmp(e.name + 8, "ZM ", 3)) continue;
        memcpy(mods[nmods].name11, e.name, 11);
        mods[nmods].name11[11] = 0;
        for (i = 0; i < 8 && e.name[i] != ' '; i++) mods[nmods].label[i] = e.name[i];
        mods[nmods].label[i] = 0;
        mods[nmods].cat = category(mods[nmods].label);
        if (mods[nmods].cat != CAT_HIDDEN) nmods++;
    }
}

static int8_t find_cat(uint8_t cat)          /* first module of a single-module category */
{
    uint8_t i;
    for (i = 0; i < nmods; i++) if (mods[i].cat == cat) return i;
    return -1;
}

static const uint8_t *icon_of(const char *name)
{
    uint8_t i;
    for (i = 0; i < NZICONS; i++) if (!strcmp(zicons[i].name, name)) return zicons[i].bits;
    for (i = 0; i < NZICONS; i++) if (!strcmp(zicons[i].name, "DEFAULT")) return zicons[i].bits;
    return zicons[0].bits;
}

/* ---- persistent state (SHELL_STATE, kept while modules run) --------------------------------- */
typedef struct { uint8_t magic, top, in_list, sel, first; } shell_state_t;
static __at(SHELL_STATE) shell_state_t st;
#define SHELL_MAGIC 0x5F

/* top-level items present: modules only if they exist, GAMES/TOOLS if non-empty */
static uint8_t items[NCAT], nitems;
static void build_items(void)
{
    uint8_t c, i;
    nitems = 0;
    for (c = 0; c < NCAT; c++) {
        if (c != CAT_REBOOT) {
            for (i = 0; i < nmods && mods[i].cat != c; i++) ;
            if (i == nmods) continue;
        }
        items[nitems++] = c;
    }
}

/* ---- drawing ----------------------------------------------------------------------------------- */
#define TILE   40
#define TOP_Y  18
#define ROW_H  18
#define ROWS   5


/* the carousel: title bar once, then only the middle part changes with << >> */
static void carousel_item(void)
{
    uint8_t c = items[st.top], i, w, x;
    const char *nm = T(cat_str[c]);
    gfx_fill(0, 18, LCD_W, 86, sk.bg);
    x = (LCD_W - TILE) / 2;
    gfx_fill(x, 22, TILE, TILE / 2, sk.sel_top);
    gfx_fill(x, 22 + TILE / 2, TILE, TILE / 2, sk.sel_bot);
    icon16(icon_of(cat_icon[c]), x + 4, 26, 2, sk.sel_text);
    w = (uint8_t)strlen(nm) * FONT_W * 2;
    gfx_text((LCD_W - w) / 2, 70, nm, 2, sk.fg, sk.bg);
    if (nitems > 1) {                            /* circular: both ways always lead somewhere */
        gfx_text(8, 34, "<", 2, sk.sel_bot, sk.bg);
        gfx_text(LCD_W - 18, 34, ">", 2, sk.sel_bot, sk.bg);
    }
    x = (LCD_W - nitems * 8) / 2;                /* position dots */
    for (i = 0; i < nitems; i++) gfx_fill(x + i * 8 + 2, 94, 4, 4, i == st.top ? sk.sel_bot : sk.dim);
}

static void carousel(void)
{
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title("ZOS");
    carousel_item();
    skin_footer(T(S_FT_TOP));
}

/* the list of a category (GAMES / TOOLS): indices into mods[] */
static uint8_t list_idx[MAXMOD], nlist;

static void list_row(uint8_t i)
{
    uint8_t y = TOP_Y + (i - st.first) * ROW_H, s = i == st.sel;
    mod_t *m = &mods[list_idx[i]];
    if (s) {
        gfx_fill(0, y, LCD_W, ROW_H / 2, sk.sel_top);
        gfx_fill(0, y + ROW_H / 2, LCD_W, ROW_H - ROW_H / 2, sk.sel_bot);
    } else gfx_fill(0, y, LCD_W, ROW_H, sk.bg);
    icon16(icon_of(m->label), 4, y + 1, 1, s ? sk.sel_text : sk.fg);
    skin_text(26, y + 5, m->label, s ? sk.sel_text : sk.fg);
}

static void list(void)
{
    uint8_t i;
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title(T(cat_str[items[st.top]]));
    for (i = st.first; i < nlist && i < st.first + ROWS; i++) list_row(i);
    skin_footer(T(S_FT_LIST));
}

static void build_list(void)
{
    uint8_t i, c = items[st.top];
    nlist = 0;
    for (i = 0; i < nmods; i++) if (mods[i].cat == c) list_idx[nlist++] = i;
}

static uint16_t run(uint8_t m)
{
    memcpy((char *)MOD_CHAIN_NAME, mods[m].name11, 12);
    return MOD_CHAIN;
}

/* ---- power off: the stock standby ------------------------------------------------------------ */
#define STBY_RAM (HAL_MODRAM_BASE + 2 * ZCFG_FILE_SIZE)    /* ZRAM1, behind cfg.c's two buffers */
#define WAKE_USB 2                                         /* standby.s */
extern const uint8_t standby_blob[], standby_end[];

static void lcd_cmd(uint8_t c)                             /* as the core's lcd.s (CE3 stays selected) */
{
    GPC_D &= ~GPC_LCD_A0;
    *(volatile uint8_t *)LCD_DATA = c;
    GPC_D |= GPC_LCD_A0;
}

static void wait(uint8_t n)                                /* n ticks, interrupts on, light untouched */
{
    uint16_t t = hal_ticks();
    while ((uint8_t)(hal_ticks() - t) < n) WDT |= WDT_CLR;
}

/* the stock sequence around STANDBY.AP's loop: display off + 250 ms, sleep, display on + 20 ms,
 * sleep out + 250 ms (UI_LCD_Sleep). The light: off in standby.s, on with the waking press.
 * Returns the module to chain to (USB when woken by a PC), else -1 */
static int8_t power_off(void)
{
    uint8_t why;
    MOD_POWER_REQ = 0;
    cfg_save();                                            /* settings + resume point */
    hal_backlight(0);                                      /* light off first (stock order) */
    hal_watchdog();
    lcd_cmd(LCD_DISOFF);
    wait(25);
    memcpy((void *)STBY_RAM, standby_blob, standby_end - standby_blob);
    why = ((uint8_t (*)(void))STBY_RAM)();
    lcd_cmd(LCD_DISON);
    wait(2);
    lcd_cmd(LCD_SLPOUT);
    wait(25);
    if (why != WAKE_USB) return -1;
    hal_backlight(1);                                      /* no key press to switch it on */
    return find_cat(CAT_USB);
}

static void redraw(void)
{
    if (st.in_list) list(); else carousel();
}

/* << >> move; a fresh press past either end wraps around (holding stops at the ends) */
static uint8_t step(uint8_t i, uint8_t n, uint8_t key, uint8_t wrap)
{
    if (key == KEY_RIGHT) return i + 1 < n ? i + 1 : wrap ? 0 : i;
    if (key == KEY_LEFT) return i ? i - 1 : wrap && n ? n - 1 : i;
    return i;
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t old, c, play = 0;
    int8_t m;
    if (!fat_mount()) return MOD_REBOOT;
    if (ZCFG->magic != ZCFG_MAGIC) { cfg_load(); hal_lcd_contrast(ZCFG->contrast); }
    skin_load();
    scan();
    build_items();
    if (st.magic != SHELL_MAGIC || st.top >= nitems) { memset(&st, 0, sizeof st); st.magic = SHELL_MAGIC; }
    if (st.in_list) { build_list(); if (st.sel >= nlist) st.sel = st.first = 0; }
    redraw();
    if (MOD_POWER_REQ) e.type = EV_POWER; else e.type = EV_NONE;
    for (;;) {
        if (e.type != EV_POWER) ev_wait(&e, 0);
        if (e.type == EV_KEY_DOWN) beep();             /* key beep (SETTINGS) */
        if (e.type == EV_POWER) {
            e.type = EV_NONE;
            play = 0;
            if ((m = power_off()) >= 0) return run(m);
            redraw();
            continue;
        }
        /* PLAY opens on release, so that holding it (power off) opens nothing */
        if (e.code == KEY_PLAY) {
            if (e.type == EV_KEY_DOWN) play = 1;
            if (e.type != EV_KEY_UP || !play) continue;
            play = 0;
            e.type = EV_KEY_DOWN;
        }
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        if (!st.in_list) {                       /* ---- carousel */
            old = st.top;
            st.top = step(st.top, nitems, e.code, e.type == EV_KEY_DOWN);
            if (st.top != old) { carousel_item(); continue; }
            if (e.type != EV_KEY_DOWN || e.code != KEY_PLAY) continue;
            c = items[st.top];
            if (c == CAT_REBOOT) { cfg_save(); return MOD_REBOOT; }
            if (c == CAT_GAMES || c == CAT_TOOLS) {
                st.in_list = 1; st.sel = st.first = 0;
                build_list(); list();
                continue;
            }
            if ((m = find_cat(c)) >= 0) return run(m);
        } else {                                 /* ---- module list */
            old = st.sel;
            st.sel = step(st.sel, nlist, e.code, e.type == EV_KEY_DOWN);
            if (st.sel != old) {
                if (st.sel < st.first || st.sel >= st.first + ROWS) {
                    st.first = st.sel < st.first ? st.sel : st.sel - ROWS + 1;
                    list();
                } else { list_row(old); list_row(st.sel); }
                continue;
            }
            if (e.type != EV_KEY_DOWN) continue;
            if (e.code == KEY_MENU) { st.in_list = 0; carousel(); }
            else if (e.code == KEY_PLAY && nlist) return run(list_idx[st.sel]);
        }
    }
}
