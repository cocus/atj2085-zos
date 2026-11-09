/* MP3PLAY internals shared by mp3.c (hot path, ZRAM2) and mp3cold.c (start/stop/load, the
 * module's second code section _CODE2 in ZRAM1, see build.py "code2") */
#ifndef MP3INT_H
#define MP3INT_H
#include <stdint.h>
#include "../../core/fat.h"

extern fat_file_t mp3_file;
extern uint8_t mp3_bs[512];                    /* bitstream sector (DMA source, ZRAM1) */
extern uint8_t mp3_dma_ram[64];                /* ZRAM1 copy of the DMA routine (mp3dma.s) */
extern uint8_t mp3_first_ee, mp3_pend, mp3_have;
extern const uint8_t mp3_dma_tpl[], mp3_dma_tpl_end[];

void mp3_wr8(uint8_t pg, uint16_t a, uint8_t v);
void mp3_pll_change(uint8_t v);
void mp3_slow(void);                           /* NAND access: 24.576 MHz */
void mp3_fast(void);                           /* back to the user's choice */
void mp3_wait(uint8_t n);                      /* >= n x 10 ms */
void mp3_first_params(void);                   /* mp3cold.c */
#endif
