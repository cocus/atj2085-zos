/* ZBOOT - replaces GAME.AP (JUEGOS): no menu, loads ZOS.AP from the root of the user disk and runs
 * it (zos never returns: it reboots into the stock firmware when left). On any problem it shows a
 * message and returns to the stock main menu on a key press.
 * Build/flash: python homebrew/build.py zboot --as GAME.AP --fw  (all other stock APs are kept).
 * Loader = the GAME.AP launcher's: the image is staged in DSP RAM page LPAGE while this AP is
 * intact, then a stub in that page copies it to 0x0800 and jumps to it (launcher/stub.s). */
#include <stdint.h>
#include "actos.h"
#include "gfx.h"
#include "fs.h"

#define LPAGE     0xC1              /* DSP program RAM lane: staging page (device-verified) */
#define PAGE_WIN  0x4000
#define STUB_PAR  0x7EF0            /* in the page, see stub.s */
#define STUB_CODE 0x7F00
#define LOAD_END  0x2C00            /* the app must end below FS_FAT16's cluster cache at 0x2C00 */
#define AP_LOAD   0x0800
#define AP_HDR_SECTORS 4            /* the AP header is 0x800 bytes */

#define BG  RGB(255, 255, 255)
#define FG  RGB(0, 0, 0)
#define ERR RGB(200, 0, 0)

extern uint16_t ap_entry_sp, ap_entry_bc, ap_entry_de, ap_entry_ix, ap_entry_iy;
extern uint8_t stub_start[], stub_end[];
typedef struct { uint16_t dst; const void *src; uint16_t len; } xfer_t;
void page_put(uint8_t page, const xfer_t *x);
void page_enter(uint8_t page, uint16_t addr);
uint8_t os_page(void);

static uint8_t sector[512];

static void put(uint16_t dst, const void *src, uint16_t len)
{
    xfer_t x; x.dst = dst; x.src = src; x.len = len;
    page_put(LPAGE, &x);
}

static const char *boot_zos(void)
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
    if (addr != AP_LOAD || !len || addr + (uint16_t)nsect * 512 > LOAD_END) goto bad;
    for (i = 1; i < AP_HDR_SECTORS; i++)
        if (!fs_fread(sector, h)) goto bad;
    for (i = 0; i < nsect; i++) {
        if (!fs_fread(sector, h)) goto bad;
        put(PAGE_WIN + (uint16_t)i * 512, sector, 512);
        wdt_kick();
    }
    fs_fclose(h);
    par[0] = (uint16_t)nsect * 512;
    par[1] = entry;
    par[2] = ap_entry_sp;                        /* called by the stock MAIN: its entry state */
    par[3] = ap_entry_bc;
    par[4] = ap_entry_de;
    par[5] = ap_entry_ix;
    par[6] = ap_entry_iy;
    par[7] = os_page();
    put(STUB_PAR, par, sizeof par);
    put(STUB_CODE, stub_start, stub_end - stub_start);
    page_enter(LPAGE, STUB_CODE);                /* does not return */
bad:
    fs_fclose(h);
    return "ZOS.AP: BAD FILE";
}

uint16_t main(uint16_t param)
{
    const char *msg;
    uint8_t k;
    (void)param;
    msg = boot_zos();
    gfx_fill(0, 0, SCR_W, SCR_H, BG);
    gfx_text(4, 40, msg, 1, ERR, BG);
    gfx_text(4, 60, "ANY KEY: BACK", 1, FG, BG);
    do { wdt_kick(); k = os_get_msg(); } while (k != KEY_MENU && k != KEY_PLAY && k != KEY_LEFT &&
                                                k != KEY_RIGHT && k != MSG_USB_IN);
    return RESULT_NULL;
}
