/* The FM station, shared by FM, USBAUDIO (FM source) and cfg.c (ZOS.CFG keys FMFREQ / FMVOL):
 * 8 bytes at FM_STATE (core/module.h, HAL_SHARED: survives module chaining, cleared at boot).
 * cfg_load fills it from ZOS.CFG; SHELL calls cfg_save when it comes back and the station differs
 * from the saved one (sv_*), and at power-off / reboot. */
#ifndef ZOS_FMSTATE_H
#define ZOS_FMSTATE_H
#include <stdint.h>
#include "../../core/module.h"

typedef struct {
    uint8_t  magic;        /* FM_MAGIC = the rest is valid */
    uint8_t  vol;          /* PA volume 0..31 */
    uint16_t f10;          /* frequency, 100 kHz units (FM_FREQ_MIN..FM_FREQ_MAX) */
    uint16_t sv_f10;       /* what ZOS.CFG holds (cfg.c) */
    uint8_t  sv_vol;
} fm_state_t;
#define FMST         ((fm_state_t *)FM_STATE)
#define FM_MAGIC     0xF4
#define FM_FREQ_MIN  875
#define FM_FREQ_MAX  1080
#define FM_FREQ_DEF  987
#define FM_VOL_DEF   14
#define FM_VOL_MAX   31
#define FMST_OK      (FMST->magic == FM_MAGIC)
#define FMST_DIRTY   (FMST_OK && (FMST->f10 != FMST->sv_f10 || FMST->vol != FMST->sv_vol))
typedef char fmst_fits[sizeof(fm_state_t) <= 8 ? 1 : -1];
#endif
