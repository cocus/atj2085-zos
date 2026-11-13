/* AUDIOTST family: converter ports, register restore, small helpers, volume, the DAC (Z80 -> FIFO).
 * Included by the launcher (modules/audiotst) and the test modules (ATTONE, ATREC); see at.h and
 * the mechanism notes in modules/audiotst/main.c. */
#include <stdint.h>
#include "../../../hal/hal.h"
#include "../../../core/event.h"
#include "../../../apps/theme.h"
#include "../skin.h"
#include "../skcol.h"
#include "regs.h"                 /* ports: DACCTL, DACRATE, DAC_L.., PAVOL, AICTL, MIXER, ... */
#include "at.h"



static void regs_restore(void)
{
    DACCTL = sv[1] | 0x02;                  /* mute first, then go back down */
    PACTL = sv[13] & 0x3F;
    PACTL = sv[13];
    PAVOL = sv[4];
    AICTL = sv[8]; MIXER = sv[9]; AIGAIN = sv[10]; ADCTUNE = sv[7]; ADCMOD = sv[5];
    ADCPD2 = sv[6]; ADCPD9 = sv[12]; ADCBUS = sv[14];
    ADCFIFO = sv[11] & 0x97;                /* never write the sync / pending bits back */
    DACRATE = sv[2];
    PLLCTL = sv[0];
    DACFIFO = sv[3] & 0x1F;
    DACCTL = sv[1];
}

/* ---- small helpers --------------------------------------------------------------------------- */
static void wait_ticks(uint16_t t)
{
    uint16_t s = hal_ticks();
    while ((uint16_t)(hal_ticks() - s) < t) { hal_watchdog(); hal_idle(); }
}

static uint8_t key_rep;                     /* last getkey() was an auto-repeat */
static uint8_t getkey(void)                 /* 0 = none; MENU also for EV_QUIT */
{
    event_t e;
    while (ev_get(&e)) {
        if (e.type == EV_QUIT) return KEY_MENU;
        if (e.type == EV_BATT) skin_batt();           /* (skinlite: nothing) */
        if (e.type == EV_TIME) skin_time();
        if (e.type == EV_KEY_DOWN) { key_rep = 0; return e.code; }
        if (e.type == EV_KEY_REPEAT) { key_rep = 1; return e.code; }
    }
    return 0;
}

static void clr(uint8_t y, uint8_t h) { gfx_fill(0, y, LCD_W, h, C_BG); }
/* scale 1 on the skin background: the 6-argument gfx calls are big in SDCC */
static void txt(uint8_t x, uint8_t y, const char *s, uint16_t fg) { gfx_text(x, y, s, 1, fg, C_BG); }
static void num(uint8_t x, uint8_t y, uint16_t n, uint16_t fg) { gfx_num(x, y, n, 1, fg, C_BG); }
static void hexn(uint8_t x, uint8_t y, uint8_t n, uint16_t fg)
{
    gfx_char(x, y, n < 10 ? '0' + n : 'A' + n - 10, 1, fg, C_BG);
}
static void hex2(uint8_t x, uint8_t y, uint8_t v, uint16_t fg) { hexn(x, y, v >> 4, fg); hexn(x + 6, y, v & 15, fg); }
static void hex4(uint8_t x, uint8_t y, uint16_t v, uint16_t fg) { hex2(x, y, v >> 8, fg); hex2(x + 12, y, (uint8_t)v, fg); }

/* ---- volume + analog path (OS PA_Enable / PA_Disable) ---------------------------------------- */
static void vol_apply(void) { PAVOL = (PAVOL & 0xE0) | vol; }

static void vol_ramp(uint8_t target)        /* 1 step per 10 ms: no pops */
{
    uint8_t v = PAVOL & 0x1F;
    while (v != target) {
        if (v < target) v++; else v--;
        PAVOL = (PAVOL & 0xE0) | v;
        wait_ticks(1);
    }
}

static uint8_t dac_up, fm_up;               /* what is currently powered */

/* ---- DAC: Z80 -> FIFO (0x88/89/8A) ----------------------------------------------------------- */
static uint8_t dac_dead;

static void dac_put(int16_t s)              /* one 20-bit FIFO word = s << 4 */
{
    uint16_t t = 0;
    if (dac_dead) return;
    while (DACFIFO & 0x20) if (!++t) { dac_dead = 1; return; }
    DAC_L = 0;
    DAC_M = (uint8_t)s;
    DAC_H = (uint8_t)((uint16_t)s >> 8);
}

/* silent measurement of the DAC clock: how many FIFO words does the DAC eat per second? */
static void measure_dac(void)
{
    uint16_t t0, n = 0;
    uint8_t i, d = 0;
    DACFIFO = 0x08; DACFIFO = 0x18;           /* FIFO reset pulse */
    while (!(DACFIFO & 0x20) && d < 64) { DAC_L = 0; DAC_M = 0; DAC_H = 0; d++; }
    fifo_depth = d;
    t0 = hal_ticks();
    while ((uint16_t)(hal_ticks() - t0) < 20) {                 /* 200 ms */
        for (i = 0; i < 16; i++)
            if (!(DACFIFO & 0x20)) { DAC_L = 0; DAC_M = 0; DAC_H = 0; n++; }
    }
    hal_watchdog();
    wps = n * 5;
    measured = 1;
    if (wpf_mode) wpf = wpf_mode;
    else if (wps < 200) wpf = 2;
    else wpf = (wps < 12000) ? 1 : 2;       /* 8 kHz: 8000 words/s = mono, 16000 = L,R pairs */
}

static void dac_on(uint8_t fm_mon)
{
    PLLCTL &= 0xFC;                          /* OS DAC_SetRate: 48k family -> PLL2 bits off */
    DACRATE = 0x07;                           /* 8 kHz */
    DACCTL = 0x2A;                          /* DACEN + SDM, still muted */
    DACFIFO = 0x08; DACFIFO = 0x18;           /* OS DAC_Enable: FIFO reset, QU = 8 */
    PAVOL = (PAVOL & 0xE0);                   /* volume 0 */
    PACTL = 0xC2;                             /* analog DAC + mixer/PA + bias (OS PA_Enable) */
    if (fm_mon) MIXER |= 0x04;
    wait_ticks(10);
    DACCTL = 0x28;                          /* unmute */
    dac_dead = 0;
    if (!measured) measure_dac();
    dac_up = 1;
    vol_ramp(vol);
}

static void dac_off(void)
{
    vol_ramp(0);
    DACCTL |= 0x02;                         /* mute */
    PACTL &= 0x3F;
    MIXER &= 0xE3;
    DACCTL &= 0xDF;                         /* DACEN off (OS DAC_Disable) */
    dac_up = 0;
}

/* FM straight into the analog mixer / power amp (OS PA_Enable with fmin: D5.2, DD |= 0x42) */
static void fm_mon_on(void)
{
    MIXER |= 0x04;
    PACTL = 0x42;
    vol_ramp(vol);
    fm_up = 1;
}
static void fm_mon_off(void)
{
    vol_ramp(0);
    PACTL &= 0x3F;
    MIXER &= 0xE3;
    fm_up = 0;
}

static void draw_vol(uint8_t y)
{
    uint8_t i;
    clr(y, 10);
    txt(4, y + 1, "VOL", C_DIM);
    num(28, y + 1, vol, C_SEL);
    for (i = 0; i < 32; i++)
        gfx_fill(52 + i * 2, y + 8 - (i >> 2), 1, 1 + (i >> 2), i < vol ? C_OK : RGB(28, 34, 60));
}

