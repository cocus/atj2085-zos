/* USB audio capture (UAC1, mono 16-bit 32 kHz, isochronous EP2 IN, asynchronous), shared by
 * USBAUDIO (MIC / FM sources) and the FM module's USB OUT (modules/fm/usbout.c includes this file).
 * The ADC runs at 32 kHz; its FIFO lasts ~375 us, so cap_poll() must run that often.
 * Capture ring: 256 samples in ZRAM1 (zr_put()'s source must be below 0x4000: the 0x4000 window
 * shows ZRAM2 during the copy, so module-page buffers send ZRAM2's own contents). Packets carry
 * 32 samples, 33/34 while the ring backs up (a late frame, ADC vs USB clock): asynchronous
 * endpoint, wMaxPacketSize 68 (desc.c). With fixed 32-sample packets and two buffers, every late
 * frame dropped a whole buffer (recorded: jumps on 32-sample boundaries, ~9/s = choppy FM). */
#include <stdint.h>
#include "../common/usb/usb.h"
#include "capture.h"


#define CAP_RING      512                /* bytes */
#define CAP_MIC_GAIN  5                  /* AICTL_MICGAIN field for the microphone */
#define CAP_FIFO_KEEP 0x97               /* ADCFIFO bits restored (not SYNC / NE / bit 3) */

uint16_t audio_packets, audio_dropped;
uint8_t cap_streaming, audio_alt_cur;
static uint8_t cap_pending, cap_adc;
static uint16_t cap_wr, cap_rd;           /* ring offsets (bytes) */
static uint16_t lvl_sum;
static uint8_t lvl_cnt;
static uint8_t sv_adcbus, sv_adcm, sv_aictl, sv_mixer, sv_pd2, sv_adctune;
static uint8_t sv_adcfifo, sv_pd9, sv_aigain, sv_pllcap;
static __at(HAL_MODRAM_BASE) zr_t czr;    /* = *ZR (SDCC: no constant-pointer arithmetic) */

void cap_adc_on(uint8_t fm)
{
    if (cap_adc) return;
    sv_adcbus = ADCBUS; sv_adcm = ADCMOD; sv_aictl = AICTL; sv_mixer = MIXER;
    sv_pd2 = ADCPD2; sv_adctune = ADCTUNE; sv_adcfifo = ADCFIFO; sv_pd9 = ADCPD9;
    sv_aigain = AIGAIN; sv_pllcap = PLLCTL;
    PLLCTL &= ~PLL_AUDIO_MASK;            /* 48 kHz family: else "32 kHz" is 29.4 kHz after a 44.1k MP3 */
    ADCMOD = fm ? ADCMOD_LINEFM : ADCMOD_MIC;
    MIXER = (MIXER & ~(MIX_SRC_MASK | MIX_RATE_MASK)) | (fm ? MIX_SRC_FM : MIX_SRC_MIC) | MIX_RATE_32;
    ADCTUNE = (ADCTUNE & ~ADCTUNE_GAIN) | ADCTUNE_GAIN;
    ADCBUS = 0;
    ADCPD2 |= ADCPD2_ON;
    ADCPD9 &= ~ADCPD9_MASK;
    AICTL = (AICTL & ~(AICTL_MIC | AICTL_FM | AICTL_LINE | AICTL_MICGAIN)) |
                AICTL_ADC | AICTL_DITHER | (fm ? AICTL_FM : AICTL_MIC | CAP_MIC_GAIN);
    if (fm) AIGAIN = (AIGAIN & ~AIGAIN_FM_MASK) | AIGAIN_FM_0DB;
    ADCFIFO = ADCFIFO_RONLY;              /* FIFO reset, then run + sync, right only */
    ADCFIFO = ADCFIFO_SYNC | ADCFIFO_RONLY | ADCFIFO_RUN;
    ADCFIFO = ADCFIFO_RONLY | ADCFIFO_RUN;
    cap_wr = cap_rd = 0;
    lvl_sum = lvl_cnt = 0;
    cap_adc = 1;
}

void cap_adc_off(void)
{
    if (!cap_adc) return;
    ADCFIFO = ADCFIFO_OFF;
    ADCPD2 = sv_pd2;
    ADCPD9 = sv_pd9;
    AICTL = sv_aictl;
    MIXER = sv_mixer;
    ADCTUNE = sv_adctune;
    ADCBUS = sv_adcbus;
    ADCMOD = sv_adcm;
    AIGAIN = sv_aigain;
    ADCFIFO = sv_adcfifo & CAP_FIFO_KEEP;
    PLLCTL = (PLLCTL & ~PLL_AUDIO_MASK) | (sv_pllcap & PLL_AUDIO_MASK);
    cap_adc = 0;
}

void cap_stream(uint8_t on)
{
    cap_pending = 0;
    cap_wr = cap_rd = 0;
    if (on) {
        USB_EPI = EPI_EP2IN;                  /* ISO from now on: zero-length packets until the */
        USB_EPMODE = EPMODE_ISO | EPMODE_ISONBR;   /*   first 32 samples are ready */
    } else if (cap_streaming) {
        ep_disarm(EPI_EP2IN);
        USB_EPIRQ = EPB_EP2IN;
    }
    cap_streaming = on;
}

/* ADC FIFO -> level sums (16-bit sums of |s| >> 5, up to 63 samples per audio_level) + ring */
static void capture_samples(void)
{
    uint8_t hi, lo;
    int16_t v;
    while (ADCFIFO & ADCFIFO_NE) {
        hi = ADCDATA;
        lo = ADCDATA;
        if (lvl_cnt < 63) {
            v = (int16_t)(((uint16_t)hi << 8) | lo);
            lvl_sum += (uint16_t)(v < 0 ? -v : v) >> 5;
            lvl_cnt++;
        }
        if (!cap_streaming) continue;
        if (((cap_wr - cap_rd) & (CAP_RING - 1)) >= CAP_RING - 4) { audio_dropped++; continue; }
        czr.sec[cap_wr] = lo;
        czr.sec[cap_wr + 1] = hi;
        cap_wr = (cap_wr + 2) & (CAP_RING - 1);
    }
}

uint16_t audio_level(void)
{
    uint16_t a = lvl_cnt ? (lvl_sum / lvl_cnt) << 5 : 0;
    lvl_sum = lvl_cnt = 0;
    return a;
}

void cap_poll(void)
{
    uint16_t used, n, k;
    capture_samples();
    if (!cap_streaming) return;
    if (cap_pending) {
        if (USB_BR & EPB_EP2IN) return;       /* the last packet is still armed */
        USB_EPIRQ = EPB_EP2IN;
        cap_pending = 0;
    }
    used = (cap_wr - cap_rd) & (CAP_RING - 1);
    if (used < AUDIO_BYTES) return;
    n = used > 3 * AUDIO_BYTES ? AUDIO_BYTES + 4 : used > 2 * AUDIO_BYTES ? AUDIO_BYTES + 2 : AUDIO_BYTES;
    k = CAP_RING - cap_rd;
    if (k > n) k = n;
    zr_put(ZO_TX, czr.sec + cap_rd, k);
    if (k < n) zr_put(ZO_TX + k, czr.sec, n - k);
    cap_rd = (cap_rd + n) & (CAP_RING - 1);
    ep_arm(EPI_EP2IN, EPMODE_ISO | EPMODE_ISONBR, MPS_BULK, ZO_TX, n);
    cap_pending = 1;
    audio_packets++;
}

/* GET_INTERFACE / SET_INTERFACE of the audio streaming interface (1): alt 0 / 1 -> audio_alt() */
uint8_t usb_audio_std_request(void)
{
    uint8_t request = usb_su[1], interface = usb_su[4], value = usb_su[2], alt_value;
    if (usb_su[5]) return 0;
    if (request == 10 && usb_su[0] == 0x81 && interface == 1) {
        alt_value = audio_alt_cur;
        ep0_reply(&alt_value, 1);
        return 1;
    }
    if (request == 11 && usb_su[0] == 0x01 && interface == 1 && !usb_su[3]) {
        if (value > 1) return 0;
        if (value != audio_alt_cur) {
            audio_alt(value);
            audio_alt_cur = value;
        }
        ep0_ack_class();
        return 1;
    }
    return 0;
}
