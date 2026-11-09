/* USB module (zos): full-speed device on the ATJ2085 SIE, polled (no USB interrupt), with mass
 * storage, a CDC-ACM debug console, and USB Audio Class capture.
 * Design + register sequences: lisong_rebuild/analysis/zos_usb.md.
 *
 * Memory: the module is a normal page module (code + variables in its DSP RAM page at 0x4000).
 * The USB SIE can only DMA to/from ZRAM2 (4 KB, banks B1 + B2), which is mapped into the same
 * 0x4000 window (port 05 = PAGE_ZRAM2), so the module never touches ZRAM2 itself: it goes through
 * the core's page_read/page_write (ZRAM1 code) or a small ZRAM1 helper (usbz.s, copied to zr->stub).
 * Everything handed to those helpers lives in the ZRAM1 scratch block below. */
#ifndef ZOS_USB_H
#define ZOS_USB_H
#include <stdint.h>
#include <setjmp.h>
#include "../../hal/hal.h"
#include "regs.h"

/* event log (mass storage sessions; read later with the console's "log") */
#define LOG_N 128
typedef struct { uint8_t k, a, b, c; uint16_t w; } logent_t;

/* ---- ZRAM1 scratch: HAL_MODRAM_BASE..HAL_MODRAM_END is free while a page module runs ---------- */
typedef struct {
    uint8_t stub[48];        /* usbz.s copy (block read into ZRAM2) */
    uint8_t ep0[128];        /* EP0 data stage staging (descriptors, class data) */
    uint8_t rx[64];          /* console: last OUT packet */
    uint8_t cbw[32];         /* MSC: command block wrapper */
    uint8_t csw[16];         /* MSC: command status wrapper */
    uint8_t tx[512];         /* MSC reply staging / console output buffer */
    uint8_t sec[512];        /* console: sector buffer */
    uint8_t zero[64];        /* zeros (zr_zero source) */
    logent_t logv[LOG_N];    /* MSC event ring survives loading the audio companion module */
} zr_t;
#define ZR ((zr_t *)HAL_MODRAM_BASE)

/* ---- USB address map of ZRAM2 while USB owns both banks (P_CLKCTL2 & CLK2_BANKS = 0): the SIE
 * sees B1 at 0x000-0x7FF and B2 at 0x800-0xFFF (giveio usage); the CPU sees the same layout at
 * PAGE_BASE when it owns both banks (CLK2_BANKS set). READ(10) uses the stock ping-pong instead. */
#define ZO_RX   0x0000          /* EP1 OUT: CBW, console input, WRITE(10) drain (B1) */
#define ZO_EP0  0x0700          /* EP0 data stage, both directions (end of B1) */
#define ZO_TX   0x0800          /* EP2 IN: data-in replies, CSW, console output (B2) */
#define ZO_MAX  0x0800          /* largest single transfer (one bank) */

/* ---- functions, states -------------------------------------------------------------------- */
enum { FN_MSC, FN_CDC, FN_AUDIO_MIC, FN_AUDIO_FM, FN_AUDIO_DAC };
#define USB_IS_AUDIO(fn) ((fn) >= FN_AUDIO_MIC)
enum { US_OFF, US_NOVBUS, US_CHARGER, US_ATTACHED, US_DEFAULT, US_ADDRESS, US_CONFIGURED };
enum { AB_NONE, AB_RESET, AB_UNPLUG, AB_STOP };      /* longjmp codes out of any wait */

extern jmp_buf usb_jb;
extern uint8_t usb_fn, usb_state, usb_cfg, usb_dirty;
extern uint8_t usb_halt;                /* bit0 = EP 0x01 halted, bit1 = EP 0x82 halted */
extern uint8_t usb_su[8];               /* current SETUP packet */
extern uint16_t usb_nsetup, usb_nreset; /* statistics (console "usb", LCD) */
extern uint8_t usb_lcok, usb_lcfail;    /* control-write data stages done / timed out */

/* port access from module code (constant ports use SFRs; these take a variable port) */
uint8_t port_in(uint8_t port);
void port_out(uint8_t port, uint8_t v);

/* ZRAM2 access (CPU takes both banks for the copy, then the previous ownership is restored) */
void zr_put(uint16_t off, const void *src, uint16_t n);     /* src in ZRAM1 */
void zr_get(void *dst, uint16_t off, uint16_t n);           /* dst in ZRAM1 */
void zr_zero(uint16_t off, uint16_t n);
/* read user-disk block `lba` into ZRAM2 at CPU address `dst` (bank(s) owned by the MCU) */
uint8_t blk_to_zram2(uint32_t lba, uint16_t dst);

/* endpoints */
void ep_arm(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off, uint16_t n);
void ep_disarm(uint8_t epi);
#define W_EP0   0x01            /* give up when a new SETUP arrives (EP0 stages) */
#define W_SVC   0x02            /* serve SETUPs while waiting (console output) */
uint8_t ep_wait(uint8_t brbit, uint8_t flags, uint16_t timeout);
void usb_check(void);           /* longjmp on bus reset / VBUS loss */

/* device */
void usb_start(uint8_t fn);
void usb_stop(void);
void usb_recover(uint8_t why);  /* after a longjmp(AB_RESET / AB_UNPLUG) */
void usb_service(void);         /* VBUS, attach, bus reset, SETUP */
void usb_stop_clock(uint8_t keep_pll);   /* PLLCTL back as found (unless the CPU runs from it) */
void usb_stall_bulk(void);               /* stall EP 0x01 + 0x82 (invalid CBW) */
/* EP0 helpers for class requests */
void ep0_reply(const void *src, uint8_t n);   /* data stage IN (clipped to wLength) + status */
uint8_t ep0_out(uint8_t n);                   /* data stage OUT into ZR->ep0, 0 = timeout */
uint8_t ep0_ack_class(void);                  /* status stage of a no-data request */
uint8_t usb_audio_std_request(void);          /* audio interface GET/SET_INTERFACE */

/* descriptors (desc.c) */
const uint8_t *desc_device(void);
const uint8_t *desc_config(uint16_t *len);
uint8_t desc_string(uint8_t idx, uint8_t *out);             /* -> length, 0 = none */

/* event log indices; storage lives in fixed ZRAM1 scratch */
/* k: 'R' bus reset, 'U' cable out, 'S' SETUP (a = bmRequestType, b = bRequest, c = wValueH,
 *    w = wLength), 'C' SCSI ok / 'F' SCSI failed (a = opcode, b = sense key, c = ASC,
 *    w = LBA bits 15-0 for READ/WRITE else dCBWDataTransferLength), 'I' invalid CBW (a..c = first
 *    bytes), 'A' command aborted by reset/unplug (a = opcode, b = stage: 1 data, 2 CSW), 'H' halt
 *    set/cleared by the host (a = endpoint, b = 1 set / 0 clear),*/
#define usb_logv (ZR->logv)
extern uint8_t usb_logpos, usb_logn;
void usb_log(uint8_t k, uint8_t a, uint8_t b, uint8_t c, uint16_t w);
extern uint8_t msc_rdmode;              /* READ(10): 1 = stock bank ping-pong (default, device-verified), 0 = flat */
extern uint8_t msc_lastop, msc_laststatus, msc_stage;
extern uint32_t msc_boot_total;         /* FAT boot sector "total sectors" (0 = not read) */
void msc_abort_note(uint8_t why);

/* class hooks */
void msc_reset(void);           /* bus reset / (de)configuration */
void msc_configured(void);
void msc_poll(void);
uint8_t msc_setup(void);        /* class request: 1 handled */
void cdc_reset(void);
void cdc_configured(void);
void cdc_poll(void);
uint8_t cdc_setup(void);
void usb_audio_reset(void);
void usb_audio_configured(void);
void usb_audio_poll(void);
uint8_t usb_audio_setup(void);
extern uint16_t audio_packets, audio_dropped, audio_under;
extern uint32_t audio_words;
extern uint8_t audio_tuner_ok;
extern uint32_t msc_last;       /* last LBA reported to the host */
extern uint16_t msc_ncmd, msc_nsec;
extern uint8_t msc_ejected;
extern uint8_t msc_wr;          /* writes enabled (default on; console "wr off") */
extern uint16_t cdc_nrx, cdc_ndrop;
extern uint8_t cdc_dtr;
#endif
