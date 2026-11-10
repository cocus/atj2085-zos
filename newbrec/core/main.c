/* newbrec M1: boot screen + menu + USB raw NAND disk. Board independent: everything board specific is
 * behind target.h (newbrec/target/ximpro). The menu table is hardcoded (BOOT.CFG is a later option).
 *
 * Boot policy of M1: there is nothing to boot yet (STOCK / ZOS / OS2 start in M2), so the menu is always
 * shown; nothing starts without PLAY. M2 will boot the default entry at once unless MENU is held. */
#include <stdint.h>
#include <string.h>
#include "target.h"
#include "usb.h"
#include "stockldr.h"

SFR(PAGEREG, P_PAGE);
SFR(CLKCTL2, P_CLKCTL2);

enum { A_USB, A_STOCK, A_ZOS, A_OS2 };
typedef struct { const char *label, *note; uint8_t act; } item_t;
static const item_t items[] = {
    { "USB",   "RAW NAND DISK", A_USB },
    { "STOCK", "BLK 2/3 COPY",    A_STOCK },
    { "ZOS",   "NOT IN M1",     A_ZOS },
    { "OS2",   "NOT IN M1",     A_OS2 },
};
#define NITEMS (sizeof items / sizeof items[0])
#define Y_MENU 50
#define ROW_H  14

static void clear_body(void) { lcd_rect(0, 46, LCD_W, LCD_H - 46, C_BG); }

static void header(const char *sub)
{
    lcd_rect(0, 0, LCD_W, 46, C_BG);
    lcd_logo(6, 5);
    lcd_text(34, 9, "ZOS BOOT", C_FG, C_BG);
    lcd_text(34, 21, sub, C_ACC, C_BG);
    lcd_text(34, 33, "NEWBREC M1", C_DIM, C_BG);
}

static void footer(const char *s)
{
    lcd_rect(0, 116, LCD_W, 12, C_BG);
    lcd_text(4, 118, s, C_DIM, C_BG);
}

static void draw_item(uint8_t i, uint8_t sel)
{
    uint8_t y = Y_MENU + i * ROW_H;
    uint16_t bg = i == sel ? C_SELBG : C_BG;
    lcd_rect(0, y, LCD_W, ROW_H - 2, bg);
    lcd_text(6, y + 2, items[i].label, C_FG, bg);
    lcd_text(48, y + 2, items[i].note, i == sel ? C_FG : C_DIM, bg);
}

static void draw_menu(uint8_t sel)
{
    uint8_t i;
    header("BOOT MENU");
    clear_body();
    for (i = 0; i < NITEMS; i++) draw_item(i, sel);
    footer("<< >> MOVE   PLAY OK");
}

/* a key press (edge, debounced); idle polling kicks the watchdog through the delay */
static uint8_t wait_key(void)
{
    static uint8_t last;
    uint8_t k;
    for (;;) {
        k = tgt_key();
        if (!k) last = 0;
        else if (k != last) {
            tgt_delay_ms(20);
            if (tgt_key() == k) { last = k; return k; }
        }
        tgt_delay_ms(5);
    }
}

static void message(const char *s)
{
    lcd_rect(0, 105, LCD_W, 10, C_BG);
    lcd_text(6, 106, s, C_WARN, C_BG);
}

/* ---- USB mode ---------------------------------------------------------------------------------- */
static const char *const sname[] = { "NO CABLE", "CHARGER?", "ATTACHED", "BUS RESET", "ADDRESSED", "CONFIGURED" };
static const char hx[] = "0123456789ABCDEF";

static void line(uint8_t y, const char *label, const char *text, uint16_t c)
{
    lcd_rect(0, y, LCD_W, 10, C_BG);
    lcd_text(4, y + 1, label, C_DIM, C_BG);
    lcd_text(40, y + 1, text, c, C_BG);
}

static void numline(uint8_t y, const char *label, uint32_t v, const char *label2, uint32_t v2)
{
    lcd_rect(0, y, LCD_W, 10, C_BG);
    lcd_text(4, y + 1, label, C_DIM, C_BG);
    lcd_num(40, y + 1, v, C_FG, C_BG);
    if (label2) { lcd_text(70, y + 1, label2, C_DIM, C_BG); lcd_num(104, y + 1, v2, C_FG, C_BG); }
}

static void usb_status(uint32_t blocks)
{
    char t[8];
    line(50, "USB", sname[usb_state], usb_state == US_CONFIGURED ? C_OK : C_FG);
    if (msc_ejected) line(62, "DISK", "EJECTED", C_WARN);
    else numline(62, "BLOCKS", blocks, "MB", blocks >> 3);
    numline(74, "READ", msc_nrd, "WRITE", msc_nwr);
    numline(86, "CMDS", msc_ncmd, 0, 0);
    t[0] = hx[msc_lastop >> 4]; t[1] = hx[msc_lastop & 15]; t[2] = ' ';
    t[3] = msc_ncmd ? (msc_laststatus ? 'F' : 'O') : '-';
    t[4] = msc_ncmd ? (msc_laststatus ? 'L' : 'K') : '-';
    t[5] = 0;
    line(98, "LAST", t, msc_laststatus ? C_WARN : C_FG);
}

static void usb_mode(void)
{
    uint8_t id[4], ab, stop = 0, seen = 0;
    uint32_t blocks;
    uint16_t td;
    header("USB RAW NAND");
    clear_body();
    footer("MENU = BACK");
    nand_id(id);
    blocks = tgt_nand_blocks(id);
    if (!blocks) {
        message("NAND NOT SUPPORTED");
        lcd_text(6, 62, "ID", C_DIM, C_BG);
        for (ab = 0; ab < 4; ab++) {
            char h2[3];
            h2[0] = hx[id[ab] >> 4]; h2[1] = hx[id[ab] & 15]; h2[2] = 0;
            lcd_text(30 + ab * 18, 62, h2, C_FG, C_BG);
        }
        tgt_delay_ms(2500);
        return;
    }
    msc_set_geometry(blocks);
    usb_start();
    usb_status(blocks);
    td = tgt_ticks();
    ab = setjmp(usb_jb);                  /* every wait leaves here on reset / unplug / MENU */
    if (ab == AB_STOP) stop = 1;
    else if (ab) usb_recover(ab);
    while (!stop) {
        usb_service();
        msc_poll();
        if (usb_state >= US_ATTACHED) seen = 1;
        if (seen && usb_state == US_NOVBUS) stop = 1;       /* cable pulled after a session: menu */
        if (usb_dirty && (uint16_t)(tgt_ticks() - td) >= 25) {
            usb_dirty = 0;
            usb_status(blocks);
            td = tgt_ticks();
        }
        if (tgt_key() == KEY_MENU) stop = 1;
    }
    usb_stop();
}

/* STOCK: the loader (stockldr.s) runs from the ZRAM2 window; it returns only if no stock boot record
 * copy is found in NAND block 2 / 3 (then the menu goes on), else it never returns */
static void stock_boot(void)
{
    message("LOADING STOCK");
    PAGEREG = PAGE_ZRAM2;
    CLKCTL2 |= CLK2_BANKS;
    memcpy((void *)PAGE_BASE, stock_ldr, STOCK_LDR_LEN);
    if (((uint8_t (*)(void))PAGE_BASE)()) message("NO STOCK IN BLOCK 2/3");
}

void main(void)
{
    uint8_t sel = 0, k;
    tgt_init();
    lcd_init();
    draw_menu(sel);
    lcd_backlight(1);
    for (;;) {
        k = wait_key();
        if (k == KEY_LEFT || k == KEY_RIGHT) {
            uint8_t old = sel;
            if (k == KEY_LEFT) sel = sel ? sel - 1 : NITEMS - 1;
            else sel = sel + 1 < NITEMS ? sel + 1 : 0;
            draw_item(old, sel);
            draw_item(sel, sel);
            message("");
        } else if (k == KEY_PLAY) {
            if (items[sel].act == A_USB) { usb_mode(); draw_menu(sel); }
            else if (items[sel].act == A_STOCK) stock_boot();
            else message("NOT IN M1");
        }
    }
}
