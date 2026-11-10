/* USB function module frame (USBDISK, USBCON; include from the module's main.c after usb.h): the USB
 * launcher (USB.ZM) drew the chrome (title, the running function's row, CPU line, footer) before
 * chaining here; this file runs one session and only redraws the status lines (y 46..117), then
 * chains back to the launcher. The module provides:
 *   static void fn_status(void);   its own lines (USB state line + function lines)
 *   static void fn_poll(void);     msc_poll / cdc_poll (+ extra checks)
 *   static void fn_abort(uint8_t ab);   a wait left by longjmp (MSC: log a cut-off command)
 *   static void fn_end(void);      after usb_stop (MSC: written data in place) */
#include <stdint.h>
#include <string.h>
#include "usb.h"
#include "../../../core/module.h"
#include "../../../core/event.h"
#include "../../../apps/theme.h"
#include "../skcol.h"
#include "../clock.h"

static uint8_t stop, ab;
static uint16_t tdraw;
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

static void fn_status(void);
static void fn_poll(void);
static void fn_abort(uint8_t why);
static void fn_end(void);

/* the common part of the status: 0 = no host (only the cable state shown), 1 = draw the rest */
static uint8_t state_line(void)
{
    uint8_t s = usb_state;
    if (s <= US_CHARGER) {
        gfx_fill(0, 46, LCD_W, 26, C_BG);
        gfx_fill(0, 94, LCD_W, 24, C_BG);
        line(72, "USB", sname[s], C_SEL);
        return 0;
    }
    line(46, "USB", sname[s], s == US_CONFIGURED ? C_OK : C_FG);
    numline(96, "SETUP", usb_nsetup, "RST", usb_nreset);      /* bring-up diagnostics */
    return 1;
}

static uint16_t fn_back(void)                   /* chain back to the launcher */
{
    USH->back = 1;
    memcpy(USH->next, USB_ZM, 12);
    return MOD_CHAIN;
}

/* one session: USB on (the PC sees the player while a cable is in; plugging / unplugging is handled
 * here), PLAY or MENU off, then back to the launcher */
static uint16_t fn_session(uint8_t fn)
{
    event_t e;
    if (USH->magic != USB_MAGIC) return fn_back();     /* not started by the launcher */
    if (USH->fast) cpu_fast();                          /* (the launcher turned writes off) */
    usb_start(fn);
    fn_status();
    tdraw = hal_ticks();
    ab = setjmp(usb_jb);                    /* every wait leaves to here on reset / unplug / MENU */
    if (ab) fn_abort(ab);
    if (ab == AB_STOP) stop = 1;
    else if (ab) usb_recover(ab);
    while (!stop) {
        usb_service();
        fn_poll();
        if (usb_dirty && (uint16_t)(hal_ticks() - tdraw) >= 25) {
            usb_dirty = 0;
            fn_status();
            tdraw = hal_ticks();
        }
        while (ev_get(&e))
            if (e.type == EV_KEY_DOWN && (e.code == KEY_MENU || e.code == KEY_PLAY)) stop = 1;
    }
    usb_stop();
    fn_end();
    cpu_normal();
    usb_stop_clock(0);
    return fn_back();
}
