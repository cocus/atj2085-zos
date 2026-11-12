#ifndef ZOS_CFG_H
#define ZOS_CFG_H
#include <stdint.h>
void    cfg_load(void);       /* defaults + ZOS.CFG -> ZCFG (sets the magic) */
uint8_t cfg_save(void);       /* ZCFG -> ZOS.CFG in place; 1 = ok (or unchanged) */
/* the clock anchor (CLOCKDAY / CLOCKMIN): the OS time when the file was last written, for a start without
 * a DS3231 after a cold boot. cfg_load fills these (0 = none); cfg_save writes the running clock
 * (rtc.h CLK_OK) - but rewrites the file for it only when something else changed, the stored anchor is
 * 30+ minutes old, or cfg_anchor_force is set (SETTINGS after setting the clock, SHELL at power-off /
 * reboot): no NAND wear for a minute counter. */
extern uint16_t cfg_aday;     /* days since 2000-01-01 */
extern uint16_t cfg_amin;     /* minute of the day */
extern uint8_t  cfg_anchor_force;
#endif
