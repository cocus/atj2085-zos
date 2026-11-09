/* REC (page module: UI, file creation and commit) <-> RECRUN (ZRAM2 module: ADENCM.DSP, ADC,
 * NAND streaming) hand-over block. Why two modules: ADENCM.DSP occupies DSP program memory
 * 0-0x3FF in all three PM lanes, i.e. also the page-module lane C1 (0x4000-0x43FF), and the DSP
 * cannot fetch code while a PM lane is mapped to the CPU; so whatever runs while the DSP encodes
 * must be a ZRAM2 module (4 KB code + the ZRAM1 left after the buffers), too small for the FAT
 * writer + UI (~10 KB). REC prepares the file (free run, name, header placeholder) and chains to
 * RECRUN, which records and writes the final header, then chains back to REC to commit.
 * The block sits at the top of the module scratch RAM, outside both modules' variables (RECRUN's
 * build output must end below REC_HAND: checked in RECRUN at start). */
#ifndef REC_HAND_H
#define REC_HAND_H
#include <stdint.h>
#include "../../hal/hal.h"

#define REC_HAND_ADDR (HAL_MODRAM_END - 48)
#define REC_MAGIC 0x5A
enum { RH_RECORD = 1, RH_DONE };
enum { RATE_16K, RATE_22K, RATE_32K, NRATES };
#define RH_F_POWER  0x01                /* left by EV_POWER: power off after the commit */
#define RH_F_HEADER 0x02                /* the DSP's final header was written to sector 0 */
#define RH_F_FULL   0x04                /* stopped because the run is full */
typedef struct {
    uint8_t  magic, state, rate, flags;
    char     name[12];
    uint32_t lba0;                      /* volume sector of the file's sector 0 (run start) */
    uint32_t room;                      /* sectors in the run */
    uint32_t nsec;                      /* sectors written (incl. sector 0) */
    uint16_t c0, n;                     /* the run (fatw_c0, fatw_n) */
    uint16_t drops, secs;               /* lost samples, recorded seconds */
    uint8_t  err;                       /* FTLW_* / 0xFE no DSP file / 0 */
} rec_hand_t;
static __at(REC_HAND_ADDR) rec_hand_t rh;    /* absolute: no storage allocated */
typedef char rec_hand_fits[sizeof(rec_hand_t) <= 48 ? 1 : -1];
#endif
