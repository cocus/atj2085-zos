/* ZMAIN - minimal replacement for the stock MAIN.AP (the ACTOS shell): ZOS / USB DISK / ADFU /
 * POWER OFF. Flashed with `python homebrew/build.py zmain --as MAIN.AP --fw`.
 *
 * Safety: a USB cable plugged in at boot goes straight to USB DISK (usbdisk.ap), so the PC can
 * always reach the player (s1res, firmware upgrade) even if something else here breaks. A cable
 * plugged in while the menu is shown does the same (stock behaviour: message 0x91).
 * Dispatch as the stock MAIN (bank40 0x194B switch): usbdisk.ap returns RESULT_UPGRADE when the
 * PC asked for ADFU -> adfuf641.ap (this board's flash family, DRV_DetectUD = 1).
 * ExecAP reloads this AP from flash before it returns: globals come back with their image values,
 * so everything that must survive a call lives on the stack. */
#include <stdint.h>
#include <string.h>
#include "actos.h"
#include "gfx.h"
#include "fs.h"

#define LPAGE     0xC1              /* DSP program RAM lane: staging page (device-verified) */
#define PAGE_WIN  0x4000
#define STUB_PAR  0x7EF0            /* in the page, see stub.s */
#define STUB_CODE 0x7F00
#define LOAD_END  0x2C00            /* the app must end below FS_FAT16's cluster cache at 0x2C00 */

#define BG  RGB(255, 255, 255)
#define FG  RGB(0, 0, 0)
#define DIM RGB(120, 120, 120)
#define SEL RGB(255, 255, 255)
#define BAR RGB(60, 120, 220)
#define ERR RGB(200, 0, 0)

extern uint8_t stub_start[], stub_end[];
typedef struct { uint16_t dst; const void *src; uint16_t len; } xfer_t;
void page_put(uint8_t page, const xfer_t *x);
void page_enter(uint8_t page, uint16_t addr);
uint8_t os_page(void);
void lcd_lock(void);
void lcd_unlock(void);
void lcd_cmd(uint8_t c);
void lcd_data(uint8_t d);

/* stock MAIN boot (bank40 0x14AA-0x1527), repeated as is */
#define P_WDT        0x4E
#define WDT_LEN_MASK 0x70            /* bits 6-4 period */
#define WDT_LEN_5S6  0x30            /* 32 Hz / 5.6 s (stock) */
#define WDT_CLR      0x08
#define P_POWER      0x4F
#define POWER_STOCK  0xA5            /* VCC/VDD setting written by the stock MAIN */
__sfr __at(P_WDT) WDT;
__sfr __at(P_POWER) POWER;
#define DRV_MODE_RESIDENT 0          /* DRV_Install C: as the stock MAIN */
#define DRV_MODE_UI       1

#define LCD_DATCTL   0xBC            /* LCD controller: data control (scan direction) */
#define DATCTL_STOCK 0x02            /* the stock UI's mode, which gfx.c assumes */

/* the stock MAIN leaves the LCD in DATCTL_STOCK (UI driver + logo); we are MAIN now, and other
 * APs may change it, so set it at start and after every ExecAP */
static void lcd_mode(void)
{
    lcd_lock();
    lcd_cmd(LCD_DATCTL); lcd_data(DATCTL_STOCK); lcd_data(0x01); lcd_data(0x00);
    lcd_unlock();
}

static uint8_t sector[512];
static uint16_t launch_sp;

enum { M_ZOS, M_USB, M_ADFU, M_OFF, NITEMS };
static const char *const items[NITEMS] = { "ZOS", "USB DISK", "ADFU UPGRADE", "POWER OFF" };

static void draw(uint8_t sel, const char *msg)
{
    uint8_t i;
    lcd_mode();
    gfx_fill(0, 0, SCR_W, SCR_H, BG);
    gfx_fill(0, 0, SCR_W, 12, RGB(225, 228, 235));
    gfx_text(46, 2, "ZMAIN", 1, FG, RGB(225, 228, 235));
    for (i = 0; i < NITEMS; i++) {
        uint16_t bg = i == sel ? BAR : BG;
        gfx_fill(0, 18 + i * 16, SCR_W, 14, bg);
        gfx_text(10, 21 + i * 16, items[i], 1, i == sel ? SEL : FG, bg);
    }
    if (msg) gfx_text(4, 92, msg, 1, ERR, BG);
    gfx_text(4, 118, "<< >> MOVE  PLAY: GO", 1, DIM, BG);
}

static void put(uint16_t dst, const void *src, uint16_t len)
{
    xfer_t x; x.dst = dst; x.src = src; x.len = len;
    page_put(LPAGE, &x);
}

/* stage ZOS.AP in page LPAGE and enter it through the stub (as the GAME.AP launcher). zos never
 * returns (it reboots), so the stack handed to it is simply the current one, a bit lower. */
static const char *run_zos(void)
{
    uint16_t h, len, addr, entry, par[8];
    uint8_t i, nsect;
    drv_active_ud(0, 1);
    if (!drv_active_drive('C', 1) || !fs_init()) return "NO USER DISK";
    fs_cd(":");
    h = fs_fopen("ZOS     AP ");
    if (!h) return "NO ZOS.AP ON THE DISK";
    if (!fs_fread(sector, h) || sector[0] != 'P' || sector[2] != 'W' || sector[3] != 'G') goto bad;
    len   = sector[12] | (uint16_t)sector[13] << 8;
    addr  = sector[14] | (uint16_t)sector[15] << 8;
    entry = sector[0x1C] | (uint16_t)sector[0x1D] << 8;
    nsect = (uint8_t)((len + 511) / 512);
    if (addr != 0x0800 || !len || addr + (uint16_t)nsect * 512 > LOAD_END) goto bad;
    for (i = 0; i < 3; i++)                          /* rest of the 0x800-byte header */
        if (!fs_fread(sector, h)) goto bad;
    for (i = 0; i < nsect; i++) {
        if (!fs_fread(sector, h)) goto bad;
        put(PAGE_WIN + (uint16_t)i * 512, sector, 512);
        wdt_kick();
    }
    fs_fclose(h);
    __asm__("ld (_launch_sp), sp");
    par[0] = (uint16_t)nsect * 512;
    par[1] = entry;
    par[2] = launch_sp - 32;                         /* free stack below this frame */
    par[3] = par[4] = par[5] = par[6] = 0;
    par[7] = os_page();
    put(STUB_PAR, par, sizeof par);
    put(STUB_CODE, stub_start, stub_end - stub_start);
    page_enter(LPAGE, STUB_CODE);                    /* does not return */
bad:
    fs_fclose(h);
    return "ZOS.AP: BAD FILE";
}

/* system settings block, as the stock MAIN boot (bank40 0x1DF3): other APs (USBDISK, ...) read it;
 * if it is not valid, write the defaults with the fields the stock MAIN adjusts */
#define CV_MAGIC      0x00   /* u16 COMVAL_MAGIC */
#define CV_CONTRAST   0x08   /* u16 */
#define CV_F0F        0x0F   /* stock sets 2 */
#define CV_FLAGS      0x10   /* bit 5 moved to CV_F18 */
#define CV_FLAG5      0x20
#define CV_F14        0x14
#define CV_F15        0x15
#define CV_F17        0x17
#define CV_F18        0x18
#define CV_F19        0x19
static void comval_init(void)
{
    uint8_t *cv = sector;
    os_comval_read(cv);
    if (*(uint16_t *)(cv + CV_MAGIC) != COMVAL_MAGIC) {
        os_comval_default(cv);
        cv[CV_F0F] = 2;
        cv[CV_F14] = cv[CV_F15] = cv[CV_F17] = 0;
        if (cv[CV_FLAGS] & CV_FLAG5) { cv[CV_F18] = 1; cv[CV_FLAGS] &= ~CV_FLAG5; }
        else cv[CV_F18] = 0;
        cv[CV_F19] = 1;
        os_comval_write(cv);
    }
    os_set_contrast(*(uint16_t *)(cv + CV_CONTRAST));
}

static void usb(void)
{
    if (os_exec_ap("usbdisk.ap", 0) == RESULT_UPGRADE)
        os_exec_ap("adfuf641.ap", 0);
}

uint16_t main(uint16_t param)
{
    uint8_t sel = 0, k;
    const char *msg = 0;
    (void)param;
    /* the kernel enters MAIN with interrupts off and the OS DI-nesting counter (0x0037) raised;
     * stock MAIN starts with EI (bank40 0x152B). As the top-level AP: counter = 0, EI - else the
     * first lcd_unlock() leaves interrupts off and no key ever arrives */
    WDT |= WDT_CLR;
    WDT = (WDT & ~WDT_LEN_MASK) | WDT_LEN_5S6;
    POWER = POWER_STOCK;
    /* the drivers the stock MAIN installs (the kernel does not): UI (small), keys, settings area;
     * the stock MAIN hangs if one fails, ZMAIN carries on (USB may still work without them) */
    os_drv_install(DRV_MODE_RESIDENT, "UI_6B33S.drv");
    os_drv_install(DRV_MODE_RESIDENT, "ky_hard.drv");
    os_drv_install(DRV_MODE_RESIDENT, "vm_nanf.drv");
    __asm__("xor a, a\n ld (0x0037), a\n ei");
    comval_init();
    os_drv_install(DRV_MODE_UI, "UI_6B33L.DRV");   /* language UI driver (USBDISK/ADFU use it) */
    /* os_exec_ap("fmradio.ap", 1);  stock boot FM init: hangs in the emulator, see below */
    if (os_usb_cable()) usb();             /* safety net: cable at boot -> USB disk */
    for (;;) {
        draw(sel, msg);
        msg = 0;
        for (;;) {
            wdt_kick();
            k = os_get_msg();
            if (k == MSG_USB_IN) { usb(); break; }
            if (k == KEY_LEFT && sel) { sel--; break; }
            if (k == KEY_RIGHT && sel + 1 < NITEMS) { sel++; break; }
            if (k == KEY_PLAY) {
                if (sel == M_ZOS) msg = run_zos();
                else if (sel == M_USB) usb();
                else if (sel == M_ADFU) os_exec_ap("adfuf641.ap", 0);
                else os_exec_ap("standby.ap", 0);
                break;
            }
        }
    }
}
