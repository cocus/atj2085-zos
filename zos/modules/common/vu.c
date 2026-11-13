/* VU meter from the ADC (include from one module source). The Z80 never sees the radio's audio
 * (analog FM -> mixer -> amp) nor the DSP's PCM, so the ADC samples it: source FM input, or the
 * analog mixer output (MIX_SRC_MIXER; whether the DAC reaches it is a device question).
 * Level = average |sample| over the window (not the peak: music peaks sit near full scale),
 * on a 36 dB log scale, 0 dBFS = VU_W px. The bar takes one colour by zone: blue below -12 dB,
 * yellow up to -4.5 dB, red above. 16-bit maths only (small: MP3PLAY is ZRAM2-bound).
 *   vu_adc_on(src)  vu_adc_off()   vu_poll() often   vu_level() once per frame -> 0..VU_W
 *   vu_bar(x, y, px, off_colour)   rises at once, falls 4 px per call
 * VU_PX_ONLY defined before the #include: only vu_px() (REC/RECRUN capture themselves). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "../../apps/theme.h"
#include "regs.h"
#include "vu.h"

#ifndef VU_PX_ONLY                              /* (a module with its own capture: vu_px() only) */

#define VU_KEEP_MIX   (MIX_SRC_MASK | MIX_RATE_MASK)
#define VU_KEEP_AI    (AICTL_ADC | AICTL_DITHER)
static uint8_t sv_mix, sv_ai, sv_pd2, sv_pd9;
static uint16_t vsum;
static uint8_t vcnt, vlen;

void vu_adc_on(uint8_t src)
{
    sv_mix = MIXER; sv_ai = AICTL; sv_pd2 = ADCPD2; sv_pd9 = ADCPD9;
    MIXER = (MIXER & ~VU_KEEP_MIX) | src;     /* 8 kHz */
    ADCTUNE |= ADCTUNE_GAIN;
    ADCBUS = 0;                                  /* 16 bit */
    ADCPD2 |= ADCPD2_ON;                         /* OS ADC_Enable */
    ADCPD9 &= ~ADCPD9_MASK;
    AICTL |= VU_KEEP_AI;
    ADCFIFO = ADCFIFO_RONLY;                     /* FIFO reset, then run + sync, right only */
    ADCFIFO = ADCFIFO_SYNC | ADCFIFO_RONLY | ADCFIFO_RUN;
    ADCFIFO = ADCFIFO_RONLY | ADCFIFO_RUN;
    vsum = vcnt = vlen = 0;
}

void vu_adc_off(void)
{
    ADCFIFO = ADCFIFO_OFF;
    ADCPD2 = sv_pd2; ADCPD9 = sv_pd9;
    AICTL = (AICTL & ~VU_KEEP_AI) | (sv_ai & VU_KEEP_AI);
    MIXER = (MIXER & ~VU_KEEP_MIX) | (sv_mix & VU_KEEP_MIX);
}

void vu_poll(void)
{
    uint8_t n = 16, hi, lo;
    int16_t s;
    while (n-- && (ADCFIFO & ADCFIFO_NE)) {
        hi = ADCDATA; lo = ADCDATA;
        s = (int16_t)(((uint16_t)hi << 8) | lo);
        if (vcnt < 63) { vsum += (uint16_t)(s < 0 ? -s : s) >> 5; vcnt++; }   /* fits 16 bits */
    }
}

#endif

/* 0..32767 -> px: 1.5 dB steps (4 per bit), 0 dBFS (~61) = VU_W, 36 dB range */
uint8_t vu_px(uint16_t v)
{
    uint8_t b = 0, l;
    uint16_t q = v;
    while (q) { b++; q >>= 1; }
    if (b < 3) return 0;
    l = b * 4 + ((v >> (b - 3)) & 3);
    if (l <= 37) return 0;
    l = (l - 37) * 5;
    return l > VU_W ? VU_W : l;
}

#ifndef VU_PX_ONLY
uint8_t vu_level(void)
{
    uint16_t avg = vcnt ? (vsum / vcnt) << 5 : 0;
    vsum = vcnt = 0;
    return vu_px(avg);
}

void vu_bar(uint8_t x, uint8_t y, uint8_t n, uint16_t off)
{
    if (n + 4 < vlen) n = vlen - 4;
    vlen = n;
    gfx_fill(x, y, n, VU_H, n < VU_W * 2 / 3 ? VU_C_LOW : n < VU_W * 26 / 30 ? VU_C_MID : VU_C_HIGH);
    gfx_fill(x + n, y, VU_W - n, VU_H, off);              /* gfx_fill ignores w = 0 */
}
#endif
