/* newbrec USB device layer (ATJ2085 full-speed SIE, polled) + mass storage. Register sequences as in
 * lisong_rebuild/analysis/zos_usb.md / usb_msc.md (device-verified in zos). */
#ifndef NEWBREC_USB_H
#define NEWBREC_USB_H
#include <stdint.h>
#include <setjmp.h>
#include "regs.h"
#include "target.h"

/* USB buffer RAM (ZRAM2, 2 x 2 KB) as the SIE sees it with both banks USB-owned */
#define ZO_RX   0x0000          /* EP1 OUT: CBW, WRITE data (B1) */
#define ZO_EP0  0x0700          /* EP0 data stage (end of B1) */
#define ZO_TX   0x0800          /* EP2 IN: replies, CSW (B2) */

enum { US_NOVBUS, US_CHARGER, US_ATTACHED, US_DEFAULT, US_ADDRESS, US_CONFIGURED };
enum { AB_NONE, AB_RESET, AB_UNPLUG, AB_STOP };      /* longjmp codes out of any wait */

extern jmp_buf usb_jb;
extern uint8_t usb_state, usb_cfg, usb_dirty, usb_halt, usb_su[8];

void zr_put(uint16_t off, const void *src, uint16_t n);     /* RAM -> USB buffer RAM */
void zr_get(void *dst, uint16_t off, uint16_t n);           /* USB buffer RAM -> RAM */

void ep_arm(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off, uint16_t n);
uint8_t ep_wait(uint8_t brbit, uint16_t timeout);
void usb_check(void);
void usb_start(void);
void usb_stop(void);
void usb_service(void);
void usb_recover(uint8_t why);
void usb_stall_bulk(void);
void ep0_reply(const void *src, uint8_t n);
uint8_t ep0_ack_class(void);

/* descriptors */
const uint8_t *desc_device(void);
const uint8_t *desc_config(uint16_t *len);
uint8_t desc_string(uint8_t idx, uint8_t *out);

/* mass storage (msc.c) */
extern uint8_t msc_ejected;
extern uint16_t msc_ncmd, msc_nrd, msc_nwr;
extern uint8_t msc_lastop, msc_laststatus;
void msc_set_geometry(uint32_t blocks);
void msc_reset(void);
void msc_configured(void);
void msc_poll(void);
uint8_t msc_setup(void);

#endif
