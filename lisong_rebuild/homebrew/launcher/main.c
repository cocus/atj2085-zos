/* LAUNCHER - lists *.AP on the player's user disk (root folder) and runs the chosen one.
 * Copy homebrew apps (homebrew/out/<app>/<APP>.AP) to the player over USB, then
 * JUEGOS -> pick -> PLAY. << >> select, MENU back to the main menu.
 * Only homebrew-style APs (one segment at 0x0800, no banks, <= 9 KB) can be loaded. */
#include <stdint.h>
#include <string.h>
#include "actos.h"
#include "gfx.h"
#include "fs.h"

#define MAXAPPS   16
#define LPAGE     0xC1              /* DSP program RAM lane: staging page (device-verified W/X) */
#define PAGE_WIN  0x4000
#define STUB_PAR  0x7EF0            /* in the page, see stub.s */
#define STUB_CODE 0x7F00
#define LOAD_END  0x2C00            /* the app must end below FS_FAT16's cluster cache at 0x2C00 */

#define BG  RGB(10, 10, 40)
#define FG  RGB(230, 230, 230)
#define SEL RGB(255, 200, 0)
#define BAR RGB(0, 70, 140)
#define ERR RGB(255, 80, 80)

extern uint16_t ap_entry_sp, ap_entry_bc, ap_entry_de, ap_entry_ix, ap_entry_iy;
extern uint8_t stub_start[], stub_end[];
typedef struct { uint16_t dst; const void *src; uint16_t len; } xfer_t;
void page_put(uint8_t page, const xfer_t *x);
void page_enter(uint8_t page, uint16_t addr);
uint8_t os_page(void);

static char names[MAXAPPS][12];     /* 8.3 names as returned by FS_Dir */
static uint8_t napps;
static uint8_t sector[512];

static void show_name(uint8_t y, const char *n, uint16_t fg, uint16_t bg)
{
    char t[13]; uint8_t i, j = 0;
    for (i = 0; i < 8 && n[i] != ' ' && n[i]; i++) t[j++] = n[i];
    t[j++] = '.';
    for (i = 8; i < 11 && n[i] != ' ' && n[i]; i++) t[j++] = n[i];
    t[j] = 0;
    gfx_fill(0, y, SCR_W, 10, bg);
    gfx_text(8, y + 1, t, 1, fg, bg);
}

static void message(const char *a, const char *b)
{
    gfx_fill(0, 40, SCR_W, 40, BG);
    gfx_text(4, 44, a, 1, ERR, BG);
    if (b) gfx_text(4, 56, b, 1, FG, BG);
}

static uint8_t wait_any(void)
{
    uint8_t m;
    for (;;) {
        wdt_kick();
        m = os_get_msg();
        if (m == KEY_MENU || m == KEY_PLAY || m == KEY_LEFT || m == KEY_RIGHT || m == MSG_USB_IN) return m;
    }
}

static uint8_t mount(void)
{
    drv_active_ud(0, 1);
    if (!drv_active_drive('C', 1)) return 0;
    if (!fs_init()) return 0;
    fs_cd(":");
    return 1;
}

static void scan(void)
{
    uint8_t ok;
    napps = 0;
    memset(names, 0, sizeof names);
    for (ok = fs_dir_first(names[0], "AP "); ok && napps < MAXAPPS; ok = fs_dir_next(names[napps], "AP ")) {
        if (names[napps][0]) napps++;
    }
}

static void put(uint16_t dst, const void *src, uint16_t len)
{
    xfer_t x; x.dst = dst; x.src = src; x.len = len;
    page_put(LPAGE, &x);
}

/* validates the AP header, stages the whole image in page LPAGE, closes the file and enters the
 * stub (in the page), which copies the image to 0x0800 and jumps to it. Never returns on success. */
static const char *launch(uint16_t h)
{
    uint16_t len, addr, entry, end, par[8];
    uint8_t i, nsect;
    if (!fs_fread(sector, h)) return "READ ERROR";
    if (sector[0] != 'P' || sector[2] != 'W' || sector[3] != 'G') return "NOT AN AP FILE";
    len   = sector[12] | (uint16_t)sector[13] << 8;
    addr  = sector[14] | (uint16_t)sector[15] << 8;
    entry = sector[0x1C] | (uint16_t)sector[0x1D] << 8;
    if (sector[8] != 0 || sector[9] != 8 || sector[10] || sector[11]) return "TEXT NOT AT 0X800";
    if (addr != 0x0800) return "LOAD ADDR NOT 0X800";
    if (sector[0x1F] != 0 || (sector[0x14] | sector[0x15])) return "BANKS/DATA: NOT HOMEBREW";
    nsect = (uint8_t)((len + 511) / 512);
    end = addr + (uint16_t)nsect * 512;
    if (!len || end > LOAD_END) return "APP TOO LARGE";
    for (i = 0; i < 3; i++)                         /* skip the rest of the 0x800 header */
        if (!fs_fread(sector, h)) return "READ ERROR";
    for (i = 0; i < nsect; i++) {
        if (!fs_fread(sector, h)) return "READ ERROR";
        put(PAGE_WIN + (uint16_t)i * 512, sector, 512);
        wdt_kick();
    }
    fs_fclose(h);
    par[0] = (uint16_t)nsect * 512;
    par[1] = entry;
    par[2] = ap_entry_sp;
    par[3] = ap_entry_bc;
    par[4] = ap_entry_de;
    par[5] = ap_entry_ix;
    par[6] = ap_entry_iy;
    par[7] = os_page();
    put(STUB_PAR, par, sizeof par);
    put(STUB_CODE, stub_start, stub_end - stub_start);
    page_enter(LPAGE, STUB_CODE);                   /* does not return */
    return "STUB RETURNED";
}

uint16_t main(uint16_t param)
{
    uint8_t sel = 0, i, k;
    uint16_t h;
    const char *err;
    (void)param;
    gfx_fill(0, 0, SCR_W, SCR_H, BG);
    gfx_fill(0, 0, SCR_W, 10, BAR);
    gfx_text(4, 1, "APP LAUNCHER", 1, FG, BAR);
    if (!mount()) {
        message("NO USER DISK", "MENU: BACK");
        while (wait_any() != KEY_MENU) ;
        return RESULT_NULL;
    }
    scan();
    if (!napps) {
        message("NO *.AP IN ROOT", "COPY APPS VIA USB");
        while (wait_any() != KEY_MENU) ;
        return RESULT_NULL;
    }
    gfx_text(4, 118, "PLAY:RUN  MENU:BACK", 1, FG, BG);
    for (;;) {
        for (i = 0; i < napps && i < 10; i++)
            show_name(14 + i * 10, names[i], i == sel ? SEL : FG, i == sel ? BAR : BG);
        k = wait_any();
        if (k == KEY_MENU || k == MSG_USB_IN) return RESULT_NULL;
        if (k == KEY_RIGHT && sel + 1 < napps && sel < 9) sel++;
        if (k == KEY_LEFT && sel) sel--;
        if (k == KEY_PLAY) {
            h = fs_fopen(names[sel]);
            if (!h) { message("CANNOT OPEN", 0); continue; }
            err = launch(h);                         /* only returns on error (file still open) */
            fs_fclose(h);
            message(err, "PLAY: OK");
            wait_any();
            gfx_fill(0, 40, SCR_W, 40, BG);
        }
    }
}
