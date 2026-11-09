/* MEMORY PROBE - measures the 0x4000-0x7FFF paged window (port 0x05) on the real device.
 * Everything here runs from ZRAM1 (0x0800..), so remapping the window is safe for the code itself.
 * Non-destructive: lanes are only written at 0x6000..0x6007 after saving the bytes, and restored.
 * The lane visible with the boot value (stock OS: its drivers live there) is never written.
 *  1. checksum of 0x4000-0x40FF and 0x7F00-0x7FFF per port-05 value  -> which values alias
 *  2. write/readback at 0x6000 per candidate lane, and independence from the stock lane
 *  3. run a tiny routine copied into a lane (code execution from the window) */
#include <stdint.h>
#include "../hal/hal.h"
#include "../core/event.h"
#include "theme.h"
#include "apps.h"

#ifndef __SDCC
#define __sfr volatile unsigned char
#define __at(x)
#endif
__sfr __at(0x05) P_PAGE;

#define WIN(a) ((volatile uint8_t *)(a))

static const uint8_t vals[] = { 0x00, 0xC0, 0xC1, 0xC2, 0x34, 0x35, 0x36 };
#define NV (sizeof vals)

static uint16_t sum256(uint16_t a)
{
    uint16_t s = 0; uint16_t i;
    for (i = 0; i < 256; i++) s += WIN(a)[i] ^ (uint8_t)i;
    return s;
}

static char hexd(uint8_t v) { v &= 15; return v < 10 ? '0' + v : 'A' + v - 10; }
static void hex8(uint8_t x, uint8_t y, uint8_t v, uint16_t c)
{
    char t[3]; t[0] = hexd(v >> 4); t[1] = hexd(v); t[2] = 0;
    gfx_text(x, y, t, 1, c, C_BG);
}
static void hex16(uint8_t x, uint8_t y, uint16_t v, uint16_t c) { hex8(x, y, v >> 8, c); hex8(x + 12, y, v, c); }

/* ld a,#0x5A ; xor a,#0x0F ; ret   -> returns 0x55 if executed from the window */
static const uint8_t probe_code[] = { 0x3E, 0x5A, 0xEE, 0x0F, 0xC9 };

static uint8_t call_window(void) __naked
{
    __asm
        call 0x6000
        ret
    __endasm;
}

void app_memprobe(void)
{
    event_t e;
    uint8_t base, i, j, y, ok, wr, ind, ex, save[8];
    uint16_t lo, hi;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    ui_header("MEMORY PROBE");
    base = P_PAGE;
    gfx_text(2, 14, "BOOT 05=", 1, C_FG, C_BG);
    hex8(50, 14, base, C_SEL);
    gfx_text(2, 24, "05 SUM-LO SUM-HI W I X", 1, C_DIM, C_BG);
    y = 34;
    for (i = 0; i < NV; i++, y += 10) {
        uint8_t v = vals[i];
        __asm__("di");
        P_PAGE = v;
        lo = sum256(0x4000);
        hi = sum256(0x7F00);
        wr = ind = ex = 2;                       /* 2 = not tested */
        if (v != base && v != 0x00 && v != 0xC0) {
            /* write/readback at 0x6000 */
            for (j = 0; j < 8; j++) save[j] = WIN(0x6000)[j];
            WIN(0x6000)[0] = 0xA5; WIN(0x6000)[1] = 0x5A;
            wr = WIN(0x6000)[0] == 0xA5 && WIN(0x6000)[1] == 0x5A;
            /* independence: the stock lane must not see the pattern */
            P_PAGE = base;
            ind = !(WIN(0x6000)[0] == 0xA5 && WIN(0x6000)[1] == 0x5A);
            P_PAGE = v;
            /* execution */
            for (j = 0; j < sizeof probe_code; j++) WIN(0x6000)[j] = probe_code[j];
            ex = call_window() == 0x55;
            for (j = 0; j < 8; j++) WIN(0x6000)[j] = save[j];
        }
        P_PAGE = base;
        __asm__("ei");
        hex8(2, y, v, C_FG);
        hex16(20, y, lo, C_FG);
        hex16(56, y, hi, C_FG);
        ok = (wr == 1) ? 'Y' : (wr == 0 ? 'N' : '-');
        gfx_char(92, y, ok, 1, wr == 1 ? C_OK : C_FG, C_BG);
        gfx_char(104, y, ind == 1 ? 'Y' : (ind == 0 ? 'N' : '-'), 1, ind == 1 ? C_OK : C_FG, C_BG);
        gfx_char(116, y, ex == 1 ? 'Y' : (ex == 0 ? 'N' : '-'), 1, ex == 1 ? C_OK : C_FG, C_BG);
    }
    ui_footer("W=WRITE I=INDEP X=EXEC");
    do ev_wait(&e, 0); while (e.type != EV_KEY_DOWN);
}
