/* LCDDMA: LCD fill speed, CPU vs DMA2, at 24 and 48 MHz.
 * DMA2 (P_DMA2_*; DMA1 stays for the MP3 bitstream), byte count = n-1 (device-confirmed),
 * destination = LCD (CE3, DMA_A3_LCD; the LCD has no address pins, so the incrementing destination
 * is harmless), source = colour-bar pattern in ZRAM1 (HAL_MODRAM block, free while a page module runs).
 * Every test draws 8 vertical colour bars (16 px each): a dropped/extra byte shows as wrong colours
 * or slanted bars. PLAY: run the next test (results accumulate in the table), VOL: 24/48 MHz and
 * clear the table, MENU: back. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "../common/clock.h"

#include "regs.h"
SFR(D2_SA0, P_DMA2_SA0);
SFR(D2_SA1, P_DMA2_SA1);
SFR(D2_SA2, P_DMA2_SA2);
SFR(D2_SA3, P_DMA2_SA3);
SFR(D2_ISA, P_DMA2_ISA);
SFR(D2_DA0, P_DMA2_DA0);
SFR(D2_DA1, P_DMA2_DA1);
SFR(D2_DA2, P_DMA2_DA2);
SFR(D2_DA3, P_DMA2_DA3);
SFR(D2_IDA, P_DMA2_IDA);
SFR(D2_BCL, P_DMA2_BCL);
SFR(D2_BCH, P_DMA2_BCH);
SFR(D2_MODE, P_DMA2_MODE);
SFR(D2_CMD, P_DMA2_CMD);

#define SRC ((uint8_t *)HAL_MODRAM_BASE)       /* 1 KB pattern (4 rows of bars) */
#define REPS 16

static const uint16_t bars[8] = {
    RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
    RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0) };

enum { T_CPU, T_WS3, T_WS2, T_WS1, T_WS0, T_QUICK, T_1K, NT };
static const char *const tname[NT] = { "CPU", "DMA WS3", "DMA WS2", "DMA WS1", "DMA WS0", "QUICK", "1KB" };
static uint16_t res[NT];                       /* us per screen, 0 = not run */
static uint8_t done[NT];

static void make_pattern(void)
{
    uint16_t i;
    uint8_t *p = SRC;
    for (i = 0; i < 512; i++) {                /* 4 rows x 128 px */
        uint16_t c = bars[(i & 127) >> 4];
        *p++ = c >> 8;                         /* the LCD takes RGB565 big-endian */
        *p++ = (uint8_t)c;
    }
}

static void dma_setup(uint16_t n, uint8_t mode)
{
    D2_SA0 = (uint8_t)(uint16_t)SRC; D2_SA1 = (uint16_t)SRC >> 8; D2_SA2 = 0; D2_SA3 = DMA_A3_ZRAM1;
    D2_ISA = 0;
    D2_DA0 = 0; D2_DA1 = 0; D2_DA2 = 0; D2_DA3 = DMA_A3_LCD;
    D2_IDA = 0;
    D2_BCL = (uint8_t)(n - 1); D2_BCH = (n - 1) >> 8;                                        /* n-1 */
    D2_MODE = mode;
}

static uint8_t tmo;                            /* a DMA did not finish */
static void dma_go(void)
{
    uint16_t to = 2000;
    D2_CMD = DMA_CMD_START;
    while ((D2_CMD & DMA_CMD_START) && --to) ;             /* the CPU is halted during DMA anyway */
    if (!to) tmo = 1;
}

static void fill_screen(uint8_t t)
{
    uint8_t r;
    hal_lcd_window(0, 0, LCD_W, LCD_H);
    switch (t) {
    case T_CPU:                                /* the same bytes from RAM, by the CPU */
        for (r = 0; r < 32; r++) hal_lcd_blit(SRC, 512);
        break;
    case T_QUICK:
        dma_setup(256, DMA_MODE_WS3);
        for (r = 0; r < 128 && !tmo; r++) dma_go();    /* counters reload on each start? */
        break;
    case T_1K:
        for (r = 0; r < 32 && !tmo; r++) { dma_setup(1024, DMA_MODE_WS3); dma_go(); }
        break;
    default: {                                 /* WS3..WS0: full re-programming per row */
        static const uint8_t ws[4] = { DMA_MODE_WS3, DMA_MODE_WS2, DMA_MODE_WS1, DMA_MODE_WS0 };
        uint8_t mode = ws[t - T_WS3];
        for (r = 0; r < 128 && !tmo; r++) { dma_setup(256, mode); dma_go(); }
    }
    }
}

static void table(void)
{
    uint8_t i, y = SKIN_TITLE_H + 2;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(clk_fast ? "LCD DMA  @48MHZ" : "LCD DMA  @24MHZ");
    for (i = 0; i < NT; i++, y += 11) {
        gfx_text(4, y, tname[i], 1, C_FG, C_BG);
        if (done[i] == 2) gfx_text(64, y, "TMO", 1, C_SEL, C_BG);
        else if (done[i]) { gfx_num(64, y, res[i], 1, C_OK, C_BG); gfx_text(100, y, "US", 1, C_DIM, C_BG); }
        else gfx_text(64, y, "-", 1, C_DIM, C_BG);
    }
    gfx_text(4, y + 2, "US PER FULL SCREEN", 1, C_DIM, C_BG);
    skin_footer("PLAY:NEXT VOL:MHZ");
}

uint16_t mod_main(void)
{
    event_t e;
    uint8_t next = 0, i;
    uint16_t t0, dt;
    make_pattern();
    skin_load();
    table();
    for (;;) {
        skin_wait(&e, 0);
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_VOL) {
            if (clk_fast) cpu_normal(); else cpu_fast();
            memset(done, 0, sizeof done);
            next = 0;
            table();
        } else if (e.code == KEY_PLAY) {
            tmo = 0;
            t0 = hal_ticks();
            for (i = 0; i < REPS && !tmo; i++) { fill_screen(next); hal_watchdog(); }
            dt = hal_ticks() - t0;                         /* 10 ms units for REPS screens */
            res[next] = (uint16_t)((uint32_t)dt * 10000UL / REPS);
            done[next] = tmo ? 2 : 1;
            skin_wait(&e, 120);                              /* look at the bars */
            next = (next + 1) % NT;
            table();
        }
    }
    cpu_normal();
    return 0;
}
