/* REC hot path: ADC FIFO -> PCM ring, the ADENCM.DSP mailbox (REQ_PCM: ring -> DSP, REQ_OUT:
 * DSP -> encoded-sector queue), and the NAND busy-wait hook ftlw_idle().
 * Polling, no ADC interrupt (the core ISR has no hook): the FIFO (12 samples = 545 us at
 * 22.05 kHz, 375 us at 32 kHz) is drained by the main loop between small UI steps and, while the
 * NAND write path is busy (program <= 0.7 ms, erase <= 2 ms, a unit close = 128 copy-backs +
 * 2 erases, ~40-100 ms), by ftlw_idle() from its ready-wait loop. The main loop calls the write
 * path with interrupts off, so a DSP request during it stays pending in P_IRQST (the source stays
 * enabled: a request raised while 27.0 is masked is lost) and ftlw_idle() answers it itself
 * exactly like the core ISR would (HDR5 read twice, ack). The DSP therefore keeps encoding while
 * the NAND is busy; its output waits in the NQ-sector queue (ogg_rec_rds.md 2.4 latency plan). */
#include <stdint.h>
#include "../../hal/hal.h"
#include "regs.h"
#include "rec.h"


recbuf_t rb;
volatile uint8_t rec_on, pcm_w, pcm_full, pcm_rd, ee_pend, out_pend, stop_req;
volatile uint8_t q_head, q_tail, q_count, hdr_idx;
uint16_t settle, drops;
uint8_t *cap_p, *cap_end;                      /* capture.s write position, chunk end */
uint32_t nsamples;
uint16_t vsum;                                  /* VU: sum of |s| >> 5 over vcnt samples */
uint8_t vcnt;

/* stock descriptors (RECORD.AP 0A:2844 / 0A:2837) */
static void dma_pcm(void)                       /* PCM chunk -> DM_PCM (DSP mode, lanes L M H) */
{
    uint8_t *d = rb.desc, *s = rb.pcm[pcm_rd];
    d[0] = (uint8_t)(uint16_t)s; d[1] = (uint16_t)s >> 8; d[2] = 0; d[3] = DMA_A3_ZRAM1; d[4] = 0;
    d[5] = (uint8_t)DM_PCM; d[6] = DM_PCM >> 8; d[7] = 0; d[8] = DMA_A3_DSPRAM; d[9] = PAGE_IDM_L & PAGE_LANE;
    d[10] = (uint8_t)(PCM_CHUNK - 1); d[11] = (PCM_CHUNK - 1) >> 8; d[12] = DMA_MODE_DSP;
    rec_dma(d);
}
static void dma_out(void)                       /* DM_OUT lane L -> queue slot (byte per word) */
{
    uint8_t *d = rb.desc, *s = rb.q[q_head];
    d[0] = (uint8_t)DM_OUT; d[1] = DM_OUT >> 8; d[2] = 0; d[3] = DMA_A3_DSPRAM; d[4] = PAGE_IDM_L & PAGE_LANE;
    d[5] = (uint8_t)(uint16_t)s; d[6] = (uint16_t)s >> 8; d[7] = 0; d[8] = DMA_A3_ZRAM1; d[9] = 0;
    d[10] = 0xFF; d[11] = 0x01; d[12] = DMA_MODE_WS0;
    rec_dma(d);
}

static void give_pcm(void)                      /* a pending REQ_PCM with a full chunk */
{
    ee_pend = 0;
    if (stop_req) {
        HDR4 = PCM_STOP;
    } else {
        dma_pcm();
        if (++pcm_rd == NCHUNK) pcm_rd = 0;
        pcm_full--;
        HDR4 = 0;
    }
    HDR5 = REP_PCM;
}

static void take_out(void)                      /* a pending REQ_OUT with a free queue slot */
{
    uint8_t last;
    out_pend = 0;
    dma_out();
    last = HDR4 == OUT_HEADER;
    if (last) hdr_idx = q_head;
    if (++q_head == NQ) q_head = 0;
    q_count++;
    if (!last) HDR5 = REP_OUT;                /* the header ends the session: no reply */
}

void rec_dsp(uint8_t req)
{
    if (req == REQ_START) { HDR4 = 0; HDR5 = REP_START; }
    else if (req == REQ_PCM) { ee_pend = 1; if (pcm_full || stop_req) give_pcm(); }
    else if (req == REQ_OUT) { out_pend = 1; if (q_count < NQ) take_out(); }
}

/* capture.s calls this when cap_p reaches cap_end: the chunk is complete */
void rec_chunk(void)
{
    if (pcm_full < NCHUNK - 1) {
        pcm_full++;
        if (++pcm_w == NCHUNK) pcm_w = 0;
    } else {
        drops += PCM_CHUNK / 2;                 /* ring full: this chunk is overwritten */
        nsamples -= PCM_CHUNK / 2;
    }
    cap_p = rb.pcm[pcm_w];
    cap_end = cap_p + PCM_CHUNK;
}

/* ADC FIFO -> ring (capture.s, + VU sums); then the deferred replies */
void rec_service(void)
{
    rec_capture();
    if (ee_pend && (pcm_full || stop_req)) give_pcm();
    if (out_pend && q_count < NQ) take_out();
}

/* NAND busy-wait hook (ftlw.inc FTLW_IDLE): CE1 selected, interrupts off */
void ftlw_idle(void)
{
    uint8_t r;
    if (!rec_on) return;
    if (IRQST & IRQ_DSP) {                   /* what the core ISR does (isr.s) */
        r = HDR5;
        r = HDR5;
        IRQST = IRQ_DSP;
        rec_dsp(r);
    }
    rec_service();
}
