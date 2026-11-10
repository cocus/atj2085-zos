/* ATREC: AUDIOTST's microphone / FM radio page (test module, chained from the AUDIOTST launcher
 * which drew the title; ATS->sel = 1 microphone, 2 FM; MENU chains back). Live level meter (+ FM
 * monitor through the analog mixer), PLAY records 8 kHz u-law into RAM (~1.1 s), PLAY again or a full
 * buffer stops and plays it back. MIC: << >> gain, VOL replays. FM: << >> frequency, VOL volume.
 * The recording lives in this module's RAM: it is gone once the page goes back to the launcher. */
#include "../common/audiotst/atbase.c"
#include "../common/skinlite.c"
#include "../../core/module.h"
#include <string.h>

/* ---- ADC (OS ADC_Enable + AI_Enable): 8 kHz, right channel only, 16 bit ----------------------- */
/* mic_gain (at.h) D4.2-0: x22 x27 x32 x37 x42 x47 x52 x57 (chip default 5) */
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

static void tea_tune(uint16_t f, uint8_t standby)
{
    uint32_t k = (uint32_t)f * 100UL + 225UL;
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

/* ---- MIC / FM record + playback ---------------------------------------------------------------- */
#define BUFSZ 9100
static uint8_t recbuf[BUFSZ];
static uint16_t rec_len;                    /* valid bytes (= samples at 8 kHz) */

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

    clr(SKIN_TITLE_H, LCD_H - SKIN_TITLE_H);            /* (the launcher drew the title) */
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


uint16_t mod_main(void)
{
    if (ATS->magic == AT_MAGIC) src_screen(ATS->sel == 2);
    ATS->back = 1;
    memcpy(ATS->next, AT_ZM, 12);
    return MOD_CHAIN;
}
