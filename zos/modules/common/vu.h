#ifndef ZOS_VU_H
#define ZOS_VU_H
#include <stdint.h>
#define VU_W      120
#define VU_H      6
#define VU_C_LOW  RGB(50, 110, 220)
#define VU_C_MID  RGB(255, 200, 40)
#define VU_C_HIGH RGB(255, 60, 40)
#define MIX_SRC_MIXER 0x03          /* ADC source: analog mixer output */
void    vu_adc_on(uint8_t src);     /* MIX_SRC_FM / MIX_SRC_MIXER */
void    vu_adc_off(void);
void    vu_poll(void);
uint8_t vu_level(void);
uint8_t vu_px(uint16_t v);
void    vu_bar(uint8_t x, uint8_t y, uint8_t px, uint16_t off);
#endif
