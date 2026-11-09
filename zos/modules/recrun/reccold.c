/* REC cold path (_CODE2, ZRAM1): ADENCM.DSP load, analog input + ADC rate, DSP start/stop.
 * Order and register values: the stock RECORD.AP (adagencInit 0B:2929, adagencStart 0C:2900)
 * for the DSP, modules/usbaudio/capture.c (cap_adc_on, microphone) for the ADC. */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "regs.h"
#include "rec.h"

SFR(C_IRQST, P_IRQST);
SFR(C_DSPCTL, P_DSPCTL);
SFR(C_DSPVER, P_DSPVER);
SFR(C_PLLCTL, P_PLLCTL);
SFR(C_ADCBUS, P_ADCBUS);
SFR(C_ADCMOD, P_ADCMOD);
SFR(C_AICTL, P_AICTL);
SFR(C_MIXER, P_MIXER);
SFR(C_ADCPD2, P_ADCPD2);
SFR(C_ADCTUNE, P_ADCTUNE);
SFR(C_ADCFIFO, P_ADCFIFO);
SFR(C_ADCPD9, P_ADCPD9);

#define REC_MIC_GAIN 5                          /* AICTL_MICGAIN, as capture.c */
#define FIFO_KEEP    0x97                       /* ADCFIFO bits restored (capture.c) */
static uint8_t sv_adcbus, sv_adcm, sv_aictl, sv_mixer, sv_pd2, sv_adctune, sv_adcfifo, sv_pd9, sv_pll;

/* ADC rate field + audio PLL per rate (ogg_rec_rds.md 2.3): 16 kHz and 32 kHz from the 48 kHz
 * family, 22.05 kHz = the 24 kHz code with PLL_AUDIO_441 (x 0.91875) */
static const uint8_t mix_rate[3] = { MIX_RATE_16, MIX_RATE_24, MIX_RATE_32 };
static const uint8_t pll_aud[3] = { 0, PLL_AUDIO_441, 0 };

/* ADENCM.DSP (ADEF) from the disk root into the DSP lanes (as MP3PLAY's loader) */
static const uint8_t lane_page[6] = { PAGE_IDM_L, PAGE_IDM_M, PAGE_IDM_H, PAGE_IPM_L, PAGE_IPM_M, PAGE_IPM_H };
#define ADEF_TABLE 0x10
uint8_t rec_load_dsp(void)
{
    fat_file_t f;
    fat_dirent_t e;
    xfer_t x;
    uint8_t *b = rb.q[1], i, k, sec, cnt;
    fat_root(&f);
    if (!fat_find(&f, "ADENCM  DSP", &e)) return 0;
    fat_open(&f, &e);
    if (fat_read(&f, rb.q[2], 512) != 512 || memcmp(rb.q[2], "ADEF", 4)) return 0;
    x.src = b; x.len = 512;
    for (i = 0; i < 6; i++) {
        sec = rb.q[2][ADEF_TABLE + 4 + i * 8];
        cnt = rb.q[2][ADEF_TABLE + 6 + i * 8];
        if (!fat_seek(&f, (uint32_t)sec << 9)) return 0;
        for (k = 0; k < cnt; k++) {
            hal_watchdog();
            if (fat_read(&f, b, 512) != 512) return 0;
            x.dst = PAGE_BASE + k * 512;        /* byte i of the section = DSP word i */
            page_write(lane_page[i], &x);
        }
    }
    return 1;
}

void rec_start(uint8_t ri)
{
    xfer_t x;
    /* analog input + ADC (capture.c cap_adc_on, microphone), rate per ri */
    sv_adcbus = C_ADCBUS; sv_adcm = C_ADCMOD; sv_aictl = C_AICTL; sv_mixer = C_MIXER;
    sv_pd2 = C_ADCPD2; sv_adctune = C_ADCTUNE; sv_adcfifo = C_ADCFIFO; sv_pd9 = C_ADCPD9;
    C_ADCMOD = ADCMOD_MIC;
    C_MIXER = (C_MIXER & ~(MIX_SRC_MASK | MIX_RATE_MASK)) | MIX_SRC_MIC | mix_rate[ri];
    C_ADCTUNE = (C_ADCTUNE & ~ADCTUNE_GAIN) | ADCTUNE_GAIN;
    C_ADCBUS = 0;
    C_ADCPD2 |= ADCPD2_ON;
    C_ADCPD9 &= ~ADCPD9_MASK;
    C_AICTL = (C_AICTL & ~(AICTL_MIC | AICTL_FM | AICTL_LINE | AICTL_MICGAIN)) |
              AICTL_ADC | AICTL_DITHER | AICTL_MIC | (rh.gain & AICTL_MICGAIN);
    /* encoder options: DM_PARAMS.. lane L = 0 (no VOX/AGC/noise gate, SDK basal_adag_en.c) */
    memset(rb.q[1], 0, N_PARAMS);
    x.dst = PAGE_BASE + DM_PARAMS; x.src = rb.q[1]; x.len = N_PARAMS;
    page_write(PAGE_IDMLO_L, &x);
    pcm_w = pcm_rd = pcm_full = 0; cap_p = rb.pcm[0]; cap_end = cap_p + PCM_CHUNK;
    q_head = q_tail = q_count = 0; hdr_idx = 0xFF;
    ee_pend = out_pend = stop_req = 0;
    nsamples = 0; drops = 0;
    settle = ri == RATE_32K ? 3200 : ri == RATE_22K ? 2205 : 1600;    /* 100 ms */
    C_ADCFIFO = ADCFIFO_RONLY;                  /* FIFO reset, then run + sync, right only */
    C_ADCFIFO = ADCFIFO_SYNC | ADCFIFO_RONLY | ADCFIFO_RUN;
    C_ADCFIFO = ADCFIFO_RONLY | ADCFIFO_RUN;
    rec_on = 1;
    C_IRQST = IRQ_DSP;                          /* drop a stale request */
    hal_irq_mask(IRQ_DSP, 0);
    C_DSPCTL |= DSPCTL_RUN;                     /* StartDsp: the DSP asks REQ_START first */
}

void rec_stop(void)
{
    rec_on = 0;
    hal_irq_mask(0, IRQ_DSP);
    C_IRQST = IRQ_DSP;
    C_DSPCTL &= ~DSPCTL_RUN;                    /* STOPDSP: reset, then clock off */
    C_DSPCTL &= ~(DSPCTL_RUN | DSPCTL_CLKEN);
    C_ADCFIFO = ADCFIFO_OFF;
    C_ADCPD2 = sv_pd2; C_ADCPD9 = sv_pd9; C_AICTL = sv_aictl; C_MIXER = sv_mixer;
    C_ADCTUNE = sv_adctune; C_ADCBUS = sv_adcbus; C_ADCMOD = sv_adcm;
    C_ADCFIFO = sv_adcfifo & FIFO_KEEP;
    C_PLLCTL = (C_PLLCTL & ~PLL_AUDIO_MASK) | (sv_pll & PLL_AUDIO_MASK);   /* PLL1 stays on */
}

/* DSP clock (RestDsp as MP3PLAY: 36 MIPS, PLL1 on) + the audio PLL family of the rate, then the
 * DSP held in reset with its clock on, ready for rec_load_dsp (MP3PLAY order). CPU at 24.576 MHz. */
void rec_dsp_reset(uint8_t ri)
{
    uint16_t t;
    sv_pll = C_PLLCTL;
    C_PLLCTL = (C_PLLCTL & ~(PLL_MIPS_MASK | PLL_AUDIO_MASK)) | PLL_MIPS_36 | PLL_PLL1EN | pll_aud[ri];
    t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < 6) hal_idle();     /* PLL settle (stock 2 ms) */
    C_DSPCTL = DSPCTL_INIT;
    C_DSPVER = DSPVER_INIT;
}
