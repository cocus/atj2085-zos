/* RECRUN internals shared by main.c (loop + screen pieces, ZRAM2), recio.c (capture + DSP mailbox, ZRAM2) and
 * reccold.c (start/stop/load, _CODE2 in ZRAM1). Protocol = the stock ADENCM driver (RECORD.AP
 * banks 0A-0C, lisong_rebuild/analysis/ogg_rec_rds.md 2.1), ported to polling. */
#ifndef REC_H
#define REC_H
#include <stdint.h>
#include "../rec/rechand.h"

/* ADENCM.DSP mailbox (DSP requests in HDR5, our replies in HDR5, flag/byte in HDR4) */
#define REQ_START     0x11      /* -> HDR4 = 0, reply REP_START */
#define REP_START     0x22
#define REQ_PCM       0xEE      /* wants 160 samples: DMA them, HDR4 = 0, reply REP_PCM */
#define REP_PCM       0x55      /*   (stop: HDR4 = PCM_STOP, reply REP_PCM, no data) */
#define PCM_STOP      0xAA
#define REQ_OUT       0x88      /* 512 encoded bytes at DM 0x1000: DMA them, reply REP_OUT */
#define REP_OUT       0x77
#define OUT_HEADER    0x99      /* HDR4 at REQ_OUT: the final WAV header sector (no reply) */
/* DSP data memory (words; byte lane L = PAGE_IDMLO_L window 0x4000 + address) */
#define DM_OUT        0x1000    /* encoded sector, one byte per word (lane L) */
#define DM_PCM        0x1200    /* 160 samples, DSP-mode DMA (lanes cycle L M H) */
#define DM_PARAMS     0x0FF6    /* 5 encoder options (VOX, AGC, noise level, VOR, noise), all 0 */
#define N_PARAMS      5

#define PCM_CHUNK     320       /* bytes per REQ_PCM: 160 samples, big-endian as read (hi, lo) */
#define NCHUNK        3         /* PCM ring (ZRAM1) */
#define NQ            3         /* encoded sectors waiting for the NAND (ZRAM1) */
#define SAMPLES_PER_SECTOR 1017 /* IMA ADPCM block of 512 bytes, mono */

typedef struct {
    uint8_t  pcm[NCHUNK][PCM_CHUNK];
    uint8_t  q[NQ][512];        /* q[0] doubles as the fatw buffer outside recording */
    uint8_t  desc[13];          /* DMA1 SA0 .. MODE for rec_dma (ZRAM1) */
} recbuf_t;
extern recbuf_t rb;

extern volatile uint8_t rec_on, pcm_w, pcm_full, pcm_rd, ee_pend, out_pend, stop_req;
extern volatile uint8_t q_head, q_tail, q_count, hdr_idx;
extern uint16_t settle, drops;
extern uint8_t *cap_p, *cap_end;
extern uint32_t nsamples;
extern uint16_t vsum;
extern uint8_t vcnt;

void rec_dma(const uint8_t *desc13);        /* recdma.s, ZRAM1 */
void rec_capture(void);                     /* capture.s */
void rec_service(void);                     /* ADC FIFO + deferred DSP replies */
void rec_dsp(uint8_t req);                  /* one DSP request */
uint8_t rec_load_dsp(void);                 /* reccold.c */
void rec_start(uint8_t rate_i);
void rec_stop(void);
void rec_dsp_reset(uint8_t rate_i);
#endif
