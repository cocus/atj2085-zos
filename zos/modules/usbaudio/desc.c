/* USB Audio Class 1 descriptors (full speed).
 *  MIC / FM : capture, mono 16-bit 32 kHz, isochronous EP2 IN (0x82, asynchronous).
 *  DAC      : playback (speaker), stereo 16-bit 48 or 44.1 kHz (SET_CUR on the endpoint),
 *             isochronous EP1 OUT (0x01, adaptive).
 * Only EP1/EP2 can be isochronous on this SIE (EP0/EP3: 8-byte packets) - PG p.22-29.
 * Topology: input terminal -> output terminal (no feature unit: Windows/Linux do the volume).
 * Windows' usbaudio.sys wants room for one extra sample per frame in wMaxPacketSize. */
#include <stdint.h>
#include "../usb/usb.h"

#define VID_L 0x09
#define VID_H 0x12

#define PID_MIC 0x03
#define PID_FM  0x04
#define PID_DAC 0x05
static uint8_t dev_audio[18] = {
    18, 1, 0x10, 0x01,                  /* USB 1.1 */
    0, 0, 0, 8,                         /* class per interface, EP0 8 bytes */
    VID_L, VID_H, PID_MIC, 0x00,
    0x16, 0x00, 1, 2, 3, 1              /* bcdDevice 0.16, strings, 1 configuration */
};
#define DEV_PID 10

/* capture: 100 bytes */
#define CAP_MPS 68                       /* (32 + 1) samples x 2 bytes = 66, rounded up */
static uint8_t cfg_cap[100] = {
    9, 2, 100, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 0x01, 0x01, 0, 0,                    /* AudioControl */
    9, 0x24, 0x01, 0x00, 0x01, 30, 0, 1, 1,             /*   header: ADC 1.0, 30 bytes, AS iface 1 */
    12, 0x24, 0x02, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0,    /*   input terminal 1: microphone, 1 ch */
    9, 0x24, 0x03, 3, 0x01, 0x01, 0, 1, 0,              /*   output terminal 3: USB streaming <- 1 */
    9, 4, 1, 0, 0, 0x01, 0x02, 0, 0,                    /* AudioStreaming alt 0: no bandwidth */
    9, 4, 1, 1, 1, 0x01, 0x02, 0, 0,                    /*   alt 1 */
    7, 0x24, 0x01, 3, 1, 0x01, 0x00,                    /*   general: terminal 3, PCM */
    11, 0x24, 0x02, 1, 1, 2, 16, 1, 0x00, 0x7D, 0x00,   /*   type I: 1 ch, 16 bit, 32000 Hz */
    9, 5, 0x82, 0x05, CAP_MPS, 0, 1, 0, 0,              /*   EP2 IN iso async */
    7, 0x25, 0x01, 0, 0, 0, 0
};
#define CAP_ITYPE 31                     /* input terminal wTerminalType (2 bytes) */

/* playback: 103 bytes */
#define DAC_MPS 196                      /* (48 + 1) frames x 4 bytes */
static const uint8_t cfg_dac[103] = {
    9, 2, 103, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 0x01, 0x01, 0, 0,                    /* AudioControl */
    9, 0x24, 0x01, 0x00, 0x01, 30, 0, 1, 1,             /*   header */
    12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0, 0, 0, /*   input terminal 1: USB streaming, L+R */
    9, 0x24, 0x03, 3, 0x01, 0x03, 0, 1, 0,              /*   output terminal 3: speaker <- 1 */
    9, 4, 1, 0, 0, 0x01, 0x02, 0, 0,                    /* AudioStreaming alt 0 */
    9, 4, 1, 1, 1, 0x01, 0x02, 0, 0,                    /*   alt 1 */
    7, 0x24, 0x01, 1, 1, 0x01, 0x00,                    /*   general: terminal 1, PCM */
    14, 0x24, 0x02, 1, 2, 2, 16, 2,                     /*   type I: 2 ch, 16 bit, 2 rates: */
        0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00,             /*     44100, 48000 */
    9, 5, 0x01, 0x09, DAC_MPS, 0, 1, 0, 0,              /*   EP1 OUT iso adaptive */
    7, 0x25, 0x01, 0x01, 0, 0, 0                        /*   sampling frequency control */
};

static const char *const str_mic[] = { "zos", "zos USB microphone", "ZOS000000003" };
static const char *const str_fm[] = { "zos", "zos USB radio", "ZOS000000004" };
static const char *const str_dac[] = { "zos", "zos USB speaker", "ZOS000000005" };

const uint8_t *desc_device(void)
{
    dev_audio[DEV_PID] = usb_fn == FN_AUDIO_DAC ? PID_DAC : usb_fn == FN_AUDIO_FM ? PID_FM : PID_MIC;
    return dev_audio;
}

const uint8_t *desc_config(uint16_t *len)
{
    if (usb_fn == FN_AUDIO_DAC) { *len = sizeof cfg_dac; return cfg_dac; }
    cfg_cap[CAP_ITYPE] = usb_fn == FN_AUDIO_FM ? 0x03 : 0x01;     /* 0x0603 line connector */
    cfg_cap[CAP_ITYPE + 1] = usb_fn == FN_AUDIO_FM ? 0x06 : 0x02; /* / 0x0201 microphone */
    *len = sizeof cfg_cap;
    return cfg_cap;
}

uint8_t desc_string(uint8_t idx, uint8_t *out)
{
    const char *s;
    uint8_t n = 2;
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04; return 4; }
    if (idx > 3) return 0;
    s = (usb_fn == FN_AUDIO_DAC ? str_dac : usb_fn == FN_AUDIO_FM ? str_fm : str_mic)[idx - 1];
    while (*s && n < 64) { out[n++] = *s++; out[n++] = 0; }
    out[0] = n;
    out[1] = 3;
    return n;
}
