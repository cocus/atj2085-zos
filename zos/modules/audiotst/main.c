/* AUDIOTST: DAC test tones, ADC record (microphone / FM radio), playback, live level meter.
 *
 * Mechanism (evidence in lisong_rebuild/analysis/audio_hw.md): the Z80 talks to the converters directly,
 * no DSP involved:
 *   DAC : 20-bit PCM written to ports 0x88/0x89/0x8A (the 0x8A write commits the word into the 8-deep
 *         playback FIFO), FIFO-full flag = port 0x8B bit 5. Rate 0x81, control 0x80, analog 0xDD/0x9E.
 *   ADC : port 0x9A (R hi, R lo [, L hi, L lo]), FIFO not-empty = port 0xD7 bit 5. Source/gain/rate in
 *         0xD3..0xD6, 0x9F. We poll, no IRQ (zos' 0x27 mask stays 0x20).
 * The register sequences are copies of the stock OS routines ADC_Enable/DAC_Enable/PA_Enable/AI_Enable
 * (SYSBNKA.SYS).  Everything we change is saved on entry and restored on exit.
 *
 * Keys: MENU back, LEFT/RIGHT/PLAY/VOL as printed in the footer of each screen. */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "../common/skcol.h"
#include "../common/skinlite.c"
#include "sintab.h"

#ifndef __SDCC                 /* editor / static analysis only */
#define __sfr volatile unsigned char
#define __at(x)
#endif

/* ---- ports ----------------------------------------------------------------------------------- */
__sfr __at(0x42) P_CLK;         /* DSP clock; bits 1-0 select the DAC PLL family (OS DAC_SetRate) */
__sfr __at(0x80) DACCTL;        /* 5 DACEN, 3 SDM, 1 mute */
__sfr __at(0x81) DACSR;         /* sample rate code, 0x07 = 8 kHz */
__sfr __at(0x88) DAC_L;         /* PCM[3:0] in bits 7-4 */
__sfr __at(0x89) DAC_M;         /* PCM[11:4] */
__sfr __at(0x8A) DAC_H;         /* PCM[19:12]; the write commits the word */
__sfr __at(0x8B) FIFOST;        /* 5 DAC FIFO full (RO), 4 FIFO reset released */
__sfr __at(0x9A) ADCD;
__sfr __at(0x9B) ADCBUS;        /* 0 = 16 bit */
__sfr __at(0x9E) VOLR;          /* 4-0 headphone volume, 1.8 dB/step */
__sfr __at(0x9F) ADCMOD;        /* ADC modulator tune, OS: 0xDA mic, 0x7A line/FM */
__sfr __at(0xD2) PD2;           /* OS sets bit 7 in ADC_Enable (undocumented) */
__sfr __at(0xD3) ADCTUNE;       /* 1-0 ADC input gain -6/-3/0/+3 dB */
__sfr __at(0xD4) ADC1;          /* 7 ADC en, 6 MIC, 5 line, 4 FM stage, 3 dither, 2-0 mic gain */
__sfr __at(0xD5) ADC2;          /* 7-5 fs, 4/3/2 mic/line/FM -> analog mixer, 1-0 source */
__sfr __at(0xD6) AING;          /* 6-4 line gain, 2-0 FM gain */
__sfr __at(0xD7) ADCFIFO;       /* 5 not-empty (RO), 4 right only, 2 FIFO not-reset, 7/3 IRQ en/pending */
__sfr __at(0xD9) PD9;           /* OS clears bits 1-0 in ADC_Enable (undocumented) */
__sfr __at(0xDD) ANA;           /* 7 DAC analog, 6 mixer+PA enable, 1-0 PA bias */
__sfr __at(0xF3) GPC_EN;        /* FM tuner I2C pins (see i2c.md) */
__sfr __at(0xF4) GPC_DATA;

#define NREG 16
static uint8_t sv[NREG];        /* saved on entry */

static void regs_save(void)
{
    sv[0] = P_CLK;  sv[1] = DACCTL; sv[2] = DACSR;  sv[3] = FIFOST; sv[4] = VOLR;   sv[5] = ADCMOD;
    sv[6] = PD2;    sv[7] = ADCTUNE; sv[8] = ADC1;  sv[9] = ADC2;   sv[10] = AING; sv[11] = ADCFIFO;
    sv[12] = PD9;   sv[13] = ANA;   sv[14] = ADCBUS;
}

static void regs_restore(void)
{
    DACCTL = sv[1] | 0x02;                  /* mute first, then go back down */
    ANA = sv[13] & 0x3F;
    ANA = sv[13];
    VOLR = sv[4];
    ADC1 = sv[8]; ADC2 = sv[9]; AING = sv[10]; ADCTUNE = sv[7]; ADCMOD = sv[5];
    PD2 = sv[6]; PD9 = sv[12]; ADCBUS = sv[14];
    ADCFIFO = sv[11] & 0x97;                /* never write the sync / pending bits back */
    DACSR = sv[2];
    P_CLK = sv[0];
    FIFOST = sv[3] & 0x1F;
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
static uint8_t vol = 14;                    /* 0..31 */
static void vol_apply(void) { VOLR = (VOLR & 0xE0) | vol; }

static void vol_ramp(uint8_t target)        /* 1 step per 10 ms: no pops */
{
    uint8_t v = VOLR & 0x1F;
    while (v != target) {
        if (v < target) v++; else v--;
        VOLR = (VOLR & 0xE0) | v;
        wait_ticks(1);
    }
}

static uint8_t dac_up, fm_up;               /* what is currently powered */

/* ---- DAC: Z80 -> FIFO (0x88/89/8A) ----------------------------------------------------------- */
static uint8_t wpf_mode;                    /* 0 auto, 1, 2 : FIFO writes per sample frame */
static uint8_t wpf = 2, measured;
static uint8_t fifo_depth;
static uint16_t wps;                        /* measured FIFO writes per second */
static uint8_t dac_dead;

static void dac_put(int16_t s)              /* one 20-bit FIFO word = s << 4 */
{
    uint16_t t = 0;
    if (dac_dead) return;
    while (FIFOST & 0x20) if (!++t) { dac_dead = 1; return; }
    DAC_L = 0;
    DAC_M = (uint8_t)s;
    DAC_H = (uint8_t)((uint16_t)s >> 8);
}

/* silent measurement of the DAC clock: how many FIFO words does the DAC eat per second? */
static void measure_dac(void)
{
    uint16_t t0, n = 0;
    uint8_t i, d = 0;
    FIFOST = 0x08; FIFOST = 0x18;           /* FIFO reset pulse */
    while (!(FIFOST & 0x20) && d < 64) { DAC_L = 0; DAC_M = 0; DAC_H = 0; d++; }
    fifo_depth = d;
    t0 = hal_ticks();
    while ((uint16_t)(hal_ticks() - t0) < 20) {                 /* 200 ms */
        for (i = 0; i < 16; i++)
            if (!(FIFOST & 0x20)) { DAC_L = 0; DAC_M = 0; DAC_H = 0; n++; }
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
    P_CLK &= 0xFC;                          /* OS DAC_SetRate: 48k family -> PLL2 bits off */
    DACSR = 0x07;                           /* 8 kHz */
    DACCTL = 0x2A;                          /* DACEN + SDM, still muted */
    FIFOST = 0x08; FIFOST = 0x18;           /* OS DAC_Enable: FIFO reset, QU = 8 */
    VOLR = (VOLR & 0xE0);                   /* volume 0 */
    ANA = 0xC2;                             /* analog DAC + mixer/PA + bias (OS PA_Enable) */
    if (fm_mon) ADC2 |= 0x04;
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
    ANA &= 0x3F;
    ADC2 &= 0xE3;
    DACCTL &= 0xDF;                         /* DACEN off (OS DAC_Disable) */
    dac_up = 0;
}

/* FM straight into the analog mixer / power amp (OS PA_Enable with fmin: D5.2, DD |= 0x42) */
static void fm_mon_on(void)
{
    ADC2 |= 0x04;
    ANA = 0x42;
    vol_ramp(vol);
    fm_up = 1;
}
static void fm_mon_off(void)
{
    vol_ramp(0);
    ANA &= 0x3F;
    ADC2 &= 0xE3;
    fm_up = 0;
}

/* ---- ADC (OS ADC_Enable + AI_Enable): 8 kHz, right channel only, 16 bit ----------------------- */
static uint8_t mic_gain = 5;                /* D4.2-0: x22 x27 x32 x37 x42 x47 x52 x57 (chip default 5) */
static const char *const gain_txt[8] = { "X22", "X27", "X32", "X37", "X42", "X47", "X52", "X57" };

static void adc_flush(void)
{
    uint8_t n = 0;
    ADCFIFO = 0x10;                         /* FIFO reset (bit2 = 0), right only */
    ADCFIFO = 0x54;                         /* release + sync (OS uses 0xCD with IRQ on; we poll) */
    ADCFIFO = 0x14;
    while ((ADCFIFO & 0x20) && n++ < 40) { (void)ADCD; (void)ADCD; }
}

static void adc_on(uint8_t fm)
{
    ADC2 = (ADC2 & 0x1C) | (fm ? 1 : 0);    /* source (00 mic, 01 FM), fs = 8 kHz (000) */
    ADCTUNE = (ADCTUNE & 0xFC) | 3;         /* input gain +3 dB (chip default) */
    ADCBUS = 0;                             /* 16 bit */
    PD2 |= 0x80;                            /* what the OS ADC_Enable does */
    PD9 &= 0xFC;
    if (fm) {
        ADCMOD = 0x7A;                      /* AI_Enable fmin */
        AING = (AING & 0xF8) | 5;           /* FM gain 0 dB (stock FMRADIO) */
        ADC1 = 0x88 | 0x10 | (ADC1 & 7);    /* ADC on + dither + FM stage */
    } else {
        ADCMOD = 0xDA;                      /* AI_Enable micin */
        ADC1 = 0x88 | 0x40 | mic_gain;
    }
    wait_ticks(10);
    adc_flush();
}

static void adc_off(void)
{
    PD2 &= 0x7F;                            /* OS ADC_Disable */
    ADC1 &= 0x0F;
    ADCFIFO = 0x48;
}

/* ---- FM tuner TEA5767, bit-banged on GPIO C (i2c.md) ------------------------------------------ */
#define SCL      0x01
#define SDA      0x02
#define SCL_IE   0x10
#define SDA_IE   0x20
#define EN_MASK  (SCL | SDA | SCL_IE | SDA_IE)
#define DAT_MASK (SCL | SDA)
#define TEA_ADDR 0x60

static uint8_t sv_en, sv_dat;
static void dly(void) { volatile uint8_t i; for (i = 4; i; i--) ; }
static void en_set(uint8_t set, uint8_t clr) { GPC_EN = ((GPC_EN & 0x77) | set) & ~clr & 0x77; }
static void dat_set(uint8_t set, uint8_t clr) { GPC_DATA = ((GPC_DATA & 0x07) | set) & ~clr & 0x07; }
static void scl(uint8_t v) { if (v) dat_set(SCL, 0); else dat_set(0, SCL); }
static void sda(uint8_t v) { if (v) dat_set(SDA, 0); else dat_set(0, SDA); en_set(SDA, SDA_IE); }
static void sda_release(void) { en_set(SDA_IE, SDA); }
static uint8_t sda_read(void) { return (GPC_DATA & SDA) ? 1 : 0; }

static void bus_open(void)
{
    sv_en = GPC_EN & EN_MASK; sv_dat = GPC_DATA & DAT_MASK;
    dat_set(SCL | SDA, 0);
    en_set(SCL | SDA, SCL_IE | SDA_IE);
}
static void bus_close(void)
{
    uint8_t d = GPC_DATA & 0x07, e = GPC_EN & 0x77;
    GPC_DATA = (d & ~DAT_MASK) | sv_dat;
    GPC_EN = (e & ~EN_MASK) | sv_en;
}
static void i2c_start(void) { sda(1); scl(1); dly(); sda(0); dly(); scl(0); dly(); }
static void i2c_stop(void) { scl(0); sda(0); dly(); scl(1); dly(); sda(1); dly(); }
static uint8_t i2c_wr(uint8_t b)
{
    uint8_t i, ack;
    for (i = 0; i < 8; i++) { sda(b & 0x80); b <<= 1; dly(); scl(1); dly(); scl(0); }
    sda_release(); dly(); scl(1); dly();
    ack = !sda_read();
    scl(0); dly(); sda(0);
    return ack;
}
static uint8_t i2c_rd(uint8_t nack)
{
    uint8_t i, v = 0;
    sda_release();
    for (i = 0; i < 8; i++) { scl(1); dly(); v = (v << 1) | sda_read(); scl(0); dly(); }
    sda(nack); dly(); scl(1); dly(); scl(0); dly(); sda(0);
    return v;
}

static uint8_t tea_r[5];
static uint8_t tea_ok;                      /* last write was ACKed */

static void tea_tune(uint16_t f10, uint8_t standby)
{
    uint32_t k = (uint32_t)f10 * 100UL + 225UL;
    uint16_t pll = (uint16_t)((k * 125UL + 512UL) >> 10);   /* stock CALCULATE_PLL, high-side injection */
    uint8_t w[5], i, ok;
    w[0] = (uint8_t)(pll >> 8); w[1] = (uint8_t)pll; w[2] = 0x10;
    w[3] = 0x16 | (standby ? 0x40 : 0); w[4] = 0;
    i2c_start();
    ok = i2c_wr(TEA_ADDR << 1);
    for (i = 0; ok && i < 5; i++) ok = i2c_wr(w[i]);
    i2c_stop();
    tea_ok = ok;
}
static uint8_t tea_read(void)
{
    uint8_t i, ok;
    i2c_start();
    ok = i2c_wr((TEA_ADDR << 1) | 1);
    if (ok) for (i = 0; i < 5; i++) tea_r[i] = i2c_rd(i == 4);
    i2c_stop();
    return ok;
}

/* ---- G.711 u-law: 8 bits per sample, 13-bit dynamic range, no gain setting needed ------------- */
static uint8_t ulaw_enc(int16_t p14)        /* p14: 14-bit linear */
{
    uint8_t mask, seg;
    uint16_t u;
    if (p14 < 0) { u = -p14; mask = 0x7F; } else { u = p14; mask = 0xFF; }
    if (u > 8158) u = 8158;
    u += 33;
    for (seg = 0; seg < 8 && u >= (uint16_t)(0x40 << seg); seg++) ;
    return ((seg << 4) | ((u >> (seg + 1)) & 0x0F)) ^ mask;
}
static int16_t ulaw_dec(uint8_t u)          /* -> 16-bit linear */
{
    int16_t t;
    u = ~u;
    t = ((int16_t)(u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

/* ---- level meter ------------------------------------------------------------------------------ */
#define MX 4
#define MY 44
#define MW 120
static uint16_t lvl_of(uint16_t p)          /* log-ish 0..59 */
{
    uint8_t b = 0, f;
    uint16_t q = p;
    while (q) { b++; q >>= 1; }
    if (b < 3) return b * 4;
    f = (p >> (b - 3)) & 3;
    return (uint16_t)(b * 4 + f);
}
static void bar_range(uint8_t a, uint8_t b, uint8_t on)    /* pixel columns [a,b) of the meter */
{
    uint8_t m, e;
    while (a < b) {
        uint16_t c;
        e = b;
        if (a < 80) { c = C_OK; if (e > 80) e = 80; }
        else if (a < 104) { c = C_SEL; if (e > 104) e = 104; }
        else c = RGB(255, 60, 40);
        m = e - a;
        gfx_fill(MX + a, MY, m, 8, on ? c : RGB(28, 34, 60));
        a = e;
    }
}
static uint8_t bar_len;
static void bar_set(uint8_t n)
{
    if (n > MW) n = MW;
    if (n > bar_len) bar_range(bar_len, n, 1);
    else if (n < bar_len) bar_range(n, bar_len, 0);
    bar_len = n;
}
static void bar_reset(void) { bar_len = 0; bar_range(0, MW, 0); }

/* ---- TONE screen ------------------------------------------------------------------------------ */
static const char *const tone_name[6] = { "SINE 440 HZ", "SINE 1 KHZ", "SQUARE 440 HZ", "SQUARE 1 KHZ", "SINE 250 HZ", "SINE 2 KHZ" };
/* phase increment per 8 kHz frame: f * 65536 / 8000 */
static const uint16_t tone_inc[6] = { 3605, 8192, 3605, 8192, 2048, 16384 };

static void draw_vol(uint8_t y)
{
    uint8_t i;
    clr(y, 10);
    txt(4, y + 1, "VOL", C_DIM);
    num(28, y + 1, vol, C_SEL);
    for (i = 0; i < 32; i++)
        gfx_fill(52 + i * 2, y + 8 - (i >> 2), 1, 1 + (i >> 2), i < vol ? C_OK : RGB(28, 34, 60));
}

static void tone_info(void)
{
    clr(70, 36);
    txt(4, 72, "FIFO DEPTH", C_DIM);   num(76, 72, fifo_depth, C_FG);
    txt(4, 82, "WORDS/S", C_DIM);      num(76, 82, wps, C_FG);
    txt(4, 92, "WRITES/FRAME", C_DIM); num(86, 92, wpf, wpf_mode ? C_SEL : C_OK);
    if (dac_dead) txt(4, 102, "DAC STALLED", RGB(255, 60, 40));
}

static void tone_screen(void)
{
    uint16_t ph = 0, inc;
    uint8_t shape = 0, k = 0, key, redraw = 1;
    int16_t s;

    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("TONE");
    skin_footer("<< >> VOL  PLAY:WAVE");
    dac_on(0);
    for (;;) {
        if (redraw) {
            clr(16, 12);
            txt(4, 18, tone_name[shape], C_FG);
            draw_vol(34);
            tone_info();
            inc = tone_inc[shape];
            redraw = 0;
        }
        s = (shape == 2 || shape == 3) ? ((ph & 0x8000) ? -9000 : 9000) : ((int16_t)sintab[ph >> 8] << 7);
        ph += inc;
        dac_put(s);
        if (wpf == 2) dac_put(s);
        if (++k & 127) continue;
        hal_watchdog();
        key = getkey();
        if (!key) continue;
        if (key == KEY_MENU) break;
        if (key == KEY_LEFT && vol) vol--;
        else if (key == KEY_RIGHT && vol < 31) vol++;
        else if (key == KEY_VOL) vol = vol < 28 ? vol + 3 : 0;
        else if (key == KEY_PLAY && !key_rep) { shape = shape < 5 ? shape + 1 : 0; ph = 0; }
        else continue;
        vol_apply();
        redraw = 1;
    }
    dac_off();
    regs_restore();
}

/* ---- MIC / FM record + playback ---------------------------------------------------------------- */
#define BUFSZ 9100
static uint8_t recbuf[BUFSZ];
static uint16_t rec_len;                    /* valid bytes (= samples at 8 kHz) */
static uint16_t f10 = 987;                  /* FM frequency in 100 kHz units */

enum { S_LIVE, S_REC };

static void draw_secs(uint8_t x, uint8_t y, uint16_t n, uint16_t fg)    /* n samples @ 8 kHz -> s.t */
{
    uint16_t t = n / 800;
    gfx_fill(x, y, 30, 8, C_BG);
    num(x, y, t / 10, fg);
    gfx_char(x + 6, y, '.', 1, fg, C_BG);
    num(x + 12, y, t % 10, fg);
}

static void draw_fm(void)
{
    clr(18, 22);
    txt(4, 18, "FM", C_DIM);
    num(22, 18, f10 / 10, C_SEL);
    gfx_char(40, 18, '.', 1, C_SEL, C_BG);
    num(46, 18, f10 % 10, C_SEL);
    txt(58, 18, "MHZ", C_SEL);
    txt(4, 29, tea_ok ? "TUNER ACK" : "NO TUNER ACK", tea_ok ? C_OK : RGB(255, 60, 40));
}

static void draw_mic(void)
{
    clr(18, 22);
    txt(4, 18, "MIC GAIN", C_DIM);
    txt(58, 18, gain_txt[mic_gain], C_SEL);
}

static void play_back(uint8_t fm)
{
    uint16_t i;
    uint8_t k = 0, key, redraw = 1;
    int16_t s;

    if (fm_up) fm_mon_off();
    gfx_fill(0, 52, LCD_W, 66, C_BG);
    txt(4, 54, "PLAYBACK", C_OK);
    skin_footer("<< >> VOL  PLAY:STOP");
    dac_on(0);
    for (i = 0; i < rec_len; ) {
        if (redraw) { draw_vol(70); redraw = 0; }
        s = ulaw_dec(recbuf[i++]) >> 1;
        dac_put(s);
        if (wpf == 2) dac_put(s);
        if (++k & 127) continue;
        hal_watchdog();
        key = getkey();
        if (!key) continue;
        if (key == KEY_PLAY && !key_rep) break;
        if (key == KEY_MENU) break;
        if (key == KEY_LEFT && vol) vol--;
        else if (key == KEY_RIGHT && vol < 31) vol++;
        else if (key == KEY_VOL) vol = vol < 28 ? vol + 3 : 0;
        else continue;
        vol_apply();
        redraw = 1;
    }
    dac_off();
    ADC2 = (ADC2 & 0xE3);
    if (fm) fm_mon_on();
    adc_flush();
}


/* one screen for both sources. LIVE: level meter (+ FM monitor). PLAY = record, PLAY again or buffer
 * full = stop and play back. MIC: << >> gain, VOL replays.  FM: << >> frequency, VOL volume. */
static void live_labels(uint8_t fm, uint8_t rec)
{
    uint8_t x = fm ? 96 : 4;                 /* FM: right of the TUNER line (meter is at MY = 44) */
    gfx_fill(x, 30, 28, 8, C_BG);
    if (rec) txt(x, 30, "REC", RGB(255, 60, 40));
    else txt(x, 30, "LIVE", C_OK);
}

static void src_static(uint8_t fm)
{
    gfx_fill(0, 52, LCD_W, 54, C_BG);
    txt(4, 56, "RAW", C_DIM);
    txt(4, 66, "RATE", C_DIM);
    txt(4, 76, "STORED", C_DIM);
    if (rec_len) draw_secs(46, 76, rec_len, C_FG);
    draw_vol(96);
    skin_footer(fm ? "<< >> FREQ PLAY:REC" : "<< >> GAIN PLAY:REC");
}

static void src_screen(uint8_t fm)
{
    uint16_t win = 0, spin = 0, cnt = 0, t_rate, t_fm, t_sec = 0;
    uint16_t peak = 0, hold = 0, a, raw = 0;
    int16_t s, x, d, dc = 0;
    uint8_t st = S_LIVE, key, first = 1, nodata = 1, quit = 0, do_play = 0, hi, lo;

    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title(fm ? "FM RADIO" : "MICROPHONE");
    bar_reset();
    src_static(fm);
    if (fm) {
        bus_open();
        tea_tune(f10, 0);
        draw_fm();
        wait_ticks(20);
    } else draw_mic();
    adc_on(fm);
    if (fm) fm_mon_on();
    live_labels(fm, 0);
    t_rate = t_fm = hal_ticks();

    while (!quit) {
        if (ADCFIFO & 0x20) {
            hi = ADCD; lo = ADCD;
            s = (int16_t)(((uint16_t)hi << 8) | lo);
            cnt++; spin = 0; nodata = 0;
            raw = (uint16_t)s;
            x = s >> 1;
            if (first) { dc = x; first = 0; }
            d = x - dc;
            dc += d >> 6;
            a = d < 0 ? -d : d;
            if (a > peak) peak = a;
            if (st == S_REC) {
                recbuf[rec_len++] = ulaw_enc((d >> 1) << (fm ? 0 : 1));
                if (rec_len >= BUFSZ) { do_play = 1; win = 256; }
            }
            if (++win < 256) continue;
        } else if (++spin) continue;

        /* ---- service: every 256 samples (32 ms), or when the ADC is silent ---------------------- */
        win = 0; spin = 0;
        hal_watchdog();
        if (peak > hold) hold = peak; else hold -= hold >> 3;
        bar_set((uint8_t)(lvl_of(hold) * 2));
        peak = 0;
        if ((uint16_t)(hal_ticks() - t_rate) >= 100) {
            t_rate = hal_ticks();
            gfx_fill(46, 66, 76, 8, C_BG);
            num(46, 66, cnt, cnt > 7000 && cnt < 9000 ? C_OK : RGB(255, 60, 40));
            txt(76, 66, nodata ? "NO DATA" : "S/S", C_DIM);
            gfx_fill(46, 56, 76, 8, C_BG);
            hex4(46, 56, raw, C_FG);
            hex4(80, 56, (uint16_t)dc, C_DIM);
            cnt = 0; nodata = 1;
        }
        if (st == S_REC) {
            if (rec_len / 800 != t_sec) { t_sec = rec_len / 800; draw_secs(46, 76, rec_len, C_SEL); }
        } else if (fm && (uint16_t)(hal_ticks() - t_fm) >= 50) {
            t_fm = hal_ticks();
            if (tea_read()) {
                gfx_fill(60, 29, 66, 8, C_BG);
                txt(60, 29, (tea_r[0] & 0x80) ? "RDY" : "---", C_FG);
                txt(84, 29, "LV", C_DIM);
                num(100, 29, tea_r[3] >> 4, C_SEL);
            }
        }
        key = getkey();
        if (key == KEY_MENU) quit = 1;
        else if (key == KEY_PLAY && !key_rep) {
            if (st == S_LIVE) {
                st = S_REC; rec_len = 0; t_sec = 0xFFFF;
                live_labels(fm, 1);
                skin_footer("PLAY:STOP+PLAYBACK");
            } else do_play = 1;
        } else if (key == KEY_VOL && !key_rep) {
            if (fm) { vol = vol < 28 ? vol + 3 : 0; vol_apply(); draw_vol(96); }
            else if (st == S_LIVE && rec_len) do_play = 1;
        } else if (key && st == S_LIVE && (key == KEY_LEFT || key == KEY_RIGHT)) {
            if (fm) {
                if (key == KEY_LEFT) f10 = f10 > 875 ? f10 - 1 : 1080;
                else f10 = f10 < 1080 ? f10 + 1 : 875;
                tea_tune(f10, 0); draw_fm();
            } else {
                mic_gain = (key == KEY_LEFT) ? (mic_gain ? mic_gain - 1 : 7) : ((mic_gain + 1) & 7);
                ADC1 = (ADC1 & 0xF8) | mic_gain;
                draw_mic();
            }
        }
        if (do_play) {
            do_play = 0; st = S_LIVE;
            if (rec_len) { draw_secs(46, 76, rec_len, C_FG); play_back(fm); }
            src_static(fm);
            live_labels(fm, 0);
            first = 1; hold = 0; bar_reset();
            t_rate = hal_ticks(); cnt = 0;
        }
    }
    if (fm_up) fm_mon_off();
    adc_off();
    if (fm) { tea_tune(f10, 1); bus_close(); }      /* tuner to standby */
    regs_restore();
}

/* ---- PORT DUMP: stock state at module entry vs now ---------------------------------------------- */
static void regs_screen(void)
{
    static const char *const nm[15] = { "42", "80", "81", "8B", "9E", "9F", "D2", "D3", "D4", "D5", "D6", "D7", "D9", "DD", "9B" };
    uint8_t i, x, y, now[15], key;
    gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
    skin_title("PORTS ENTRY / NOW");
    txt(4, 106, "ENTRY=STOCK STATE", C_DIM);
    skin_footer("PLAY:NEW  MENU:BACK");
    do {
        now[0] = P_CLK; now[1] = DACCTL; now[2] = DACSR; now[3] = FIFOST; now[4] = VOLR; now[5] = ADCMOD;
        now[6] = PD2; now[7] = ADCTUNE; now[8] = ADC1; now[9] = ADC2; now[10] = AING; now[11] = ADCFIFO;
        now[12] = PD9; now[13] = ANA; now[14] = ADCBUS;
        for (i = 0; i < 15; i++) {
            x = i < 8 ? 4 : 66;
            y = 16 + (i & 7) * 11;
            gfx_fill(x, y, 60, 9, C_BG);
            txt(x, y, nm[i], C_DIM);
            hex2(x + 16, y, sv[i], C_FG);
            hex2(x + 38, y, now[i], now[i] == sv[i] ? C_DIM : C_SEL);
        }
        do { key = getkey(); hal_watchdog(); hal_idle(); } while (!key);
        if (key == KEY_MENU) break;
    } while (key == KEY_PLAY);
}

/* ---- main menu -------------------------------------------------------------------------------- */
static const char *const item[5] = { "TONE GENERATOR", "MIC: METER+REC", "FM: METER+REC", "DAC WORDS/FRAME", "PORT DUMP" };
static const char *const wpf_txt[3] = { "AUTO", "1", "2" };

static void menu_draw(uint8_t sel)
{
    uint8_t i;
    for (i = 0; i < 5; i++) {
        uint16_t bg = i == sel ? C_SELBG : C_BG;
        gfx_fill(0, 18 + i * 14, LCD_W, 13, bg);
        gfx_text(6, 21 + i * 14, item[i], 1, i == sel ? C_SELFG : C_FG, bg);
        if (i == 3) gfx_text(100, 21 + i * 14, wpf_txt[wpf_mode], 1, C_OK, bg);
    }
}

uint16_t mod_main(void)
{
    uint8_t sel = 0, key;
    regs_save();
    skin_load();
    for (;;) {
        gfx_fill(0, 0, LCD_W, LCD_H, C_BG);
        skin_title("AUDIO TEST");
        txt(4, 96, "PAGE", C_DIM);
        num(34, 96, page_get(), C_FG);
        txt(4, 106, "KEEP VOLUME LOW", C_DIM);
        skin_footer("<< >> MOVE PLAY:GO");
        menu_draw(sel);
        for (;;) {
            do { key = getkey(); hal_watchdog(); hal_idle(); } while (!key);
            if (key == KEY_MENU) { regs_restore(); return 0; }
            if (key == KEY_LEFT) sel = sel ? sel - 1 : 4;
            else if (key == KEY_RIGHT) sel = sel < 4 ? sel + 1 : 0;
            else if (key == KEY_PLAY && !key_rep) break;
            menu_draw(sel);
        }
        if (sel == 0) tone_screen();
        else if (sel == 1) src_screen(0);
        else if (sel == 2) src_screen(1);
        else if (sel == 3) { wpf_mode = wpf_mode < 2 ? wpf_mode + 1 : 0; measured = 0; if (wpf_mode) wpf = wpf_mode; }
        else regs_screen();
    }
}
