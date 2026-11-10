/* newbrec USB descriptors: one mass-storage interface (class 08 / SCSI transparent / bulk-only).
 * VID 0x1209 = pid.codes, PID 0x0001 = pid.codes' TEST pid (private testing only), bcdDevice 0x0100. */
#include <stdint.h>
#include "usb.h"

static const uint8_t dev[18] = {
    18, 1, 0x10, 0x01, 0, 0, 0, 8,
    0x09, 0x12, 0x01, 0x00,         /* 1209:0001 */
    0x00, 0x01, 1, 2, 3, 1
};
static const uint8_t cfg[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x01, 2, 64, 0, 0,        /* EP1 OUT bulk */
    7, 5, 0x82, 2, 64, 0, 0         /* EP2 IN bulk */
};
static const char *const str[] = { "ZOS", "NEWBREC RAW NAND", "NB0000000001" };

const uint8_t *desc_device(void) { return dev; }
const uint8_t *desc_config(uint16_t *len) { *len = sizeof cfg; return cfg; }

uint8_t desc_string(uint8_t idx, uint8_t *out)
{
    const char *s;
    uint8_t n = 2;
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04; return 4; }
    if (idx > 3) return 0;
    s = str[idx - 1];
    while (*s && n < 64) { out[n++] = *s++; out[n++] = 0; }
    out[0] = n;
    out[1] = 3;
    return n;
}
