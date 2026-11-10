/* AUDIOTST launcher <-> its test modules (ATTONE.ZM tone generator, ATREC.ZM microphone / FM meter +
 * record + playback): hand-over block at HAL_SHARED. The launcher (modules/audiotst) saves the stock
 * converter registers once (sv: restored after every test and when it leaves, shown by PORT DUMP),
 * draws the chrome (skinned title) and chains to a test module, which draws its own area below the
 * title and chains back. Settings that outlive a module are kept here too. */
#ifndef ZOS_AT_H
#define ZOS_AT_H
#include <stdint.h>
#include "../../../hal/hal.h"
#include "../../../core/config.h"            /* (before the short names below: zcfg_t.vol) */

#define AT_MAGIC 0x3C
#define AT_ZM    "AUDIOTSTZM "
#define NREG 16
typedef struct {
    char     next[12];                  /* MOD_CHAIN_NAME: must be first */
    uint8_t  magic, back, sel;          /* AT_MAGIC; set by a test chaining back; menu row */
    uint8_t  sv[NREG];                  /* converter registers at the first entry (stock state) */
    uint8_t  vol;                       /* 0..31 */
    uint8_t  wpf_mode, wpf, measured;   /* DAC FIFO writes per frame: mode 0 auto / 1 / 2, in use */
    uint8_t  fifo_depth;
    uint16_t wps;                       /* measured FIFO writes per second */
    uint8_t  mic_gain;                  /* D4.2-0 */
    uint16_t f10;                       /* FM frequency in 100 kHz units */
} at_shared_t;
static __at(HAL_SHARED) at_shared_t at_sh;     /* absolute: no storage allocated */
#define ATS (&at_sh)
#define vol        (ATS->vol)
#define wpf_mode   (ATS->wpf_mode)
#define wpf        (ATS->wpf)
#define measured   (ATS->measured)
#define fifo_depth (ATS->fifo_depth)
#define wps        (ATS->wps)
#define mic_gain   (ATS->mic_gain)
#define f10        (ATS->f10)
#define sv         (ATS->sv)
#endif
