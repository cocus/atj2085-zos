/* USB descriptors. VID 0x1209 = pid.codes; PIDs 0x0001 / 0x0002 are pid.codes' TEST PIDs
 * (https://pid.codes/1209/0001/, for private testing only, never for a shipped product). Using
 * them keeps Windows from binding the stock Actions driver / s1res (VID 0x10D6) and gives each
 * function its own PID, so Windows does not mix up cached driver choices between the two.
 *  - MSC: class 08 / subclass 06 (SCSI transparent) / protocol 50 (Bulk-Only) -> usbstor.sys
 *  - CDC-ACM: device class EF/02/01 + IAD + 02/02/01 control interface + 0A data interface ->
 *    Windows 10+ binds usbser.sys without an .inf (USB\Class_02&SubClass_02&Prot_01).
 */
#include <stdint.h>
#include "usb.h"
/* one function per module (usbdev.c): USB_CDC_ONLY = the console, else mass storage */

#define VID_L 0x09
#define VID_H 0x12
#define EP_OUT   0x01               /* EP1 OUT (EPI_EP1OUT), bulk 64 */
#define EP_IN    0x82               /* EP2 IN  (EPI_EP2IN),  bulk 64 */
#define EP_NOTIF 0x81               /* EP1 IN  (EPI_EP1IN),  interrupt 8, CDC only, always NAKs */

#ifndef USB_CDC_ONLY
static const uint8_t dev_msc[18] = {
    18, 1, 0x10, 0x01,              /* USB 1.10 (as the stock disk) */
    0, 0, 0, 8,                     /* class per interface, EP0 = 8 bytes */
    VID_L, VID_H, 0x01, 0x00,       /* 1209:0001 */
    0x10, 0x00, 1, 2, 3, 1          /* bcdDevice 0.10, strings, 1 configuration */
};

static const uint8_t cfg_msc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50, /* bus powered, 100 mA */
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, EP_OUT, 2, 64, 0, 0,
    7, 5, EP_IN, 2, 64, 0, 0
};
static const char *const str_msc[] = { "zos", "zos USB disk (read-only)", "ZOS000000001" };
#define DEV dev_msc
#define CFG cfg_msc
#define STR str_msc
#else
static const uint8_t dev_cdc[18] = {
    18, 1, 0x00, 0x02,              /* USB 2.00 (IAD); full speed only: device qualifier stalls */
    0xEF, 0x02, 0x01, 8,            /* "uses IAD" */
    VID_L, VID_H, 0x02, 0x00,       /* 1209:0002 */
    0x10, 0x00, 1, 2, 3, 1
};

static const uint8_t cfg_cdc[75] = {
    9, 2, 75, 0, 2, 1, 0, 0x80, 50,
    8, 11, 0, 2, 0x02, 0x02, 0x01, 0,           /* IAD: interfaces 0-1, CDC ACM */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,         /* communication interface, AT commands */
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,                     /* call management: none, data interface 1 */
    4, 0x24, 0x02, 0x02,                        /* ACM: line coding + control line state */
    5, 0x24, 0x06, 0, 1,                        /* union: master 0, slave 1 */
    7, 5, EP_NOTIF, 3, 8, 0, 16,                /* interrupt IN, 16 ms */
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,               /* data interface */
    7, 5, EP_OUT, 2, 64, 0, 0,
    7, 5, EP_IN, 2, 64, 0, 0
};
static const char *const str_cdc[] = { "zos", "zos debug console", "ZOS000000002" };
#define DEV dev_cdc
#define CFG cfg_cdc
#define STR str_cdc
#endif

const uint8_t *desc_device(void)
{
    return DEV;
}

const uint8_t *desc_config(uint16_t *len)
{
    *len = sizeof CFG;
    return CFG;
}

/* string descriptor `idx` as UTF-16LE into out (<= 64 bytes); 0 = no such string (stall) */
uint8_t desc_string(uint8_t idx, uint8_t *out)
{
    const char *s;
    uint8_t n = 2;
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04; return 4; }  /* en-US */
    if (idx > 3) return 0;
    s = STR[idx - 1];
    while (*s && n < 64) { out[n++] = *s++; out[n++] = 0; }
    out[0] = n;
    out[1] = 3;
    return n;
}
