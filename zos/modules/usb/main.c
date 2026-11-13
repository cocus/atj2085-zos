/* USB: launcher of the zos USB functions (page module, full skin). << >> choose, PLAY starts, MENU
 * leaves, VOL toggles the CPU clock 24/48 MHz for the session (48 MHz: mass-storage writes off).
 *   MASS STORAGE   -> USBDISK.ZM  (the user disk; "(RW)" = writes on, the default)
 *   SERIAL CONSOLE -> USBCON.ZM   (CDC-ACM debug command line)
 *   USB AUDIO      -> USBAUDIO.ZM (microphone / FM radio / speaker; its own source menu)
 * This module draws the chrome (title, the running function's row, CPU line, footer) and chains to the
 * function module, which only redraws its status lines and chains back (common/usb/usbsh.h).
 * Woken from standby by a PC (SHELL sets autostart): straight into mass storage, no key needed.
 * Design: lisong_rebuild/analysis/zos_usb.md. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/usb/usbsh.h"
#include "regs.h"
#include "strings.h"


#define NFN 3                                    /* FN_MSC, FN_CDC (common/usb/usb.h), audio */
#define FN_AUDIO 2
static const char *const fmod[NFN] = { "USBDISK ZM ", "USBCON  ZM ", "USBAUDIOZM " };
#define ROW_Y(i) (18 + (i) * 12)
#define CPU_Y 84
#define CAB_Y 72
static uint8_t vbus;

static const char *fname(uint8_t i)
{
    return i == 0 ? T(USH->wr ? S_MSC_RW : S_MSC_RO) : T(i == 1 ? S_CDC : S_AUDIO);
}

static void row(uint8_t i, uint8_t sel, uint16_t fg)
{
    uint8_t y = ROW_Y(i);
    if (sel) {
        if (!skin_draw("SELBAR  ZI ", 0, y, 0)) gfx_fill(0, y, LCD_W, 11, sk.sel_bot);
        skin_text(6, y + 2, fname(i), fg);
    } else {
        gfx_fill(0, y, LCD_W, 11, sk.bg);
        gfx_text(6, y + 2, fname(i), 1, fg, sk.bg);
    }
}

static void line(uint8_t y, uint8_t label, const char *text, uint16_t c)
{
    gfx_fill(0, y, LCD_W, 10, sk.bg);
    gfx_text(4, y + 1, T(label), 1, sk.dim, sk.bg);
    gfx_text(46, y + 1, text, 1, c, sk.bg);
}

static void cpu_line(void) { line(CPU_Y, S_CPU, USH->fast ? "48 MHZ" : "24 MHZ", sk.fg); }

static void cable_line(void)
{
    vbus = (USB_STAT & USBST_VBUS) != 0;
    line(CAB_Y, S_USB, T(vbus ? S_CABLE : S_NOCABLE), vbus ? sk.fg : sk.dim);
}

static void menu(void)
{
    uint8_t i;
    gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - 10 - SKIN_TITLE_H, sk.bg);
    for (i = 0; i < NFN; i++) row(i, i == USH->fn, i == USH->fn ? sk.sel_text : sk.fg);
    cable_line();
    cpu_line();
    skin_footer(T(S_FT_MENU));
}

/* chrome for the function module, then chain to it; 0 = its file is missing (message shown) */
static uint16_t go(void)
{
    fat_file_t d;
    fat_dirent_t e;
    event_t ev;
    uint8_t f = USH->fn;
    fat_root(&d);
    if (!fat_find(&d, fmod[f], &e)) {
        line(CAB_Y, S_USB, T(S_NOMOD), C_SEL);
        skin_wait(&ev, 200);
        return 0;
    }
    if (f == FN_AUDIO) {                         /* USBAUDIO: its own source menu below the title */
        skin_title(T(S_AUDIO));
        gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - SKIN_TITLE_H, sk.bg);
    } else {                                     /* MSC / CDC: status lines from y 46 */
        gfx_fill(0, SKIN_TITLE_H, LCD_W, LCD_H - 10 - SKIN_TITLE_H, sk.bg);
        row(f, 1, C_OK);
        cpu_line();
        skin_footer(T(S_FT_RUN));
    }
    memcpy(USH->next, fmod[f], 12);
    return MOD_CHAIN;
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t autostart = USH->magic == USB_MAGIC && USH->autostart, back = USH->magic == USB_MAGIC && USH->back;
    uint16_t r;
    if (!back) {                                 /* from the shell: defaults (as the old USB module) */
        memset((uint8_t *)USH + 12, 0, sizeof *USH - 12);
        USH->magic = USB_MAGIC;
        USH->wr = 1;
        USH->rdmode = 1;
    }
    USH->back = USH->autostart = 0;
    skin_load();
    if (!back || USH->fn == FN_AUDIO) {          /* (MSC / CDC left the title as it was) */
        gfx_fill(0, 0, LCD_W, SKIN_TITLE_H, sk.bg);
        skin_title(T(S_TITLE));
    }
    if (autostart && (USB_STAT & USBST_VBUS)) {     /* woken by a PC: mass storage */
        USH->fn = 0;
        if ((r = go())) return r;
    }
    menu();
    for (;;) {
        if (!skin_wait(&e, 50)) {                  /* cable in / out */
            if (((USB_STAT & USBST_VBUS) != 0) != vbus) cable_line();
            continue;
        }
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_LEFT || e.code == KEY_RIGHT) {
            row(USH->fn, 0, sk.fg);
            USH->fn = (USH->fn + (e.code == KEY_RIGHT ? 1 : NFN - 1)) % NFN;
            row(USH->fn, 1, sk.sel_text);
        } else if (e.code == KEY_VOL) {          /* 48 MHz: NAND writes off (timing unverified) */
            USH->fast ^= 1;
            if (USH->fast) { USH->wr = 0; row(0, !USH->fn, USH->fn ? sk.fg : sk.sel_text); }
            cpu_line();
        } else if (e.code == KEY_PLAY) {
            if ((r = go())) return r;
            menu();
        }
    }
    USH->magic = 0;                              /* next start: defaults */
    return 0;
}
