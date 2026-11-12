/* PERFTEST - CPU clock measurement, DMA LCD fill experiment, guarded clock-step test.
 * Findings and the reasoning behind every port write: lisong_rebuild/analysis/dma_clock.md
 *
 * Safety rules of this module:
 *  - only ports documented in the ATJ2085X programming guide are written (00, 06-13, 25, 42, 70);
 *  - DMA experiments touch only: ZRAM1 0x3400-0x34FF (saved + restored) as source, CE3 (the LCD,
 *    DA3 = 0x8C) as destination. Never a DSP lane (this module runs from one);
 *  - the clock-step tests need an explicit PLAY confirmation, run ~0.2 s with no LCD access and
 *    put ports 00/42/70 back to the values read at entry, before returning to the menu;
 *  - if the machine hangs the zos watchdog resets into the stock firmware (nothing is written to flash).
 */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"

#ifndef __SDCC                 /* editor / static analysis only */
#define __sfr volatile unsigned char
#define __at(x)
#endif

__sfr __at(0x00) P_CLK;        /* 7-6 ext wait states, 4 clock source, 3-0 divider */
#ifdef PERF_DMA2                /* DMA2 = ports 0x14-0x21 (same layout; recommended for the LCD) */
#define DB 0x14
#define DEND 0x20
#else                          /* DMA1 = ports 0x06-0x13 */
#define DB 0x06
#define DEND 0x08
#endif
__sfr __at(DB + 0) P_SA0;
__sfr __at(DB + 1) P_SA1;
__sfr __at(DB + 2) P_SA2;
__sfr __at(DB + 3) P_SA3;        /* 7 ext, 6 IPM/IDM/ZRAM2 select, 4-0 SA[28:24] */
__sfr __at(DB + 4) P_ISA;
__sfr __at(DB + 5) P_DA0;
__sfr __at(DB + 6) P_DA1;
__sfr __at(DB + 7) P_DA2;
__sfr __at(DB + 8) P_DA3;        /* same layout; CE3 = 011 in DA[28:26] -> 0x8C with bit 7 (external) */
__sfr __at(DB + 9) P_IDA;
__sfr __at(DB + 10) P_BC0;
__sfr __at(DB + 11) P_BC1;
__sfr __at(DB + 12) P_DMODE;      /* 7-6 wait states, 5 DSP mode, 4/3 down-count, 2 DSP boot, 1/0 IO */
__sfr __at(DB + 13) P_DCMD;       /* 0 = start (self-clearing) */
__sfr __at(0x25) P_DIRQ;       /* 3 = DMA1 end (W1C); NEVER write bit 0 (DMA reset pulse) */
__sfr __at(0x2E) P_DSPCTL;     /* read only here */
__sfr __at(0x42) P_PLL;        /* 7 HOSCEN, 6 PLL1EN, 5 M48EN, 4-2 DSP clock */
__sfr __at(0x4E) P_WDT;        /* read only here */
__sfr __at(0x4F) P_DCDC;       /* read only here */
__sfr __at(0x70) P_MCUDMA;     /* 3 = MCU clock from PLL, 2-0 DMA clock divider */

void spin(void);               /* spin.s */

#define NFILL     32           /* full-screen fills per timing run (~0.5 s) */
#define DMA_SRC   0x3400       /* 256-byte pattern: HAL_MODRAM block, unused while a page module runs */
#define CHUNK_T   26900UL      /* T-states per spin() + loop overhead in measure() */

static char    ln[24];
static uint8_t keep[256];      /* saved ZRAM1 bytes */
static uint16_t mhz100, cpu_us, dma_us;
static uint8_t  dma_bcn, dma_ws = 3, dma_quick, dma_flag, dma_state;  /* dma_state 0 untested 1 ok 2 timeout */
static uint8_t  sel;
static char    msg[24];

/* ------------------------------------------------------------------------------- helpers */
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *put_u(char *p, uint16_t v)
{
    char t[6];
    uint8_t n = 0;
    do { t[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static char *put_h(char *p, uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    *p++ = hx[v >> 4];
    *p++ = hx[v & 15];
    return p;
}

static void line(uint8_t y, const char *s, uint16_t fg)
{
    char b[22];
    uint8_t i = 0;
    while (s[i] && i < 21) { b[i] = s[i]; i++; }
    while (i < 21) b[i++] = ' ';
    b[21] = 0;
    gfx_text(1, y, b, 1, fg, C_BG);
}

static void wait_ticks(uint16_t t)
{
    uint16_t s = hal_ticks();
    while ((uint16_t)(hal_ticks() - s) < t) { hal_watchdog(); hal_idle(); }
}

/* ------------------------------------------------------------------------------- CPU clock */
/* spin() for 20 ticks (200 ms of the RTC-derived 10 ms tick, which does not depend on the CPU
 * clock) and convert the count to MHz*100. Accuracy ~1 % (ISR time, tick granularity). */
static uint16_t measure(void)
{
    uint16_t t, el, n = 0;
    t = hal_ticks();
    while (hal_ticks() == t) ;
    t = hal_ticks();
    do { spin(); n++; hal_watchdog(); el = hal_ticks() - t; } while (el < 20);
    return (uint16_t)((uint32_t)n * CHUNK_T / ((uint32_t)el * 100));
}

/* mode 0: divide by 2 (HOSC /2).  mode 1: 48 MHz from the PLL block (the "M48" clock USB uses).
 * returns MHz*100, or 0 if the starting state is not the expected one. Restores 00/42/70. */
static uint16_t clock_test(uint8_t mode)
{
    uint8_t c00 = P_CLK, c42 = P_PLL, c70 = P_MCUDMA;
    uint16_t r;
    if ((c00 & 0x1F) != 0x10 || (c70 & 0x08)) return 0;   /* HOSC, /1, MCU not on the PLL */
    if (mode == 0) {
        P_CLK = (c00 & 0xE0) | 0x11;                 /* HOSC, divider /2 */
        wait_ticks(2);
        r = measure();
        P_CLK = c00;
        wait_ticks(1);
        return r;
    }
    P_PLL = c42 | 0xE0;                              /* HOSCEN + PLL1EN + M48EN (as the stock USB code) */
    wait_ticks(3);                                   /* PLL lock, 30 ms (stock waits ~70 us) */
    P_CLK = (c00 & 0xE0) | 0x10;                     /* bit 4 = 48 MHz when 70.3 = 1; divider /1 */
    P_MCUDMA = c70 | 0x08;                           /* MCU clock := PLL clock */
    r = measure();
    P_MCUDMA = c70;                                  /* back to HOSC first ... */
    P_CLK = c00;
    P_PLL = c42;                                     /* ... then the PLL registers */
    wait_ticks(1);
    return r;
}

/* ------------------------------------------------------------------------------- LCD fills */
static uint16_t cpu_fill(void)
{
    uint8_t i;
    uint16_t t = hal_ticks();
    for (i = 0; i < NFILL; i++) {
        hal_lcd_window(0, 0, LCD_W, LCD_H);
        hal_lcd_fill(RGB(255, 0, 0), (uint16_t)LCD_W * LCD_H);
    }
    return (uint16_t)((uint32_t)(uint16_t)(hal_ticks() - t) * 10000UL / NFILL);   /* us per fill */
}

static void dma_prog(uint8_t ws, uint16_t bc)
{
    P_SA0 = DMA_SRC & 0xFF; P_SA1 = DMA_SRC >> 8; P_SA2 = 0; P_SA3 = 0x00;   /* ZRAM1 */
    P_ISA = 0x08;                                  /* stock convention for a ZRAM endpoint */
    P_DA0 = 0; P_DA1 = 0; P_DA2 = 0; P_DA3 = 0x8C;                            /* external, CE3 */
    P_IDA = 0;
    P_BC0 = bc & 0xFF; P_BC1 = bc >> 8;
    P_DMODE = (uint8_t)(ws << 6);
}

static uint8_t dma_go(void)
{
    uint16_t to = 60000;
    P_DCMD = 0x01;
    while ((P_DCMD & 1) && --to) ;
    if (!to) { P_DCMD = 0; return 0; }
    return 1;
}

/* NFILL full screens of colour `col` by DMA, 128 transfers of 256 bytes per screen */
static uint8_t dma_fill(uint16_t col)
{
    uint8_t i, r, ok = 1;
    uint16_t k, t, bc = 256 - (dma_bcn ? 0 : 1);
    volatile uint8_t *s = (volatile uint8_t *)DMA_SRC;
    for (k = 0; k < 256; k++) keep[k] = s[k];
    for (k = 0; k < 256; k += 2) { s[k] = col >> 8; s[k + 1] = col & 0xFF; }
    t = hal_ticks();
    for (i = 0; i < NFILL && ok; i++) {
        hal_lcd_window(0, 0, LCD_W, LCD_H);
        dma_prog(dma_ws, bc);
        for (r = 0; r < LCD_H; r++) {
            if (!dma_quick && r) dma_prog(dma_ws, bc);
            if (!dma_go()) { ok = 0; break; }
        }
        hal_watchdog();
    }
    dma_us = (uint16_t)((uint32_t)(uint16_t)(hal_ticks() - t) * 10000UL / NFILL);
    dma_flag = (P_DIRQ & DEND) != 0;
    if (dma_flag) P_DIRQ = DEND;                      /* W1C the DMA1-end flag we caused */
    for (k = 0; k < 256; k++) s[k] = keep[k];
    return ok;
}

/* ------------------------------------------------------------------------------- UI */
static const char *const names[] = {
    "CPU CLOCK MEASURE", "LCD FILL BY CPU", "LCD FILL BY DMA", "DMA COUNT", "DMA WAIT STATES",
    "DMA RESTART", "CLOCK /2 TEST", "CLOCK 48MHZ TEST"
};
#define NITEMS 8

static void draw_item(uint8_t i)
{
    char *p = ln;
    p = put_s(p, i == sel ? "> " : "  ");
    p = put_s(p, names[i]);
    if (i == 3) p = put_s(p, dma_bcn ? " N" : " N-1");
    if (i == 4) { *p++ = ' '; p = put_u(p, dma_ws); }
    if (i == 5) p = put_s(p, dma_quick ? " QUICK" : " FULL");
    *p = 0;
    line(SKIN_TITLE_H + 1 + 9 * i, ln, i == sel ? C_SEL : C_FG);
}

static void draw_all(void)
{
    char *p;
    uint8_t i;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("PERF TEST");
    for (i = 0; i < NITEMS; i++) draw_item(i);
    p = ln; p = put_s(p, "00:"); p = put_h(p, P_CLK); p = put_s(p, " 42:"); p = put_h(p, P_PLL);
    p = put_s(p, " 70:"); p = put_h(p, P_MCUDMA); *p = 0;
    line(88, ln, C_DIM);
    p = ln; p = put_s(p, "CPU X.01MHZ "); p = put_u(p, mhz100); *p = 0;
    line(97, ln, C_OK);
    p = ln; p = put_s(p, "FILL US C"); p = put_u(p, cpu_us); p = put_s(p, " D"); p = put_u(p, dma_us); *p = 0;
    line(106, ln, C_OK);
    line(115, msg, C_SEL);
}

static uint8_t confirm(const char *what)
{
    event_t e;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("CONFIRM");
    gfx_text(4, 20, what, 1, C_SEL, C_BG);
    gfx_text(4, 34, "CHANGES CPU CLOCK", 1, C_FG, C_BG);
    gfx_text(4, 44, "FOR 0.2 S, THEN", 1, C_FG, C_BG);
    gfx_text(4, 54, "RESTORES IT. A HANG", 1, C_FG, C_BG);
    gfx_text(4, 64, "RESETS TO STOCK.", 1, C_FG, C_BG);
    gfx_text(4, 84, "PLAY: GO", 1, C_OK, C_BG);
    gfx_text(4, 94, "ANY OTHER KEY: CANCEL", 1, C_DIM, C_BG);
    do skin_wait(&e, 0); while (e.type != EV_KEY_DOWN);
    return e.code == KEY_PLAY;
}

static void run(void)
{
    char *p;
    msg[0] = 0;
    switch (sel) {
    case 0: mhz100 = measure(); break;
    case 1: cpu_us = cpu_fill(); break;
    case 2:
        dma_state = dma_fill(RGB(255, 0, 0)) ? 1 : 2;
        wait_ticks(120);                              /* keep the red screen: check it is solid */
        p = msg; p = put_s(p, dma_state == 1 ? "DMA OK FLAG:" : "DMA TIMEOUT FLAG:");
        p = put_u(p, dma_flag); *p = 0;
        break;
    case 3: dma_bcn ^= 1; break;
    case 4: dma_ws = (dma_ws + 3) & 3; break;
    case 5: dma_quick ^= 1; break;
    case 6:
    case 7: {
        uint16_t r;
        if (!confirm(sel == 6 ? "CLOCK /2 TEST" : "CLOCK 48 MHZ TEST")) { p = msg; p = put_s(p, "CANCELLED"); *p = 0; break; }
        r = clock_test(sel - 6);
        p = msg;
        if (!r) p = put_s(p, "UNEXPECTED CLK STATE");
        else { p = put_s(p, sel == 6 ? "DIV2 X.01MHZ " : "M48 X.01MHZ "); p = put_u(p, r); }
        *p = 0;
        break;
    }
    }
}

uint16_t mod_main(void)
{
    event_t e;
    skin_load();
    mhz100 = measure();
    draw_all();
    for (;;) {
        skin_wait(&e, 0);
        if (e.type != EV_KEY_DOWN) continue;
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_RIGHT) sel = (sel + 1) % NITEMS;
        else if (e.code == KEY_LEFT) sel = (sel + NITEMS - 1) % NITEMS;
        else if (e.code == KEY_PLAY) run();
        draw_all();
    }
    return 0;
}
