/* CPU clock switch for modules (include in ONE source file per module). Sequence:
 * lisong_rebuild/analysis/dma_clock.md 2.2. 24.576 MHz = HOSC /1 (stock and zos default);
 * 48 MHz = PLL1's 48 MHz output (P_CLKCTL2.CLK2_PLLCPU + P_CLKCTL.CLK_SRC).
 * The 10 ms tick (RTC, 32 kHz crystal) and the watchdog do not change with the CPU clock.
 * PLL1 feeds the 48 MHz clock and the DSP: never reprogram it while fast (mp3.c pll_change).
 * External bus wait states (LCD/NAND) are kept. */
#ifndef MOD_CLOCK_H
#define MOD_CLOCK_H
#include <stdint.h>
#include "../../hal/hal.h"
#include "regs.h"


static uint8_t clk_s00, clk_s70, clk_fast;

static void clk_wait_ticks(uint8_t n)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < n) hal_idle();
}

static void cpu_fast(void)
{
    if (clk_fast) return;
    clk_s00 = CLKCTL;
    clk_s70 = CLKCTL2;
    if ((PLLCTL & (PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN)) != (PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN)) {
        PLLCTL |= PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN;   /* = the stock USB enable */
        clk_wait_ticks(4);                             /* >= 30 ms PLL lock (only when it was off) */
    }
    CLKCTL = (clk_s00 & CLK_WS_MASK) | CLK_SRC;        /* 48 MHz source, divider /1, wait states kept */
    CLKCTL2 = clk_s70 | CLK2_PLLCPU;                   /* switch */
    clk_fast = 1;
}

static void cpu_normal(void)
{
    if (!clk_fast) return;
    CLKCTL2 = clk_s70;                                 /* HOSC again */
    CLKCTL = clk_s00;
    clk_fast = 0;                                      /* PLL1/M48 stay on (the DSP uses PLL1) */
}
#endif
