/* FM USB OUT: the USBAUDIO capture path (ADC from the FM input -> ring -> EP2 IN isochronous), with
 * the radio still playing through the mixer and the FM UI running. The class hooks of the shared
 * USB core (usbdev.c, USB_AUDIO_ONLY): capture only, no class requests. */
#include "../usbaudio/capture.c"

void audio_alt(uint8_t value) { cap_stream(value); }       /* host starts / stops recording */
void usb_audio_reset(void) { cap_stream(0); audio_alt_cur = 0; }
void usb_audio_configured(void) { usb_audio_reset(); }
uint8_t usb_audio_setup(void) { return 0; }                  /* no class requests (stall) */
void usb_audio_poll(void) { cap_poll(); }
