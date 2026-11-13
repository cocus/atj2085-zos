/* DECPLAY hot path (ZRAM2): the OGGDEC / PCMDEC request handler, pause, stop, volume, clocks.
 * Same structure as mp3play/mp3.c: zos' ISR reads + acks each DSP request and posts EV_DSP, the reply
 * is sent from the main loop; all DSP RAM access goes through page_read/page_write; the bitstream DMA
 * (decdma.s) runs from ZRAM1 because it remaps port 05. Protocol: dec.h. */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "regs.h"
#include "dec.h"
#include "../common/clock.h"
#define file mp3_file
#define bs   dec_bs


mp3_t mp3;
fat_file_t mp3_file;
uint8_t dec_bs[512];
dec_dma_t dec_dmad;
uint8_t dec_ogg, dec_mode, dec_have, dec_pend, dec_first_bb;
uint32_t dec_arg, dec_last, dec_pf;
uint16_t dec_eof;
uint16_t dec_pm_sec[3];
fat_file_t dec_dspf;
dec_info_t dec_info;

/* NAND reads at 24.576 MHz (mp3.c: 48 MHz reads corrupted data on the device) */
static uint8_t want_fast;
#define SLOW()  cpu_normal()
#define FAST()  do { if (want_fast) cpu_fast(); } while (0)
void mp3_cpu(uint8_t fast) { want_fast = fast; if (fast) cpu_fast(); else cpu_normal(); }
void dec_slow(void) { SLOW(); }
void dec_fast(void) { FAST(); }
void dec_wait(uint8_t n) { clk_wait_ticks(n + 1); }      /* >= n x 10 ms */
void dec_pll(uint8_t v)                                  /* DSP clock change: never while at 48 MHz */
{
    cpu_normal();
    PLLCTL = v;
    if (want_fast) { clk_wait_ticks(4); cpu_fast(); }
}

/* ---- DSP RAM byte access (lane page PAGE_IDMLO_L/M/H) ---- */
uint8_t dec_rd8(uint8_t pg, uint16_t a)
{
    xfer_t x; uint8_t v;
    x.dst = (uint16_t)&v; x.src = (const void *)a; x.len = 1;
    page_read(pg, &x);
    return v;
}
void dec_wr8(uint8_t pg, uint16_t a, uint8_t v)
{
    xfer_t x;
    x.dst = a; x.src = &v; x.len = 1;
    page_write(pg, &x);
}

void mp3_volume(uint8_t v)
{
    mp3.vol = v > PAVOL_MAX ? PAVOL_MAX : v;
    PAVOL = mp3.vol;
}

void mp3_stop(void)
{
    if (mp3.state == MP3_IDLE) return;
    hal_irq_mask(0, IRQ_DSP);
    IRQST = IRQ_DSP;
    DSPCTL &= ~DSPCTL_RUN;                     /* reset, then clock off */
    DSPCTL &= ~(DSPCTL_RUN | DSPCTL_CLKEN);
    DACCTL |= DAC_MUTE;
    DACFIFO |= DACFIFO_RUN;
    mp3.state = MP3_IDLE;
}

/* end of file: the DSP asked for a sector past the end (no reply, as the stock drivers) and, for
 * PCMDEC, never sends REQ_END: stop once its buffer has played out */
#define EOF_DRAIN 40                           /* x 10 ms */
void dec_poll(void)
{
    if (dec_eof && mp3.state == MP3_PLAYING && (uint16_t)(hal_ticks() - dec_eof) > EOF_DRAIN) {
        mp3_stop();
        mp3.state = MP3_DONE;
    }
}


static void handle(uint8_t r)
{
    uint8_t b[4], i;
    uint16_t now;
    switch (r) {
    case REQ_START: dec_req_start(); break;    /* (cold: once per start) */
    case REQ_FORMAT: dec_req_format(); break;
    case REQ_DATA:                             /* step (24 bit) from the last sector sent, HDR4 = direction */
        if (dec_ogg) HDR3 = 0; else HDR0 = 0;  /* fade state: none */
        b[3] = 0;
        for (i = 0; i < 3; i++)
            b[i] = dec_ogg ? dec_rd8(PAGE_IDMLO_L + i, OGG_DM_ARG) : dec_rd8(PAGE_IDMLO_H, PCM_DM_STEP + i);
        if (HDR4 == DIR_FWD) dec_last += *(uint32_t *)b;
        else dec_last -= *(uint32_t *)b;
        if (!(dec_have && dec_pf == dec_last) && !dec_read_sec(dec_last)) {
            if (!dec_eof) dec_eof = hal_ticks() | 1;
            dec_last -= *(uint32_t *)b;        /* (as if not asked: a retry steps the same way) */
            break;                             /* end of file: no reply */
        }
        dec_dma(bs);
        HDR5 = REP_DATA;                       /* reply first, then prefetch (as the stock drivers) */
        dec_read_sec(dec_last + 1);
        break;
    case REQ_STATUS:                           /* current time; volume (+ EQ / speed) back */
        if (dec_ogg) {
            now = dec_rd8(PAGE_IDMLO_L, OGG_DM_TIME) + dec_rd8(PAGE_IDMLO_M, OGG_DM_TIME) * 60 +
                  dec_rd8(PAGE_IDMLO_H, OGG_DM_TIME) * 3600U;
            dec_wr8(PAGE_IDMLO_L, OGG_DM_VOL, mp3.vol);
            dec_wr8(PAGE_IDMLO_L, OGG_DM_EQ, 0);
        } else {
            now = dec_rd8(PAGE_IDMLO_L, PCM_DM_TIME) + dec_rd8(PAGE_IDMLO_L, PCM_DM_TIME + 1) * 60 +
                  dec_rd8(PAGE_IDMLO_L, PCM_DM_TIME + 2) * 3600U;
            dec_wr8(PAGE_IDMLO_L, PCM_DM_VOL, mp3.vol);
            dec_wr8(PAGE_IDMLO_L, PCM_DM_SPEED, 0);
        }
        if (dec_first_bb && now) {             /* after a seek: does the DSP count from the file start? */
            dec_first_bb = 0;
            if (now * 2 >= mp3.base_s) mp3.base_s = 0;
        }
        mp3.now_s = now;
        HDR5 = REP_STATUS;
        break;
    case REQ_END:                              /* HDR4 = END_OK, or 0x10 / 0x2x: error, bad format */
        mp3_stop();
        mp3.state = MP3_DONE;
        return;
    case REQ_TABLE:                            /* OGGDEC: restore its PM table (used as scratch) */
        if (dec_ogg) dec_table();
        HDR5 = REP_TABLE;
        break;
    case REQ_2C: HDR4 = 0; HDR5 = REP_2C; break;
    case REQ_FADED: HDR5 = REP_FADED; break;
    case REQ_SENT: HDR5 = REP_SENT; break;
    case REQ_OVERLAY: HDR5 = REP_OVERLAY; break;
    case REQ_MIPS:                             /* OGG: straight to 48 MIPS (stock SetDspMips(3)); PCM: ack only */
        if (dec_ogg && (PLLCTL & PLL_MIPS_MASK) != PLL_MIPS_48) dec_pll((PLLCTL & ~PLL_MIPS_MASK) | PLL_MIPS_48);
        HDR5 = REP_MIPS;
        break;
    default:                                   /* unknown: no reply (as the stock handlers) */
        break;
    }
}

void mp3_service(uint8_t r)
{
    if (mp3.state == MP3_IDLE || mp3.state == MP3_DONE) return;
    if (r != HDR5) return;                     /* stale: posted before a restart (seek, next track) */
    if (mp3.state == MP3_PAUSED) { dec_pend = r; return; }   /* answered on resume */
    handle(r);
}

void mp3_pause(uint8_t on)
{
    if (on && mp3.state == MP3_PLAYING) {
        mp3.state = MP3_PAUSED;
        DACCTL |= DAC_MUTE;
    } else if (!on && mp3.state == MP3_PAUSED) {
        DACCTL &= ~DAC_MUTE;
        mp3.state = MP3_PLAYING;
        if (dec_pend) { handle(dec_pend); dec_pend = 0; }
    }
}
