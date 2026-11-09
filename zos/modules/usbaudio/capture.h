/* USB audio capture shared by USBAUDIO (MIC / FM sources) and FM (USB OUT): see capture.c */
#ifndef ZOS_CAPTURE_H
#define ZOS_CAPTURE_H
#include <stdint.h>

#define AUDIO_SAMPLES 32                 /* per 1 ms packet at 32 kHz */
#define AUDIO_BYTES   (AUDIO_SAMPLES * 2)

extern uint16_t audio_packets, audio_dropped;
extern uint8_t cap_streaming;            /* EP2 IN alt 1 selected by the host */
extern uint8_t audio_alt_cur;            /* audio streaming interface alternate setting */

void cap_adc_on(uint8_t fm);             /* ADC 32 kHz mono from the FM input (1) or the microphone */
void cap_adc_off(void);                  /* ADC registers as found by cap_adc_on */
void cap_stream(uint8_t on);             /* EP2 IN isochronous on (ring emptied) / off */
void cap_poll(void);                     /* drain the ADC FIFO (VU level, ring) + send packets:
                                            at least every ~300 us while the ADC runs */
uint16_t audio_level(void);              /* average |sample| since the last call, 0..32767 */
void audio_alt(uint8_t value);           /* module hook: SET_INTERFACE(1, value) changed it */
#endif
