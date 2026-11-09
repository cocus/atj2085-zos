/* USB: zos USB device module. LEFT/RIGHT choose the function, PLAY connects (the player appears on
 * the PC as soon as a cable is in; plugging/unplugging is handled while connected), PLAY or MENU
 * disconnects, MENU (disconnected) leaves. VOL toggles the CPU clock 24/48 MHz (disconnected).
 *   MASS STORAGE: the user disk, read-only (Windows: removable drive, write-protected) unless
 *                 the console's "wr on" enabled writes (24 MHz only; the menu shows "(RW)")
 *   SERIAL CONSOLE: CDC-ACM COM port with a debug command line (cdc.c)
 *   USB AUDIO: companion USB Audio Class 1 microphone or radio module
 * Design: lisong_rebuild/analysis/zos_usb.md. */
#include <stdint.h>
#include <string.h>
#include "usb.h"
#include "../../core/module.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "../common/clock.h"
#include "../common/ftlw.h"

static uint8_t fn, stop, ab;
static uint16_t tdraw;

static const char *const fname[3] = { "MASS STORAGE (RO)", "SERIAL CONSOLE", "USB AUDIO" };
static const char *const sname[] = { "OFF", "NO CABLE", "CHARGER?", "ATTACHED", "BUS RESET",
                                     "ADDRESSED", "CONFIGURED" };

static void line(uint8_t y, const char *label, const char *text, uint16_t c)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_text(4, y + 1, label, 1, C_DIM, C_BG);
    gfx_text(46, y + 1, text, 1, c, C_BG);
}

static void numline(uint8_t y, const char *label, uint16_t a, const char *label2, uint16_t b)
{
    gfx_fill(0, y, LCD_W, 10, C_BG);
    gfx_text(4, y + 1, label, 1, C_DIM, C_BG);
    gfx_num(46, y + 1, a, 1, C_FG, C_BG);
    if (label2) { gfx_text(82, y + 1, label2, 1, C_DIM, C_BG); gfx_num(102, y + 1, b, 1, C_FG, C_BG); }
}

static void menu(uint8_t running)
{
    uint8_t i, y, first = running ? fn : 0, count = running ? 1 : 3;
    gfx_fill(0, 16, LCD_W, 48, C_BG);
    for (i = first; i < first + count; i++) {
        y = running ? 16 : 16 + i * 12;
        gfx_fill(0, y, LCD_W, 11, i == fn ? C_SELBG : C_BG);
        gfx_text(6, y + 2, i == FN_MSC && msc_wr ? "MASS STORAGE (RW)" : fname[i], 1, i == fn ? (running ? C_OK : C_SELFG) : C_FG, i == fn ? C_SELBG : C_BG);
    }
    line(84, "CPU", clk_fast ? "48 MHZ" : "24 MHZ", C_FG);
    skin_footer(running ? "PLAY/MENU: OFF" : "<< >> SELECT PLAY:GO");
}

/* last SCSI command on the LCD: "LAST 28 OK  RD FLAT" (hex opcode) */
static void hexop(void)
{
    char t[18];
    static const char hx[] = "0123456789ABCDEF";
    t[0] = hx[msc_lastop >> 4]; t[1] = hx[msc_lastop & 15]; t[2] = ' ';
    t[3] = msc_ncmd ? (msc_laststatus ? 'F' : 'O') : '-';
    t[4] = msc_ncmd ? (msc_laststatus ? 'L' : 'K') : '-';
    strcpy(t + 5, msc_rdmode ? "  RD PP" : "  RD FLAT");
    line(108, "LAST", t, msc_laststatus ? C_SEL : C_FG);
}

static void status(void)
{
    uint8_t s = usb_state;
    if (s <= US_CHARGER) {
        gfx_fill(0, 46, LCD_W, 36, C_BG);
        line(72, "USB", sname[s], C_SEL);
        return;
    }
    line(46, "USB", sname[s], s == US_CONFIGURED ? C_OK : s <= US_CHARGER ? C_SEL : C_FG);
    if (usb_fn == FN_MSC) {
        if (msc_ejected) line(58, "DISK", "EJECTED", C_SEL);
        else numline(58, "CMDS", msc_ncmd, "MB", (uint16_t)((msc_last + 1) >> 11));
        numline(70, "SECT", msc_nsec, 0, 0);
        hexop();
    } else if (usb_fn == FN_CDC) {
        line(58, "PORT", cdc_dtr ? "OPEN (DTR)" : "CLOSED", cdc_dtr ? C_OK : C_FG);
        numline(70, "RX", cdc_nrx, "DRP", cdc_ndrop);
    }
    numline(96, "SETUP", usb_nsetup, "RST", usb_nreset);     /* bring-up diagnostics */
}

static void run(void)
{
    event_t e;
    usb_nsetup = usb_nreset = 0;
    msc_ncmd = msc_nsec = 0;
    cdc_nrx = cdc_ndrop = 0;
    msc_ejected = 0;
    stop = 0;
    menu(1);
    usb_start(fn);
    status();
    tdraw = hal_ticks();
    ab = setjmp(usb_jb);                    /* every wait leaves to here on reset / unplug / MENU */
    if (ab && fn == FN_MSC) msc_abort_note(ab);    /* logged if a SCSI command was cut off */
    if (ab == AB_STOP) stop = 1;
    else if (ab) usb_recover(ab);
    while (!stop) {
        usb_service();
        if (fn == FN_MSC) msc_poll();
        else if (fn == FN_CDC) cdc_poll();
        if (usb_state == US_NOVBUS) msc_ejected = 0;
        if (usb_dirty && (uint16_t)(hal_ticks() - tdraw) >= 25) {
            usb_dirty = 0;
            status();
            tdraw = hal_ticks();
        }
        while (ev_get(&e))
            if (e.type == EV_KEY_DOWN && (e.code == KEY_MENU || e.code == KEY_PLAY)) stop = 1;
    }
    usb_stop();
    ftlw_flush();                           /* written data in place before anything else */
    status();
    menu(0);
}

static void run_audio(void)
{
    static const char module[] = "USBAUDIOZM ";
    static const char *const errors[] = {
        "OK", "NOT FOUND", "READ ERROR", "NOT A MODULE", "TOO BIG", "NEWER API"
    };
    mod_hdr_t hdr;
    uint8_t err = mod_load(module, PAGE_IPM_H, &hdr);
    if (err == MOD_OK) mod_run(PAGE_IPM_H, &hdr);
    else {
        gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
        skin_title("USB AUDIO");
        gfx_text(4, 24, "LOAD FAILED", 1, C_SEL, C_BG);
        gfx_text(4, 38, err < sizeof errors / sizeof errors[0] ? errors[err] : "LOAD ERROR",
                 1, C_FG, C_BG);
        skin_footer("PLAY/MENU: BACK");
        for (;;) {
            event_t e;
            ev_wait(&e, 0);
            if (e.type == EV_KEY_DOWN && (e.code == KEY_MENU || e.code == KEY_PLAY)) break;
        }
    }
}

/* "total sectors" of the FAT boot sector (LBA 0), 0 = no valid boot sector */
static uint32_t boot_total(void)
{
    uint8_t *b = ZR->sec;
    uint32_t n;
    if (!hal_blk_read(0, b) || b[510] != 0x55 || b[511] != 0xAA) return 0;
    n = b[0x13] | ((uint16_t)b[0x14] << 8);
    if (!n) n = b[0x20] | ((uint16_t)b[0x21] << 8) | ((uint32_t)b[0x22] << 16) | ((uint32_t)b[0x23] << 24);
    return n;
}

uint16_t mod_main(void)
{
    event_t e;
    uint16_t cap;
    /* capacity as the stock USB disk reports it (msc.c): RAM 0x008A = flash size in 128 KB
     * units from BREC's flash detect (hal_blk_init reads the same word) */
    cap = *(volatile uint16_t *)0x008A;
    if (cap < 0x100 || cap > 0x4000) cap = 0x1EC0;          /* this player's 1 GB NAND */
    msc_last = ((uint32_t)cap << 8) - 0x8000 - 0x210;
    /* cross-check with the FAT boot sector (formatted by Windows through the stock driver, so its
     * sector count = stock last LBA + 1); if they disagree, trust the volume (ver/log show both) */
    msc_boot_total = boot_total();
    if (msc_boot_total > 0x1000 && msc_boot_total - 1 > msc_last) msc_last = msc_boot_total - 1;
    usb_fn = fn = FN_MSC;
    skin_load();
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("USB");
    status();
    menu(0);
    for (;;) {
        ev_wait(&e, 0);
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_LEFT || e.code == KEY_RIGHT) {
            usb_fn = fn = (fn + (e.code == KEY_RIGHT ? 1 : 2)) % 3;
            status(); menu(0);
        }
        else if (e.code == KEY_VOL) {       /* 48 MHz: NAND writes off (timing unverified) */
            if (clk_fast) cpu_normal(); else { cpu_fast(); msc_wr = 0; }
            menu(0);
        }
        else if (e.code == KEY_PLAY) {
            if (fn == FN_MSC || fn == FN_CDC) run();
            else run_audio();
            status(); menu(0);
        }
    }
    cpu_normal();
    usb_stop_clock(0);
    return 0;
}
