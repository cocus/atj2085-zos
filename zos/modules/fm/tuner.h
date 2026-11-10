/* FM tuner drivers (tuner.c) and the RDS decoder (rds.c). Two chips on the same bit-banged I2C bus
 * (GPIO C0 SCL / C1 SDA), picked by tn_probe():
 *   TN_RDA  RDA5807M/SP/FP family in native register mode (0x11 random access, 0x10 sequential),
 *           with RDS; detected by its chip ID 0x58xx (register 00h)
 *   TN_TEA  TEA5767 (or anything else that ACKs 0x60, incl. an RDA in TEA mode), 5-byte frames
 * Register map / RDS notes: lisong_rebuild/analysis/ogg_rec_rds.md 3.5. */
#ifndef FM_TUNER_H
#define FM_TUNER_H
#include <stdint.h>

enum { TN_NONE, TN_TEA, TN_RDA };
extern uint8_t tn_type;

typedef struct {
    uint8_t  lev;           /* 0..15 (TEA level; RDA RSSI / 4) */
    uint8_t  stereo;
    uint8_t  station;       /* a real station here (TEA: level + IF counter; RDA: FM_TRUE) */
    uint8_t  done;          /* RDA: STC (tune / seek complete); TEA: always 1 */
    uint16_t f10;           /* RDA: READCHAN in 100 kHz units; TEA: 0 */
} tn_stat_t;

void    i2c_open(void);                 /* pins: bus idle high (saved, restored by i2c_close) */
void    i2c_close(void);
uint8_t tn_probe(void);                 /* -> tn_type */
void    tn_tune(uint16_t f10);          /* RDA: TUNE, completes ~10-60 ms later (STC) */
void    tn_seek(uint8_t up);            /* RDA: hardware seek start (tn_stat .done / .f10 follow it) */
uint8_t tn_status(tn_stat_t *s);        /* 0 = no answer */
void    tn_off(void);                   /* standby / power down */
uint8_t tn_id(void);                    /* RDA chip ID high byte (0x58) as probed, else 0 */

/* RDS (RDA only): rds_poll() reads one group if the chip has one (call every ~40 ms) */
#define RDS_PS   0x01                   /* rds_new bits: what changed since the UI last looked */
#define RDS_RT   0x02
#define RDS_PI   0x04
extern char     rds_ps[9];              /* Program Service, shown once all 4 segments are confirmed */
extern char     rds_rt[65];             /* RadioText so far (0-terminated at the first gap / CR) */
extern uint16_t rds_pi;
extern uint8_t  rds_new;
void rds_poll(void);
void rds_clear(void);
uint8_t rds_text(char *d, const char *s, uint8_t n);   /* displayable copy (font chars), returns length */
#endif
