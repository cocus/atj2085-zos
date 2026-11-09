/* DECPLAY cold path (section _CODE2, ZRAM1): DSP program load, start / stop, the once-per-start
 * requests (REQ_START, REQ_FORMAT, REQ_TABLE), the file probe hand-off to DECINFO.ZM. */
#pragma codeseg CODE2
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "../../core/module.h"
#include "../../apps/theme.h"
#include "../music/shared.h"
#include "regs.h"
#include "dec.h"
#define ZS_EXTERN
#include "strings.h"
#include "../mp3play/ui.h"

SFR(IRQST, P_IRQST);
SFR(DSPCTL, P_DSPCTL);
SFR(DSPVER, P_DSPVER);
SFR(HDR4, P_HDR4);
SFR(HDR5, P_HDR5);
SFR(PLLCTL, P_PLLCTL);
SFR(DACCTL, P_DACCTL);
SFR(DACRATE, P_DACRATE);
SFR(DACFIFO, P_DACFIFO);
SFR(PAVOL, P_PAVOL);
SFR(AICTL, P_AICTL);
SFR(MIXER, P_MIXER);
SFR(PACTL, P_PACTL);

#define bs   dec_bs
#define file mp3_file

/* title/artist + probe (format, total time, bitrate, channels) of the next file: DECINFO.ZM in a DSP
 * lane page (the DSP is stopped here: track_info) */
void id3_read(const fat_dirent_t *e, char *title, char *artist)
{
    mod_hdr_t h;
    dec_info.e = e;
    dec_info.title = title;
    dec_info.artist = artist;
    title[0] = artist[0] = dec_info.ok = 0;
    MUS->arg = &dec_info;
    if (mod_load(DECINFO_ZM, PAGE_IPM_M, &h) == MOD_OK) mod_run(PAGE_IPM_M, &h);
}

/* ---- DSP program (ADEF) from the disk root into the DSP lanes (as mp3cold.c load_dsp) ---- */
static const uint8_t lane_page[6] = {          /* sections DMLS DMMS DMHS PMLS PMMS PMHS */
    PAGE_IDM_L, PAGE_IDM_M, PAGE_IDM_H, PAGE_IPM_L, PAGE_IPM_M, PAGE_IPM_H };
#define ADEF_TABLE 0x10                        /* 6 x {tag[4], u16 start sector, u16 count} */

static uint8_t load_dsp(const char *name11)
{
    fat_dirent_t e;
    xfer_t x;
    uint16_t sect[6][2];
    uint8_t i, k;
    fat_root(&dec_dspf);
    if (!fat_find(&dec_dspf, name11, &e)) return 0;
    fat_open(&dec_dspf, &e);
    if (fat_read(&dec_dspf, bs, 512) != 512 || memcmp(bs, "ADEF", 4)) return 0;
    for (i = 0; i < 6; i++) memcpy(sect[i], bs + ADEF_TABLE + 4 + i * 8, 4);
    for (i = 0; i < 3; i++) dec_pm_sec[i] = sect[3 + i][0];
    x.src = bs; x.len = 512;
    for (i = 0; i < 6; i++) {
        if (!fat_seek(&dec_dspf, (uint32_t)sect[i][0] << 9)) return 0;
        for (k = 0; k < sect[i][1]; k++) {
            hal_watchdog();
            if (fat_read(&dec_dspf, bs, 512) != 512) return 0;
            x.dst = PAGE_BASE + k * 512;       /* byte i of the section = DSP word i */
            page_write(lane_page[i], &x);
        }
    }
    return 1;
}

/* REQ_TABLE (OGGDEC): PM words OGG_TABLE_PM.. of every lane back from OGGDEC.DSP (the stock reads
 * the same bytes from OGGTABLE.BIN, GetOggTable), with the DSP clock stopped */
void dec_table(void)
{
    xfer_t x;
    uint16_t o, n;
    uint8_t i;
    DSPCTL &= ~DSPCTL_CLKEN;
    dec_slow();
    x.src = bs;
    for (i = 0; i < 3; i++) {
        fat_seek(&dec_dspf, ((uint32_t)dec_pm_sec[i] << 9) + OGG_TABLE_PM);
        for (o = 0; o < OGG_TABLE_N; o += n) {
            n = OGG_TABLE_N - o > 512 ? 512 : OGG_TABLE_N - o;
            fat_read(&dec_dspf, bs, n);
            x.dst = PAGE_BASE + OGG_TABLE_PM + o; x.len = n;
            page_write(PAGE_IPM_L + i, &x);
        }
    }
    dec_have = 0;                              /* bs held the prefetched sector */
    dec_fast();
    DSPCTL |= DSPCTL_CLKEN;
}

/* REQ_START: HDR4 = play mode; OGG: DM_ARG = file sectors (play) / resume sector, PCM: resume sector */
void dec_req_start(void)
{
    uint8_t i;
    HDR4 = dec_mode;
    if (dec_ogg)                               /* 24 bit, one byte per lane */
        for (i = 0; i < 3; i++) dec_wr8(PAGE_IDMLO_L + i, OGG_DM_ARG, ((uint8_t *)&dec_arg)[i]);
    else {
        if (dec_mode)                          /* 32 bit in lane H, one byte per word */
            for (i = 0; i < 4; i++) dec_wr8(PAGE_IDMLO_H, PCM_DM_RESUME + i, ((uint8_t *)&dec_arg)[i]);
        for (i = 0; i < 11; i++) dec_wr8(PAGE_IDMLO_L, PCM_DM_EQ + i, 0);   /* EQ off (stock: zeros) */
    }
    HDR5 = REP_START;
}

/* REQ_FORMAT: HDR4 low nibble = DAC rate code (both programs), then unmute */
void dec_req_format(void)
{
    uint8_t c = HDR4 & 0x0F;
    mp3.rate = c;
    if ((DACRATE ^ c) & 0x0F) {
        DACCTL &= ~DAC_EN;
        DACRATE = c;
        if (c >= DACRATE_441) { PLLCTL |= PLL_AUDIO_441; dec_wait(3); }
        else PLLCTL &= ~PLL_AUDIO_MASK;
    }
    DACCTL |= DAC_EN;
    DACFIFO &= ~DACFIFO_RUN; DACFIFO |= DACFIFO_RUN;
    DACCTL &= ~DAC_MUTE;
    HDR4 = 0;
    HDR5 = REP_FORMAT;
}

/* ---- analog on (as mp3cold.c) ---- */
/* volume ramp, one step per tick: a DAC reset or PA power change is only silent at volume 0 */
static void pa_fade(uint8_t to)
{
    uint8_t v;
    while ((v = PAVOL & PAVOL_MASK) != to) {
        PAVOL = PAVOL + (v < to ? 1 : 0xFF);
        dec_wait(1);
    }
}

static void analog_on(void)
{
    if (PACTL & PA_EN) pa_fade(0);             /* track change: amplifier still on, silence the DAC reset */
    DACCTL = 0;
    DACRATE = DACRATE_441;
    PLLCTL |= PLL_AUDIO_441;
    DACFIFO &= ~DACFIFO_RUN;
    DACCTL = DAC_INIT | DAC_MUTE;              /* unmuted at REQ_FORMAT */
    DACFIFO = DACFIFO_INIT;
    if (!(PACTL & PA_EN)) {                    /* anti-pop: power up silent */
        PAVOL &= ~PAVOL_MASK;
        PACTL |= PA_DAC | PA_EN;
        PACTL |= PA_ON;
        dec_wait(20);
    }
    pa_fade(mp3.vol);                          /* then fade the volume in */
}

static const dec_dma_t dmad[2] = {             /* bitstream DMA: PCMDEC, OGGDEC (stock descriptors) */
    { 0x0100, DMA_LANE_IDM_H, DMA_MODE_WS0, PAGE_IDMLO_H },
    { 0x0002, DMA_LANE_IDM_L, DMA_MODE_DSP, PAGE_IDMLO_L } };

uint8_t mp3_start(const fat_dirent_t *e, uint32_t off)
{
    uint8_t k;
    mp3_stop();
    mp3.now_s = 0;
    if (!dec_info.ok) {                        /* not playable: say so, then the next track */
        gfx_fill(0, 48, LCD_W, 28, C_BG);
        gfx_text(4, 58, T(S_UNSUP), 1, C_SEL, C_BG);
        dec_wait(100);
        mp3.state = MP3_DONE;
        return 1;
    }
    dec_ogg = dec_info.ogg;
    mp3.mode = dec_info.mode;
    mp3.total_s = dec_info.total_s;
    memcpy(mp3.kbps, dec_info.kbps, 3);
    mp3.bps = dec_info.bps;                    /* main.c: != 0 = seeking possible */
    analog_on();
    DACFIFO = DACFIFO_CODEC;
    dec_pll((PLLCTL & ~PLL_MIPS_MASK) | (dec_ogg ? PLL_MIPS_36 : PLL_MIPS_12) | PLL_AUDIO_441 | PLL_PLL1EN);
    dec_wait(5);                               /* PLL settle */
    DSPCTL = DSPCTL_INIT;                      /* reset held, clock on, IRQ2DSP idle */
    DSPVER = DSPVER_INIT;
    dec_slow();
    k = load_dsp(dec_ogg ? "OGGDEC  DSP" : "PCMDEC  DSP");
    fat_open(&file, e);
    dec_fast();
    if (!k) return 0;
    off &= ~511UL;                             /* seek / resume: a sector, the DSP finds its place */
    if (off >= e->size) off = 0;
    mp3.start = off;
    mp3.base_s = mp3.bps ? dec_udiv(off >> dec_info.shift, mp3.bps) : 0;
    dec_mode = off ? MODE_RESUME : MODE_PLAY;
    dec_arg = (off ? off : e->size + 511) >> 9;   /* OGG play: file length in sectors */
    memcpy(&dec_dmad, &dmad[dec_ogg], sizeof dec_dmad);
    dec_last = 0xFFFFFFFFUL; dec_have = dec_pend = dec_eof = 0; dec_first_bb = 1;
    mp3.state = MP3_PLAYING;
    IRQST = IRQ_DSP;                           /* drop a stale request */
    hal_irq_mask(IRQ_DSP, 0);                  /* DSP requests -> EV_DSP */
    DSPCTL |= DSPCTL_RUN;
    return 1;
}

uint32_t mp3_offset(uint16_t s)                /* byte position of second s */
{
    uint32_t r = 0, a = mp3.bps;
    for (; s; s >>= 1, a <<= 1) if (s & 1) r += a;
    return r << dec_info.shift;
}

void mp3_off(uint8_t keep_pa)                             /* PA_Disable (as mp3cold.c) */
{
    pa_fade(0);                                /* fade out before the DSP stops / the amplifier goes off */
    mp3_stop();
    if (keep_pa) return;                       /* chaining to the other player: no PA power cycle (pop) */
    AICTL |= AICTL_PAOFF;
    PACTL &= ~(PA_DAC | PA_EN);
    MIXER &= ~(MIX_MIC | MIX_LINE | MIX_FM);
    DACCTL |= DAC_MUTE;
    dec_wait(4);
    AICTL &= ~AICTL_PAOFF;
}
