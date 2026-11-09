/* MP3PLAY cold path: start / stop / MP3DEC load / first-request parameters. Linked into the
 * module's second code section (_CODE2, ZRAM1 0x2E80-0x33FF: free core RAM in takeover mode), which main.c loads from the .ZM file
 * (offset 0x1000) before anything else runs: ZRAM2 (4 KB) is full with the hot path. */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "regs.h"
#include "mp3.h"
#include "mp3int.h"

SFR(IRQST, P_IRQST);
SFR(DSPCTL, P_DSPCTL);
SFR(DSPVER, P_DSPVER);
SFR(PLLCTL, P_PLLCTL);
SFR(DACCTL, P_DACCTL);
SFR(DACRATE, P_DACRATE);
SFR(DACFIFO, P_DACFIFO);
SFR(PAVOL, P_PAVOL);
SFR(AICTL, P_AICTL);
SFR(MIXER, P_MIXER);
SFR(PACTL, P_PACTL);

#define bs mp3_bs
#define file mp3_file
#define dma_ram mp3_dma_ram
#define first_ee mp3_first_ee
#define pend mp3_pend
#define have mp3_have
#define wr8 mp3_wr8
#define pll_change mp3_pll_change
#define wait_ticks mp3_wait
#define SLOW() mp3_slow()
#define FAST() mp3_fast()
#define STEP(n, s)

/* ---- 1. MP3DEC.DSP (ADEF) from the disk root into the DSP lanes ---- */
static const uint8_t lane_page[6] = {          /* sections DMLS DMMS DMHS PMLS PMMS PMHS */
    PAGE_IDM_L, PAGE_IDM_M, PAGE_IDM_H, PAGE_IPM_L, PAGE_IPM_M, PAGE_IPM_H };
#define ADEF_TABLE 0x10                        /* 6 x {tag[4], u16 start sector, u16 count} */

static uint8_t load_dsp(void)
{
    fat_file_t f;
    fat_dirent_t e;
    xfer_t x;
    uint8_t sect[6][2], i, k;
    fat_root(&f);
    if (!fat_find(&f, "MP3DEC  DSP", &e)) return 0;
    fat_open(&f, &e);
    if (fat_read(&f, bs, 512) != 512 || memcmp(bs, "ADEF", 4)) return 0;
    for (i = 0; i < 6; i++) {
        sect[i][0] = bs[ADEF_TABLE + 4 + i * 8];
        sect[i][1] = bs[ADEF_TABLE + 6 + i * 8];
    }
    x.src = bs; x.len = 512;
    for (i = 0; i < 6; i++) {
        if (!fat_seek(&f, (uint32_t)sect[i][0] << 9)) return 0;
        for (k = 0; k < sect[i][1]; k++) {
            hal_watchdog();
            if (fat_read(&f, bs, 512) != 512) return 0;
            x.dst = PAGE_BASE + k * 512;       /* byte i of the section = DSP word i */
            page_write(lane_page[i], &x);
        }
    }
    return 1;
}

/* ---- 2/3. analog on, DSP reset + clock, load, start ---- */
static void analog_on(void)
{
    DACCTL = 0;                                /* DAC_Enable(mode 0, 44.1 kHz) */
    DACRATE = DACRATE_441;
    PLLCTL |= PLL_AUDIO_441;
    DACFIFO &= ~DACFIFO_RUN;
    DACCTL = DAC_INIT | DAC_MUTE;              /* unmuted at REQ_FORMAT (mp3.c) */
    DACFIFO = DACFIFO_INIT;
    if (!(PACTL & PA_EN)) {                    /* PA_Enable({DAC, vol}), anti-pop: power up silent */
        PAVOL &= ~PAVOL_MASK;
        PACTL |= PA_DAC | PA_EN;               /* stock order: path first, then the bias */
        PACTL |= PA_ON;
        wait_ticks(20);                        /* let the amplifier settle */
    }
    while ((PAVOL & PAVOL_MASK) < mp3.vol) {   /* then fade the volume in */
        PAVOL++;
        wait_ticks(1);
    }
    PAVOL = (PAVOL & ~PAVOL_MASK) | mp3.vol;
}

uint8_t mp3_start(const fat_dirent_t *e, uint32_t off)
{
    uint8_t k;
    mp3_stop();
    memset(&mp3.now_s, 0, sizeof mp3 - 1);     /* keep vol (first member) */
    STEP(3, "DAC + PA ON");
    analog_on();
    STEP(4, "DSP CLOCK + RESET");
    DACFIFO = DACFIFO_CODEC;                   /* c_mp3decINIT */
    pll_change((PLLCTL & ~PLL_MIPS_MASK) | PLL_MIPS_36 | PLL_AUDIO_441 | PLL_PLL1EN);   /* RestDsp */
    wait_ticks(5);                             /* PLL settle (stock: 2 ms; generous) */
    DSPCTL = DSPCTL_INIT;                      /* reset held, clock on, IRQ2DSP idle */
    DSPVER = DSPVER_INIT;
    STEP(5, "LOAD MP3DEC LANES");
    SLOW();
    k = load_dsp();
    FAST();
    if (!k) return 0;
    fat_open(&file, e);                        /* after load_dsp: the FAT cache is shared */
    mp3.size = e->size;
    off &= ~511UL;                             /* seek / resume: the DSP syncs on the next frame */
    if (off >= e->size) off = 0;
    SLOW();
    if (off && !fat_seek(&file, off)) off = 0;
    FAST();
    mp3.start = off;
    memcpy(dma_ram, mp3_dma_tpl, mp3_dma_tpl_end - mp3_dma_tpl);
    first_ee = 1; pend = 0; have = 0;
    mp3.state = MP3_PLAYING;
    STEP(6, "DSP IRQ ON");
    IRQST = IRQ_DSP;                           /* drop a stale request left by the stock OS */
    hal_irq_mask(IRQ_DSP, 0);                  /* DSP requests -> EV_DSP */
    STEP(7, "DSP RUN");
    DSPCTL |= DSPCTL_RUN;
    return 1;
}

void mp3_off(void)                             /* PA_Disable */
{
    mp3_stop();
    while (PAVOL & PAVOL_MASK) {               /* fade out before the amplifier goes off */
        PAVOL--;
        wait_ticks(1);
    }
    AICTL |= AICTL_PAOFF;
    PACTL &= ~(PA_DAC | PA_EN);
    MIXER &= ~(MIX_MIC | MIX_LINE | MIX_FM);
    DACCTL |= DAC_MUTE;
    wait_ticks(4);
    AICTL &= ~AICTL_PAOFF;
}

/* first REQ_DATA: DSP parameters (spec 4.2): volume, file length, stock default gates */
static const struct { uint8_t pg; uint16_t a; uint8_t v; } fparm[] = {
    { PAGE_IDMLO_L, DM_GATE_RMS, 0x2F }, { PAGE_IDMLO_M, DM_GATE_RMS, 0x20 }, { PAGE_IDMLO_H, DM_GATE_RMS, 0x00 },
    { PAGE_IDMLO_L, DM_GATE_TIME, 0x50 }, { PAGE_IDMLO_M, DM_GATE_TIME, 0x01 }, { PAGE_IDMLO_H, DM_GATE_TIME, 0x00 } };
void mp3_first_params(void)
{
    uint8_t i;
    wr8(PAGE_IDMLO_L, DM_VOLUME, mp3.vol);
    wr8(PAGE_IDMLO_L, DM_FILELEN, ((uint8_t *)&mp3.size)[0]);
    wr8(PAGE_IDMLO_L, DM_FILELEN3, ((uint8_t *)&mp3.size)[3]);
    wr8(PAGE_IDMLO_M, DM_FILELEN, ((uint8_t *)&mp3.size)[1]);
    wr8(PAGE_IDMLO_H, DM_FILELEN, ((uint8_t *)&mp3.size)[2]);
    for (i = 0; i < sizeof fparm / sizeof fparm[0]; i++) wr8(fparm[i].pg, fparm[i].a, fparm[i].v);
}


/* time of the start offset, from the average byte rate (exact for CBR; VBR: approximate) */
/* 32-bit divide/multiply without the SDCC library (__divulong + __mullong = 347 bytes of ZRAM2) */
static uint16_t udiv(uint32_t n, uint16_t d)         /* n / d, the quotient must fit 16 bits */
{
    uint32_t r = 0;
    uint16_t q = 0;
    uint8_t i;
    for (i = 32; i; i--) {
        r = (r << 1) | (uint8_t)(((uint8_t *)&n)[3] >> 7);
        n <<= 1;
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}
uint32_t mp3_offset(uint16_t s)                     /* byte position of second s */
{
    uint32_t r = 0, a = mp3.bps;
    for (; s; s >>= 1, a <<= 1) if (s & 1) r += a;
    return r;
}
void mp3_base(void)
{
    mp3.bps = mp3.total_s ? udiv(mp3.size, mp3.total_s) : 0;
    mp3.base_s = mp3.bps ? udiv(mp3.start, mp3.bps) : 0;
}
