#ifndef ZOS_CFG_H
#define ZOS_CFG_H
#include <stdint.h>
void    cfg_load(void);       /* defaults + ZOS.CFG -> ZCFG (sets the magic) */
uint8_t cfg_save(void);       /* ZCFG -> ZOS.CFG in place; 1 = ok (or unchanged) */
#endif
