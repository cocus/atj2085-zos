/* USB Audio Class 1: capture from the ATJ2085 ADC (MIC / FM) or playback to the DAC (DAC).
 * Capture: capture.c (shared with the FM module): the ADC at 32 kHz, a ring in ZRAM1, 1 ms USB
 * frames on EP2 IN (isochronous). Playback: EP1 OUT isochronous packets (48 or 44.1 kHz stereo 16-bit) go
 * into a 1 KB ring in ZRAM1; dacfeed.s pushes the ring into the DAC FIFO (the DAC has no DMA and
 * no interrupt: the CPU polls). Host and DAC clocks drift: the ring absorbs it, a full ring drops
 * the excess (counted), an empty one re-primes. */
#include <stdint.h>
#include "../usb/usb.h"
#include "../../core/config.h"
#include "../../core/module.h"
#include "capture.h"

SFR(GPC_EN, P_GPC_EN);
SFR(GPC_D, P_GPC_D);
SFR(PLLCTL, P_PLLCTL);
SFR(DACCTL, P_DACCTL);
SFR(DACRATE, P_DACRATE);
SFR(DACFIFO, P_DACFIFO);
SFR(DAC_L, P_DAC_L);
SFR(DAC_M, P_DAC_M);
SFR(DAC_H, P_DAC_H);
SFR(PAVOL, P_PAVOL);
SFR(PACTL, P_PACTL);
SFR(CLKCTL2, P_CLKCTL2);

#define TEA_ADDR         0x60
#define TEA_STBY         0x40
#define TEA_XTAL_HCC_SNC 0x16
#define GPC_MASK         0x07
#define GPC_EN_MASK      0x77

uint8_t audio_tuner_ok;

static uint8_t active, pending;
static uint8_t sv_gpc_en, sv_gpc_d, radio_source;
static uint8_t dac_streaming;               /* DAC mode: alt 1 selected */
uint8_t audio_test;                         /* USB TEST source (main.c) */
static __at(HAL_MODRAM_BASE) zr_t zra;     /* = *ZR (SDCC: no constant-pointer arithmetic) */
static void dac_stream(uint8_t on);

/* ISO endpoints: one packet per arm from/to the fixed start address; for OUT the byte count of
 * the packet is the CBC read back after it (USB TEST source: verified for IN on the device) */
static uint16_t cbc_of(uint8_t epi)
{
    port_out(P_USB_EPI, epi);
    return ((uint16_t)port_in(P_USB_CNTH) << 8) | port_in(P_USB_CNTL);
}
uint16_t audio_dbg;                       /* last EP1 OUT packet size (screen, DAC mode) */
uint8_t dac_mono;                         /* dacfeed.s: one DAC word per frame (main.c << >>) */
uint8_t audio_peak;                       /* DAC: largest |sample| high byte since the last screen update */

static void delay(void)
{
    volatile uint8_t i;
    for (i = 4; i; i--) ;
}

static void gpio_set(uint8_t set, uint8_t clear)
{
    GPC_D = ((GPC_D & GPC_MASK) | set) & ~clear & GPC_MASK;
}

static void gpio_enable(uint8_t set, uint8_t clear)
{
    GPC_EN = ((GPC_EN & GPC_EN_MASK) | set) & ~clear & GPC_EN_MASK;
}

static void scl(uint8_t high)
{
    if (high) gpio_set(GPC_SCL, 0);
    else gpio_set(0, GPC_SCL);
}

static void sda(uint8_t high)
{
    if (high) gpio_set(GPC_SDA, 0);
    else gpio_set(0, GPC_SDA);
    gpio_enable(GPC_SDA, GPC_SDA << 4);
}

static void sda_release(void)
{
    gpio_enable(GPC_SDA << 4, GPC_SDA);
}

static uint8_t sda_read(void)
{
    return (GPC_D & GPC_SDA) ? 1 : 0;
}

static void i2c_start(void)
{
    sda(1); scl(1); delay(); sda(0); delay(); scl(0); delay();
}

static void i2c_stop(void)
{
    scl(0); sda(0); delay(); scl(1); delay(); sda(1); delay();
}

static uint8_t i2c_write(uint8_t value)
{
    uint8_t i, ack;
    for (i = 0; i < 8; i++) {
        sda(value & 0x80);
        value <<= 1;
        delay(); scl(1); delay(); scl(0);
    }
    sda_release(); delay(); scl(1); delay();
    ack = !sda_read();
    scl(0); delay(); sda(0);
    return ack;
}

/* the FM module's station (FM_STATE, same layout as modules/fm): tuned here too, << >> while running */
typedef struct { uint8_t magic, vol; uint16_t f10; } fm_state_t;
static __at(FM_STATE) fm_state_t fmst;
#define FM_MAGIC  0xF4
#define FREQ_MIN  875
#define FREQ_MAX  1080
uint16_t audio_f10 = 987;                  /* 100 kHz units */

static void radio_tune(uint8_t standby)
{
    uint32_t khz = (uint32_t)audio_f10 * 100UL + 225UL;
    uint16_t pll = (uint16_t)((khz * 125UL + 512UL) >> 10);
    uint8_t data[5], i, ok;
    data[0] = (uint8_t)(pll >> 8);
    data[1] = (uint8_t)pll;
    data[2] = 0x10;
    data[3] = TEA_XTAL_HCC_SNC | (standby ? TEA_STBY : 0);
    data[4] = 0;
    i2c_start();
    ok = i2c_write(TEA_ADDR << 1);
    for (i = 0; ok && i < 5; i++) ok = i2c_write(data[i]);
    i2c_stop();
    audio_tuner_ok = ok;
}

static void radio_bus_open(void)
{
    sv_gpc_en = GPC_EN & GPC_EN_MASK;
    sv_gpc_d = GPC_D & GPC_MASK;
    gpio_set(GPC_SCL | GPC_SDA, 0);
    gpio_enable(GPC_SCL | GPC_SDA, (GPC_SCL | GPC_SDA) << 4);
}

static void radio_bus_close(void)
{
    GPC_D = ((GPC_D & ~GPC_MASK) | sv_gpc_d) & GPC_MASK;
    GPC_EN = ((GPC_EN & ~GPC_EN_MASK) | sv_gpc_en) & GPC_EN_MASK;
}

static void audio_stop(void)
{
    if (!active) return;
    if (audio_test) { ep_disarm(EPI_EP2IN); active = pending = 0; return; }
    cap_stream(0);
    cap_adc_off();
    if (radio_source) {
        radio_tune(1);
        radio_bus_close();
    }
    active = 0;
}

/* USB TEST source: all of ZRAM2 (the SIE's 4 KB) holds 16-bit words equal to their own USB address,
 * the IN endpoint is armed with a FIXED start address: each recorded sample is the address the SIE
 * read (diagnoses the ISO addressing on the device) */

static void test_fill(void)
{
    uint16_t a, i;
    uint8_t *b = zra.tx;
    for (a = 0; a < 0x1000; a += 512) {
        for (i = 0; i < 512; i += 2) { b[i] = (uint8_t)(a + i); b[i + 1] = (uint8_t)((a + i) >> 8); }
        zr_put(a, b, 512);
    }
}

static void audio_start(void)
{
    if (audio_test) {
        test_fill();
        port_out(P_USB_EPI, EPI_EP2IN);
        port_out(P_USB_EPMODE, EPMODE_ISO | EPMODE_ISONBR);
        pending = 0;
        active = 1;
        return;
    }
    radio_source = usb_fn == FN_AUDIO_FM;
    if (radio_source) {
        if (fmst.magic == FM_MAGIC && fmst.f10 >= FREQ_MIN && fmst.f10 <= FREQ_MAX) audio_f10 = fmst.f10;
        radio_bus_open();
        radio_tune(0);
    }
    cap_adc_on(radio_source);
    cap_stream(1);
    active = 1;
}

void usb_audio_reset(void)
{
    if (dac_streaming) dac_stream(0);
    audio_stop();
    audio_alt_cur = 0;
    audio_tuner_ok = 0;
}

void usb_audio_configured(void)
{
    usb_audio_reset();
}

void audio_alt(uint8_t value)             /* SET_INTERFACE(1, value): capture.c */
{
    if (usb_fn == FN_AUDIO_DAC) dac_stream(value);
    else if (value) audio_start();
    else audio_stop();
}

/* ---- playback ------------------------------------------------------------------------------- */
#define DAC_PKT     200                   /* ISO OUT buffer (descriptor: 196 = 49 frames x 4) */
#define DAC_CHUNK   48                    /* ZRAM2 -> ring copy: 4 pieces per packet, FIFO fed between */
#define DAC_RING    1024                  /* ZR->tx + ZR->sec (adjacent, unused in audio mode) */
#define DAC_PRIME   384                   /* bytes buffered before the DAC starts (2 ms) */
#define SAMPLING_FREQ_CONTROL 0x01
#define REQ_SET_CUR 0x01
#define REQ_GET_CUR 0x81
#define REQ_GET_MIN 0x82
#define REQ_GET_MAX 0x83
#define REQ_GET_RES 0x84
#define RATE_441    44100UL
#define RATE_48     48000UL

uint8_t *dac_rd, *dac_wr, *dac_ring, *dac_end;
uint16_t audio_under;                     /* DAC ring ran empty while playing */
uint32_t audio_words;                     /* DAC FIFO words written (rate check: ~96000/s = stereo) */
void dac_feed(void);                      /* dacfeed.s */
static uint8_t dac_go, dac_441, dac_powered;
/* two OUT buffers at fixed USB offsets (both banks stay with the SIE): when a packet is in, the next
 * buffer is armed at once and the full one is copied out in two fast pieces right away (~0.1 ms,
 * the next packet is 1 ms away). Splitting the banks (one each) gave silence: the bank views are
 * not what the READ(10) ping-pong suggests for this direction (unverified). */
static uint8_t dac_buf;
#define DAC_BUF(b) ((b) ? 0x100 : 0x000)
static uint8_t sv_pll, sv_dacctl, sv_dacrate, sv_dacfifo, sv_pavol, sv_pactl;

static void wait_ticks(uint8_t n)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < n) hal_watchdog();
}

static void dac_rate(uint8_t r441)        /* DAC clock family + rate code, FIFO restarted */
{
    DACCTL |= DAC_MUTE;
    PLLCTL = (PLLCTL & ~PLL_AUDIO_MASK) | (r441 ? PLL_AUDIO_441 : 0);
    DACRATE = r441 ? DACRATE_441 : DACRATE_48;
    DACFIFO &= ~DACFIFO_RUN;
    DACFIFO |= DACFIFO_RUN;
    DACCTL &= ~DAC_MUTE;
    dac_441 = r441;
}

/* DAC calibration: 20000 zero words pushed as fast as the FIFO-full flag allows, timed: the DAC's
 * real word rate (9600 = 48 kHz with L and R words, 4800 = one word per frame) */
uint16_t dac_cal48, dac_cal441;
#define CAL_WORDS 20000u
static uint16_t dac_measure(void)           /* DAC words per second / 10: 20000 FIFO-paced writes */
{
    uint16_t t, n = 0;
    uint8_t k = 0;
    t = hal_ticks();
    while (n < CAL_WORDS) {
        if (!(DACFIFO & DACFIFO_FULL)) { DAC_M = 0; DAC_H = 0; n++; }
        if (!++k) hal_watchdog();             /* the loop runs ~0.2-0.5 s: watchdog 176 ms */
    }
    t = hal_ticks() - t;                      /* 10 ms ticks */
    return t ? (uint16_t)((uint32_t)CAL_WORDS * 10 / t) : 0;   /* words/s / 10 */
}

/* DAC + headphone amplifier on (before USB connects: SET_INTERFACE must answer within 50 ms),
 * anti-pop as the player: amplifier up silent, then fade the volume in */
void dac_power(uint8_t on)
{
    uint8_t vol = ZCFG->magic == ZCFG_MAGIC && ZCFG->vol <= PAVOL_MAX ? ZCFG->vol : VOL_DEF;
    if (on && !dac_powered) {
        sv_pll = PLLCTL; sv_dacctl = DACCTL; sv_dacrate = DACRATE; sv_dacfifo = DACFIFO;
        sv_pavol = PAVOL; sv_pactl = PACTL;
        DACCTL = DAC_INIT | DAC_MUTE;
        DACFIFO = DACFIFO_INIT;
        DAC_L = 0;
        PAVOL &= ~PAVOL_MASK;
        dac_rate(1);                             /* how many FIFO words does the DAC take per second? */
        dac_cal441 = dac_measure();              /*   (silent: volume 0, amplifier still off) */
        dac_rate(0);
        dac_cal48 = dac_measure();
        PACTL |= PA_DAC | PA_EN;
        PACTL |= PA_ON;
        wait_ticks(20);
        while ((PAVOL & PAVOL_MASK) < vol) { PAVOL++; wait_ticks(1); }
        dac_powered = 1;
    } else if (!on && dac_powered) {
        while (PAVOL & PAVOL_MASK) { PAVOL--; wait_ticks(1); }
        DACCTL |= DAC_MUTE;
        PACTL = sv_pactl;
        PAVOL = sv_pavol;
        DACFIFO = sv_dacfifo;
        DACRATE = sv_dacrate;
        PLLCTL = sv_pll;
        DACCTL = sv_dacctl;
        dac_powered = 0;
    }
}

static void dac_stream(uint8_t on)        /* SET_INTERFACE alt 1 / 0 */
{
    dac_ring = zra.tx;
    dac_end = zra.tx + DAC_RING;
    dac_rd = dac_wr = dac_ring;
    dac_go = 0;
    dac_buf = 0;
    if (on) ep_arm(EPI_EP1OUT, EPMODE_ISO, MPS_BULK, DAC_BUF(0), DAC_PKT);
    else { ep_disarm(EPI_EP1OUT); port_out(P_USB_EPIRQ, EPB_EP1OUT); }
    dac_streaming = on;
}

static uint16_t dac_used(void) { return (uint16_t)(dac_wr - dac_rd) & (DAC_RING - 1); }

static void dac_service(void)
{
    uint8_t *old = dac_rd;
    if (dac_go) {
        dac_feed();
        audio_words += ((uint16_t)(dac_rd - old) & (DAC_RING - 1)) >> 1;
        if (dac_rd == dac_wr) { dac_go = 0; audio_under++; }   /* underrun: buffer up again */
    } else if (dac_used() >= DAC_PRIME) dac_go = 1;
}

static void dac_poll(void)
{
    uint16_t n, k, room, off, src;
    dac_service();
    if (!dac_streaming || (port_in(P_USB_BR) & EPB_EP1OUT)) return;   /* no packet yet */
    n = cbc_of(EPI_EP1OUT);                            /* bytes of this packet */
    port_out(P_USB_EPIRQ, EPB_EP1OUT);
    audio_dbg = n;
    off = DAC_BUF(dac_buf);                            /* the full buffer */
    dac_buf ^= 1;
    ep_arm(EPI_EP1OUT, EPMODE_ISO, MPS_BULK, DAC_BUF(dac_buf), DAC_PKT);   /* next packet: armed at once */
    src = off;
    if (n > DAC_PKT) n = DAC_PKT;
    n &= ~3;                                             /* whole L/R frames */
    room = (DAC_RING - 4 - dac_used()) & ~3;
    if (n > room) { audio_dropped++; n = room; }
    for (off = 0; off < n; off += k) {
        k = n - off > DAC_CHUNK ? DAC_CHUNK : n - off;
        if (k > (uint16_t)(dac_end - dac_wr)) k = dac_end - dac_wr;
        zr_get(dac_wr, src + off, k);
        dac_wr += k;
        if (dac_wr == dac_end) dac_wr = dac_ring;
        dac_service();
    }
    audio_packets++;
}

/* class requests: the endpoint's sampling frequency (DAC mode only) */
uint8_t usb_audio_setup(void)
{
    uint8_t r[3], rq = usb_su[1];
    uint32_t hz;
    if (usb_fn != FN_AUDIO_DAC || usb_su[3] != SAMPLING_FREQ_CONTROL || (usb_su[4] & 0x7F) != 0x01)
        return 0;
    if (usb_su[0] == 0x22 && rq == REQ_SET_CUR) {
        if (ep0_out(3)) {
            hz = zra.ep0[0] | ((uint16_t)zra.ep0[1] << 8) | ((uint32_t)zra.ep0[2] << 16);
            dac_rate(hz == RATE_441);
            dac_rd = dac_wr = dac_ring;                  /* old-rate samples: drop */
            dac_go = 0;
        }
        return 1;
    }
    if (usb_su[0] == 0xA2 && rq >= REQ_GET_CUR && rq <= REQ_GET_RES) {
        hz = rq == REQ_GET_MIN ? RATE_441 : rq == REQ_GET_MAX ? RATE_48 : rq == REQ_GET_RES ? 1 :
             dac_441 ? RATE_441 : RATE_48;
        r[0] = (uint8_t)hz; r[1] = (uint8_t)(hz >> 8); r[2] = (uint8_t)(hz >> 16);
        ep0_reply(r, 3);
        return 1;
    }
    return 0;
}

void audio_tune(int8_t step)               /* FM: 100 kHz up / down, wraps; kept in FM_STATE */
{
    audio_f10 += step;
    if (audio_f10 < FREQ_MIN) audio_f10 = FREQ_MAX;
    if (audio_f10 > FREQ_MAX) audio_f10 = FREQ_MIN;
    if (fmst.magic == FM_MAGIC) fmst.f10 = audio_f10;
    if (active && radio_source) radio_tune(0);
}

void usb_audio_poll(void)
{
    if (usb_fn == FN_AUDIO_DAC) { dac_poll(); return; }
    if (!active) return;
    if (audio_test) {                                    /* fixed SA, static pattern */
        if (pending && (port_in(P_USB_BR) & EPB_EP2IN)) return;
        port_out(P_USB_EPIRQ, EPB_EP2IN);
        ep_arm(EPI_EP2IN, EPMODE_ISO | EPMODE_ISONBR, MPS_BULK, ZO_TX, AUDIO_BYTES);
        pending = 1;
        audio_packets++;
        return;
    }
    cap_poll();
}
