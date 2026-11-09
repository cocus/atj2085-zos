/* zos MP3 driver: the stock MP3DEC.DSP program driven over the DSP host-interface mailbox.
 * Spec: lisong_rebuild/analysis/mp3_driver_spec.md (v04 MUSIC.AP codec, sections 1-8).
 * Runs as a ZRAM2 module: MP3DEC occupies every DSP RAM lane, so the code lives in ZRAM2 (port 05 =
 * PAGE_ZRAM2: every lane half stays with the DSP) and all DSP RAM access goes through the core's
 * page_read/page_write (ZRAM1 code); buffers are module variables (ZRAM1).
 * zos' ISR reads + acks each DSP request and posts EV_DSP; the reply is sent from the main loop.
 * MP3DEC.DSP is read from the root of the user disk (copy it from the firmware: fwtool.py unpack). */
#include <stdint.h>
#include <string.h>
#include "../../hal/hal.h"
#include "../../core/fat.h"
#include "regs.h"
#include "mp3.h"
#include "mp3int.h"
#define file mp3_file
#define bs mp3_bs
#define dma_ram mp3_dma_ram
#define first_ee mp3_first_ee
#define pend mp3_pend
#define have mp3_have
#define wr8 mp3_wr8
#define pll_change mp3_pll_change
#include "../common/clock.h"

SFR(IRQST, P_IRQST);
SFR(DSPCTL, P_DSPCTL);
SFR(DSPVER, P_DSPVER);
SFR(HDR0, P_HDR0);
SFR(HDR1, P_HDR1);
SFR(HDR2, P_HDR2);
SFR(HDR3, P_HDR3);
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
SFR(CHIPID_FB, P_CHIPID_FB);
SFR(CHIPID_FC, P_CHIPID_FC);
SFR(CHIPID_FD, P_CHIPID_FD);

mp3_t mp3;
fat_file_t mp3_file;
uint8_t mp3_bs[512];                        /* bitstream sector (DMA source, ZRAM1) */
/* mp3dma.s: DMA1 bitstream routine template; it remaps port 05, so it runs from a ZRAM1 copy */
uint8_t mp3_dma_ram[64];                    /* module variables = ZRAM1 */
typedef void (*dma_fn)(const uint8_t *buf512);
uint8_t mp3_first_ee, mp3_pend, mp3_have;

/* NAND reads at 48 MHz corrupted data on the device (crash, then no module would load): every
 * read runs at 24.576 MHz; switching back is cheap while the PLL stays on */
static uint8_t want_fast;
#define SLOW()  cpu_normal()
#define FAST()  do { if (want_fast) cpu_fast(); } while (0)

static void fill(void)                         /* next 512 bytes of the file into bs */
{
    uint16_t n;
    SLOW();
    n = fat_read(&file, bs, 512);
    FAST();
    have = n != 0;
    if (n && n < 512) memset(bs + n, 0, 512 - n);
}

/* ---- DSP RAM byte access (lane page PAGE_IDMLO_L/M/H) ---- */
static uint8_t rd8(uint8_t pg, uint16_t a)
{
    xfer_t x; uint8_t v;
    x.dst = (uint16_t)&v; x.src = (const void *)a; x.len = 1;
    page_read(pg, &x);
    return v;
}
void mp3_wr8(uint8_t pg, uint16_t a, uint8_t v)
{
    xfer_t x;
    x.dst = a; x.src = &v; x.len = 1;
    page_write(pg, &x);
}

/* at least n x 10 ms, independent of the CPU clock (RTC tick) */
#define wait_ticks(n) clk_wait_ticks((n) + 1)    /* >= n x 10 ms (clock.h, RTC tick) */

/* ---- CPU clock: the 48 MHz clock comes from PLL1, which also clocks the DSP. Changing the DSP
 * clock (RestDsp, REQ_MIPS) while the CPU runs from PLL1 crashed the device (track 2 at 48 MHz),
 * so every DSP clock change is bracketed by a drop to 24.576 MHz. */
void mp3_cpu(uint8_t fast) { want_fast = fast; if (fast) cpu_fast(); else cpu_normal(); }
uint8_t mp3_cpu_fast(void) { return want_fast; }
void mp3_slow(void) { SLOW(); }
void mp3_fast(void) { FAST(); }
void mp3_wait(uint8_t n) { wait_ticks(n); }
void mp3_pll_change(uint8_t v)
{
    cpu_normal();
    PLLCTL = v;
    if (want_fast) { clk_wait_ticks(4); cpu_fast(); }   /* let the reprogrammed PLL settle first */
}

void mp3_volume(uint8_t v)
{
    mp3.vol = v > PAVOL_MAX ? PAVOL_MAX : v;
    PAVOL = mp3.vol;
}

#ifdef MP3_DIAG
#define DIAG(c) mp3_diag(0, c)
static const char hexd[] = "0123456789ABCDEF";
#else
#define DIAG(c)
#endif

/* MP3_STEP: device bring-up aid - stop before every stage (see main.c mp3_step) */
#ifdef MP3_STEP
void mp3_step(uint8_t n, const char *what);
#define STEP(n, s) mp3_step(n, s)
#else
#define STEP(n, s)
#endif

void mp3_stop(void)
{
    if (mp3.state == MP3_IDLE) return;
    hal_irq_mask(0, IRQ_DSP);
    IRQST = IRQ_DSP;
    DSPCTL &= ~DSPCTL_RUN;                     /* STOPDSP: reset, then clock off */
    DSPCTL &= ~(DSPCTL_RUN | DSPCTL_CLKEN);
    /* PLL1 stays on: it also feeds the 48 MHz CPU clock */
    DACCTL |= DAC_MUTE;
    DACFIFO |= DACFIFO_RUN;
    mp3.state = MP3_IDLE;
}

/* ---- 4/5. one DSP request ---- */
static void handle(uint8_t r)
{
    uint16_t step;
    uint8_t code, fb, fc;
    switch (r) {
    case REQ_START:
        HDR4 = MODE_PLAY;
        HDR5 = REP_START;
        break;
    case REQ_DATA:
        step = HDR2 | (uint16_t)HDR3 << 8;
        if (step > 1) {                        /* DSP skip (FF): drop the prefetched sector */
            SLOW();
            fat_seek(&file, file.pos + ((uint32_t)(step - 1 - have) << 9));
            FAST();
            have = 0;
        }
        if (first_ee) {
            first_ee = 0;
            mp3_first_params();
        }
        DIAG('N');
        if (!have) fill();                     /* first request (or after a skip) */
        if (!have) HDR0 = DATA_END;
        else {
            DIAG('D');
            ((dma_fn)dma_ram)(bs);
            HDR0 = DATA_OK;
        }
        HDR3 = 0x01;                           /* always 1 in v04 (meaning unknown, spec 4.2) */
        HDR5 = REP_DATA;                       /* reply first, then prefetch (as the stock driver) */
        DIAG('n');
        if (have) fill();
        break;
    case REQ_FORMAT:
        mp3.mode = HDR0;                       /* MPEG channel mode, MP3_MODE_MONO = 3 */
        mp3.kbps[0] = HDR1; mp3.kbps[1] = HDR2; mp3.kbps[2] = HDR3;
        code = HDR4;
        mp3.rate = code;
        mp3.total_s = rd8(PAGE_IDMLO_L, DM_TOTAL_S) + rd8(PAGE_IDMLO_L, DM_TOTAL_M) * 60 +
                      rd8(PAGE_IDMLO_L, DM_TOTAL_H) * 3600U;
        mp3_base();
        if ((DACRATE ^ code) & 0x0F) {
            DACCTL &= ~DAC_EN;
            DACRATE = code;
            if ((code & 0x0F) >= DACRATE_441) { DIAG('W'); PLLCTL |= PLL_AUDIO_441; wait_ticks(3); }
            else PLLCTL &= ~PLL_AUDIO_MASK;
        }
        DACCTL |= DAC_EN;
        DACFIFO &= ~DACFIFO_RUN; DACFIFO |= DACFIFO_RUN;
        DACCTL &= ~DAC_MUTE;
        HDR5 = REP_FORMAT;
        break;
    case REQ_STATUS:
        mp3.now_s = rd8(PAGE_IDMLO_L, DM_NOW_S) + rd8(PAGE_IDMLO_L, DM_NOW_M) * 60 +
                    rd8(PAGE_IDMLO_L, DM_NOW_H) * 3600U;
        mp3.kbps[0] = HDR1; mp3.kbps[1] = HDR2; mp3.kbps[2] = HDR3;
        wr8(PAGE_IDMLO_L, DM_VOLUME, mp3.vol);
        fb = CHIPID_FB; fc = CHIPID_FC;        /* v04 quirk, copied verbatim (spec 4.2) */
        CHIPID_FB = fb | 0x80; CHIPID_FC = fc | 0x80; CHIPID_FD &= 0x7F;
        HDR3 = CHIPID_FD;
        CHIPID_FB = fb; CHIPID_FC = fc;
        HDR4 = mp3.resumed ? 0 : STATUS_RUNNING;
        mp3.resumed = 0;
        HDR5 = REP_STATUS;
        break;
    case REQ_END:
    case REQ_FADED:
        mp3_stop();
        mp3.state = MP3_DONE;
        return;
    case REQ_SENT: HDR5 = REP_SENT; break;
    case REQ_CHIP: HDR4 = DSPVER; HDR5 = REP_CHIP; break;
    case REQ_VERDICT: HDR5 = REP_VERDICT; break;
    case REQ_MIPS:
        if ((PLLCTL & PLL_MIPS_MASK) < PLL_MIPS_48) { DIAG('P'); pll_change(PLLCTL + PLL_MIPS_STEP); }
        HDR5 = REP_MIPS;
        break;
    case REQ_OVERLAY: HDR5 = REP_OVERLAY; break;     /* never requested by MP3DEC v04 */
    default:                                   /* unknown: no reply (as the stock handler) */
        (void)0;                               /* (mp3.unknown = r for debugging) */
        break;
    }
}

/* call on EV_DSP */
void mp3_service(uint8_t r)
{
    if (mp3.state == MP3_IDLE || mp3.state == MP3_DONE) return;
    if (mp3.state == MP3_PAUSED) { pend = r; return; }   /* answered on resume */
#if defined(MP3_DIAG) || defined(MP3_STEP)
    mp3.nreq++; mp3.last = r;
#endif
#ifdef MP3_STEP
    if (mp3.nreq < 5) { static char t[8] = "REQ   "; t[4] = "0123456789ABCDEF"[r >> 4]; t[5] = "0123456789ABCDEF"[r & 15]; mp3_step(8, t); }
#endif
#ifdef MP3_DIAG
    mp3_diag(1, hexd[r >> 4]); mp3_diag(2, hexd[r & 15]); DIAG('R');
#endif
    handle(r);
    DIAG('.');
}

void mp3_pause(uint8_t on)
{
    if (on && mp3.state == MP3_PLAYING) {
        mp3.state = MP3_PAUSED;
        DACCTL |= DAC_MUTE;
    } else if (!on && mp3.state == MP3_PAUSED) {
        DACCTL &= ~DAC_MUTE;
        mp3.state = MP3_PLAYING;
        mp3.resumed = 1;
        if (pend) { handle(pend); pend = 0; }
    }
}
