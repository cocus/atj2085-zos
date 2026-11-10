/* FM: radio (TEA5767 tuner on bit-banged I2C, GPIO C). The tuner's audio is analog: it goes from
 * the FM input pins through the analog mixer to the headphone amp, no DAC/ADC/DSP involved
 * (lisong_rebuild/analysis/audio_hw.md, i2c.md; register sequences from the stock OS AI_Enable /
 * PA_Enable). << >> tune 100 kHz (hold = keep going), PLAY seek up, VOL toggles volume mode
 * (<< >> volume, PLAY = USB OUT on/off), MENU back. Frequency and volume kept in FM_STATE.
 *
 * USB OUT (default: SETTINGS "FM USB OUT", ZCFG_FM_USB): with a cable the player enumerates as the
 * zos USB radio (USBAUDIO's FM source: UAC1 capture, 32 kHz mono, EP2 IN, modules/usbaudio/
 * capture.c + desc.c + the shared USB core) while the radio keeps playing. The ADC then runs at
 * 32 kHz and its FIFO lasts ~375 us: everything the loop does is cut into pieces with pump()
 * (USB service + capture) between them - text glyph by glyph, fills row by row, the I2C tuner
 * transfers every 4 bits. No key beep while USB OUT is on (beep() busy-waits ~35 ms). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../core/event.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../common/skin.h"
#include "regs.h"
#include "../common/vu.h"
#include "../common/draw.h"
#include "strings.h"
#include "../common/usb/usb.h"
#include "../usbaudio/capture.h"
#include "../../core/config.h"
#include "../common/beep.h"

void pump(void);
void big_char(uint8_t x, uint8_t y, char c, uint8_t sc, uint16_t fg, uint16_t bg);   /* skinlib.c */

SFR(PAVOL, P_PAVOL);
SFR(ADCMOD, P_ADCMOD);
SFR(AICTL, P_AICTL);
SFR(MIXER, P_MIXER);
SFR(AIGAIN, P_AIGAIN);
SFR(PACTL, P_PACTL);
SFR(DACCTL, P_DACCTL);
SFR(GPC_EN, P_GPC_EN);
SFR(GPC_D, P_GPC_D);

/* ---- TEA5767 ------------------------------------------------------------------------------------ */
#define TEA_ADDR      0x60
/* write byte 3 */
#define TEA3_HLSI     0x10      /* high-side injection */
#define TEA3_MS       0x08      /* forced mono */
/* write byte 4 */
#define TEA4_STBY     0x40
#define TEA4_XTAL     0x10      /* 32.768 kHz crystal */
#define TEA4_HCC      0x04      /* high cut */
#define TEA4_SNC      0x02      /* stereo noise cancelling */
/* read bytes */
#define TEAR0_RF      0x80      /* ready */
#define TEAR2_STEREO  0x80
#define TEAR2_IF      0x7F      /* IF counter: a real station has 0x31..0x3E */
#define TEAR3_LEV_SH  4         /* level 0-15 in bits 7-4 */
#define FREQ_MIN      875       /* 100 kHz units */
#define FREQ_MAX      1080
#define SEEK_LEVEL    7

#define GPC_EN_OUT    (GPC_SCL | GPC_SDA)               /* output enables (bits 0-2) */
#define GPC_EN_IN     ((GPC_SCL | GPC_SDA) << 4)        /* input enables (bits 4-6) */
#define GPC_EN_VALID  0x77
#define GPC_D_VALID   0x07

static uint8_t sv_en, sv_d, sv_pavol, sv_adcmod, sv_aictl, sv_mixer, sv_aigain, sv_pactl;

static void dly(void) { volatile uint8_t i; for (i = 4; i; i--) ; }
static void en_set(uint8_t set, uint8_t clr) { GPC_EN = ((GPC_EN & GPC_EN_VALID) | set) & ~clr & GPC_EN_VALID; }
static void d_set(uint8_t set, uint8_t clr) { GPC_D = ((GPC_D & GPC_D_VALID) | set) & ~clr & GPC_D_VALID; }
static void scl(uint8_t v) { if (v) d_set(GPC_SCL, 0); else d_set(0, GPC_SCL); }
static void sda(uint8_t v) { if (v) d_set(GPC_SDA, 0); else d_set(0, GPC_SDA); en_set(GPC_SDA, GPC_SDA << 4); }
static void sda_release(void) { en_set(GPC_SDA << 4, GPC_SDA); }
static uint8_t sda_read(void) { return (GPC_D & GPC_SDA) ? 1 : 0; }
static void i2c_start(void) { sda(1); scl(1); dly(); sda(0); dly(); scl(0); dly(); }
static void i2c_stop(void) { scl(0); sda(0); dly(); scl(1); dly(); sda(1); dly(); }
static uint8_t i2c_wr(uint8_t b)
{
    uint8_t i, ack;
    for (i = 0; i < 8; i++) { sda(b & 0x80); b <<= 1; dly(); scl(1); dly(); scl(0); if ((i & 3) == 3) pump(); }
    sda_release(); dly(); scl(1); dly();
    ack = !sda_read();
    scl(0); dly(); sda(0);
    return ack;
}
static uint8_t i2c_rd(uint8_t nack)
{
    uint8_t i, v = 0;
    sda_release();
    for (i = 0; i < 8; i++) { scl(1); dly(); v = (v << 1) | sda_read(); scl(0); dly(); if ((i & 3) == 3) pump(); }
    sda(nack); dly(); scl(1); dly(); scl(0); dly(); sda(0);
    return v;
}

static uint8_t tea_r[5], tea_ok;
static void tea_tune(uint16_t f10, uint8_t standby)
{
    uint32_t k = (uint32_t)f10 * 100UL + 225UL;               /* kHz + IF (high side) */
    uint16_t pll = (uint16_t)((k * 125UL + 512UL) >> 10);     /* stock CALCULATE_PLL: 4 * f / 32768 Hz */
    uint8_t w[5], i, ok;
    w[0] = (uint8_t)(pll >> 8); w[1] = (uint8_t)pll; w[2] = TEA3_HLSI;
    w[3] = TEA4_XTAL | TEA4_HCC | TEA4_SNC | (standby ? TEA4_STBY : 0); w[4] = 0;
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

/* ---- analog path ------------------------------------------------------------------------------- */
typedef struct { uint8_t magic; uint8_t vol; uint16_t f10; } fm_state_t;
static __at(FM_STATE) fm_state_t st;
#define FM_MAGIC 0xF4

static uint8_t usb_on;                          /* USB OUT running (device attached / waiting) */
static void wait_ticks(uint16_t t)
{
    uint16_t s = hal_ticks();
    while ((uint16_t)(hal_ticks() - s) < t) { hal_watchdog(); if (usb_on) pump(); else hal_idle(); }
}
static void vol_ramp(uint8_t to)                /* 1 step per tick: no pops */
{
    uint8_t v = PAVOL & PAVOL_MASK;
    while (v != to) {
        v += v < to ? 1 : -1;
        PAVOL = (PAVOL & ~PAVOL_MASK) | v;
        wait_ticks(1);
    }
}

static void radio_on(void)
{
    sv_en = GPC_EN; sv_d = GPC_D; sv_pavol = PAVOL; sv_adcmod = ADCMOD; sv_aictl = AICTL;
    sv_mixer = MIXER; sv_aigain = AIGAIN; sv_pactl = PACTL;
    d_set(GPC_SCL | GPC_SDA, 0);                /* I2C idle high */
    en_set(GPC_EN_OUT, GPC_EN_IN);
    PAVOL &= ~PAVOL_MASK;
    ADCMOD = ADCMOD_LINEFM;                     /* AI_Enable(fmin) + AI_SetGain(FM, 0 dB) */
    AICTL |= AICTL_FM;
    AIGAIN = (AIGAIN & ~AIGAIN_FM_MASK) | AIGAIN_FM_0DB;
    PACTL = PA_EN;                              /* PA_Enable(fmin), anti-pop: path, then the bias, */
    PACTL = PA_EN | PA_ON;                      /*   DAC off, volume 0, FM input still muted */
    wait_ticks(20);                             /* amplifier settles silently */
    MIXER |= MIX_FM;                            /* then FM into the mixer (volume fades in later) */
    vu_adc_on(MIX_SRC_FM);                      /* level meter: the ADC samples the FM input */
}
static void radio_off(void)
{
    vol_ramp(0);
    MIXER &= ~MIX_FM;                           /* input muted before the amplifier goes off */
    wait_ticks(2);
    tea_tune(st.f10, 1);
    vu_adc_off();
    PACTL = sv_pactl; MIXER = sv_mixer; AIGAIN = sv_aigain; AICTL = sv_aictl; ADCMOD = sv_adcmod;
    PAVOL = sv_pavol;
    GPC_D = (GPC_D & ~(GPC_SCL | GPC_SDA)) | (sv_d & (GPC_SCL | GPC_SDA));
    GPC_EN = (GPC_EN & ~(GPC_EN_OUT | GPC_EN_IN)) | (sv_en & (GPC_EN_OUT | GPC_EN_IN));
}

/* ---- USB OUT ---------------------------------------------------------------------------------- */
static uint8_t quit_req;                        /* MENU seen inside a USB control transfer wait */

/* USB service + capture (with USB OUT), else the VU's ADC FIFO. Every longjmp(usb_jb) (bus reset,
 * unplug, MENU) comes from inside usb_service(), so it lands here while this frame is alive. */
void pump(void)
{
    uint8_t ab;
    if (!usb_on) { vu_poll(); return; }
    ab = setjmp(usb_jb);
    if (ab == AB_STOP) quit_req = 1;
    else if (ab) usb_recover(ab);
    usb_service();
    cap_poll();
}

static void usb_out(uint8_t on)
{
    if (on == usb_on) return;
    if (on) {
        usb_start(FN_AUDIO_FM);                 /* 48 MHz SIE clock, pull-up when VBUS is there */
        vu_adc_off();
        cap_adc_on(1);                          /* ADC 32 kHz from the FM input: VU + USB */
        usb_on = 1;
    } else {
        usb_on = 0;
        cap_adc_off();
        usb_stop();                             /* pull-up off: the host sees an unplug */
        usb_stop_clock(0);
        vu_adc_on(MIX_SRC_FM);
    }
}

/* ---- screen (drawn in pieces: pump() between them) --------------------------------------------- */
static uint8_t volmode, lev_shown, st_shown, usb_shown, vlen, mk_x;
static char f_shown[5];

static void pfill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c)   /* row by row */
{
    for (; h; h--, y++) { gfx_fill(x, y, w, 1, c); pump(); }
}
static void ptext(uint8_t x, uint8_t y, const char *t, uint16_t fg, uint16_t bg)
{
    for (; *t; t++, x += FONT_W) { gfx_char(x, y, *t, 1, fg, bg); pump(); }
}

static void footer(void)
{
    pfill(0, LCD_H - 10, LCD_W, 10, sk.bg);
    ptext(4, LCD_H - 9, T(volmode ? S_FT_VOL : S_FT_TUNE), sk.dim, sk.bg);
}

static void draw_freq(void)                     /* changed digits only + the band marker */
{
    char t[5];
    uint8_t i, x;
    uint16_t f = st.f10;
    t[0] = f >= 1000 ? '1' : ' ';
    t[1] = '0' + (f / 100) % 10; t[2] = '0' + (f / 10) % 10; t[3] = '.'; t[4] = '0' + f % 10;
    for (i = 0; i < 5; i++)
        if (t[i] != f_shown[i]) { f_shown[i] = t[i]; big_char(10 + i * FONT_W * 3, 34, t[i], 3, sk.fg, sk.bg); }
    x = 4 + (uint8_t)((f - FREQ_MIN) * 117u / (FREQ_MAX - FREQ_MIN));
    if (x == mk_x) return;
    if (mk_x != 0xFF) { gfx_fill(mk_x, 63, 3, 9, sk.bg); gfx_fill(mk_x, 66, 3, 3, sk.dim); }
    gfx_fill(x, 63, 3, 9, sk.sel_bot);
    mk_x = x;
    pump();
}

static void draw_vol(void)
{
    ptext(4, 102, "VOL", volmode ? sk.sel_bot : sk.dim, sk.bg);
    pfill(28, 103, 93, 6, sk.bar);
    pfill(28, 103, st.vol * 3, 6, volmode ? sk.sel_bot : sk.fg);
}

static void draw_vu(uint8_t n)                  /* common/vu.c's bar, redrawn where it changed */
{
    uint16_t c, c0;
    if (n + 4 < vlen) n = vlen - 4;             /* falls slowly */
    c = n < VU_W * 2 / 3 ? VU_C_LOW : n < VU_W * 26 / 30 ? VU_C_MID : VU_C_HIGH;
    c0 = vlen < VU_W * 2 / 3 ? VU_C_LOW : vlen < VU_W * 26 / 30 ? VU_C_MID : VU_C_HIGH;
    if (c != c0) pfill(4, 91, n, VU_H, c);      /* colour zone changed: the whole lit part */
    else if (n > vlen) pfill(4 + vlen, 91, n - vlen, VU_H, c);
    if (n < vlen) pfill(4 + n, 91, vlen - n, VU_H, sk.bar);
    vlen = n;
}

static void draw_usb(void)                      /* "USB": dim = waiting for a host, fg = enumerated, */
{                                               /*   accent = the PC is recording */
    uint8_t u = !usb_on ? 0 : cap_streaming ? 3 : usb_state == US_CONFIGURED ? 2 : 1;
    if (u == usb_shown) return;
    usb_shown = u;
    if (!u) pfill(52, 80, 3 * FONT_W, FONT_H, sk.bg);
    else ptext(52, 80, "USB", u == 3 ? sk.sel_bot : u == 2 ? sk.fg : sk.dim, sk.bg);
}

static void draw_status(void)                   /* signal bars + stereo, from the tuner */
{
    uint8_t lev = 0, ster = 0, i, h;
    if (tea_read()) { lev = tea_r[3] >> TEAR3_LEV_SH; ster = (tea_r[2] & TEAR2_STEREO) != 0; }
    if (lev != lev_shown) {
        lev_shown = lev;
        for (i = 0; i < 5; i++) {               /* 5 bars, 3 levels each */
            h = 2 + i * 2;
            gfx_fill(4 + i * 5, 88 - 10, 4, 10 - h, sk.bg);
            gfx_fill(4 + i * 5, 88 - h, 4, h, lev > i * 3 ? sk.sel_bot : sk.dim);
            pump();
        }
    }
    if (ster != st_shown) {
        st_shown = ster;
        spk_icon(34, 80, ster, ster ? sk.sel_bot : sk.dim, sk.bg);
        pump();
    }
}

static void screen(void)
{
    gfx_fill(0, 0, LCD_W, LCD_H, sk.bg);
    skin_title(T(S_TITLE));
    if (!tea_ok) gfx_text(4, 20, T(S_NOTUNER), 1, RGB(255, 60, 40), sk.bg);
    gfx_text(100, 48, "MHZ", 1, sk.dim, sk.bg);
    lev_shown = st_shown = mk_x = 0xFF;
    usb_shown = 0;
    f_shown[0] = f_shown[1] = f_shown[2] = f_shown[3] = f_shown[4] = 0;
    gfx_fill(4, 66, 120, 3, sk.dim);            /* band track */
    gfx_fill(4, 91, VU_W, VU_H, sk.bar);
    vlen = 0;
    draw_freq();
    draw_vol();
    footer();
}

static void tune(uint16_t f)
{
    if (f < FREQ_MIN) f = FREQ_MAX;
    if (f > FREQ_MAX) f = FREQ_MIN;
    st.f10 = f;
    tea_tune(f, 0);
    draw_freq();
}

/* software seek: step up until the level and the IF counter say "station" (any key stops) */
static void seek(void)
{
    event_t e;
    uint16_t f0 = st.f10;
    uint8_t ifc;
    PAVOL &= ~PAVOL_MASK;                       /* quiet while scanning */
    do {
        tune(st.f10 + 1);
        wait_ticks(3);
        if (skin_get(&e) && (e.type == EV_KEY_DOWN || e.type == EV_POWER)) {
            if (e.type == EV_POWER) MOD_POWER_REQ = 1;
            break;
        }
        if (!tea_read()) break;
        ifc = tea_r[2] & TEAR2_IF;
    } while (st.f10 != f0 && ((tea_r[3] >> TEAR3_LEV_SH) < SEEK_LEVEL || ifc < 0x31 || ifc > 0x3E));
    vol_ramp(st.vol);
}

static uint16_t t_vu, t_st;                     /* statics: pump() may longjmp */
uint16_t mod_main(void)
{
    event_t e;
    if (st.magic != FM_MAGIC || st.f10 < FREQ_MIN || st.f10 > FREQ_MAX || st.vol > PAVOL_MAX) {
        st.magic = FM_MAGIC; st.f10 = 987; st.vol = 14;
    }
    usb_on = quit_req = 0;
    skin_load();
    radio_on();
    tea_tune(st.f10, 0);
    screen();
    wait_ticks(10);
    vol_ramp(st.vol);
    if (ZCFG_FM_USB) usb_out(1);
    t_vu = t_st = hal_ticks();
    for (;;) {
        pump();
        if (quit_req) break;
        if ((uint16_t)(hal_ticks() - t_vu) >= 3) {                                 /* 30 ms */
            t_vu = hal_ticks();
            draw_vu(usb_on ? vu_px(audio_level()) : vu_level());
        }
        if ((uint16_t)(hal_ticks() - t_st) >= 30) { t_st = hal_ticks(); draw_status(); }  /* 300 ms */
        draw_usb();
        hal_watchdog();
        if (!skin_get(&e)) continue;
        if (e.type == EV_POWER) MOD_POWER_REQ = 1;           /* SHELL powers off */
        if (e.type == EV_QUIT || MOD_POWER_REQ) break;
        if (e.type != EV_KEY_DOWN && e.type != EV_KEY_REPEAT) continue;
        if (e.type == EV_KEY_DOWN && !usb_on) {               /* key beep (SETTINGS); not with USB */
            beep();                                          /*   OUT: it busy-waits ~35 ms */
            PAVOL = (PAVOL & ~PAVOL_MASK) | st.vol;          /* beep() leaves the amplifier at 0 */
        }
        if (e.code == KEY_MENU) break;
        if (e.code == KEY_VOL) { if (e.type == EV_KEY_DOWN) { volmode ^= 1; draw_vol(); footer(); } continue; }
        if (volmode) {
            if (e.code == KEY_PLAY) { if (e.type == EV_KEY_DOWN) usb_out(!usb_on); continue; }
            if (e.code == KEY_RIGHT && st.vol < PAVOL_MAX) st.vol++;
            else if (e.code == KEY_LEFT && st.vol) st.vol--;
            PAVOL = (PAVOL & ~PAVOL_MASK) | st.vol;
            draw_vol();
        } else if (e.code == KEY_RIGHT) tune(st.f10 + 1);
        else if (e.code == KEY_LEFT) tune(st.f10 - 1);
        else if (e.code == KEY_PLAY && e.type == EV_KEY_DOWN) seek();
    }
    usb_out(0);
    radio_off();
    return 0;
}
