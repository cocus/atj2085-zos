/* newbrec USB device layer: SIE bring-up, attach/detach, bus reset, EP0 standard requests, endpoint
 * arming/waiting, USB buffer RAM access. Polled; every wait kicks the watchdog and leaves through
 * longjmp(usb_jb) on bus reset, VBUS loss or MENU. Own code, derived from our zos USB module
 * (zos/modules/common/usb/usbdev.c); sequences: lisong_rebuild/analysis/zos_usb.md section 3. */
#include <stdint.h>
#include <string.h>
#include "usb.h"

SFR(CLKCTL2, P_CLKCTL2);
SFR(PULLUP, P_USB_PULLUP);
SFR(USBST, P_USB_STAT);
SFR(USBADDR, P_USB_ADDR);
SFR(USBIRQ, P_USB_IRQ);
SFR(EPIRQ, P_USB_EPIRQ);
SFR(SETUPST, P_USB_SETUPST);
SFR(SU0, P_USB_SETUP0);
SFR(SU1, P_USB_SETUP1);
SFR(SU2, P_USB_SETUP2);
SFR(SU3, P_USB_SETUP3);
SFR(SU4, P_USB_SETUP4);
SFR(SU5, P_USB_SETUP5);
SFR(SU6, P_USB_SETUP6);
SFR(SU7, P_USB_SETUP7);
SFR(EPI, P_USB_EPI);
SFR(EPMODE, P_USB_EPMODE);
SFR(SAH, P_USB_SAH);
SFR(SAL, P_USB_SAL);
SFR(CNTH, P_USB_CNTH);
SFR(CNTL, P_USB_CNTL);
SFR(MPS, P_USB_MPS);
SFR(BR, P_USB_BR);
SFR(NAKST, P_USB_NAKST);
SFR(STALLST, P_USB_STALLST);
SFR(PAGEREG, P_PAGE);

jmp_buf usb_jb;
uint8_t usb_state, usb_cfg, usb_dirty, usb_halt, usb_su[8];
static uint8_t ep0buf[64], kc;

/* ---- USB buffer RAM: the CPU takes both banks for the copy (window 0x4000, port 05 = ZRAM2) ----- */
static void zr_xfer(uint8_t wr, uint16_t off, void *p, uint16_t n)
{
    uint8_t m = CLKCTL2;
    CLKCTL2 = m | CLK2_BANKS;
    if (wr) memcpy((void *)(PAGE_BASE + off), p, n);
    else memcpy(p, (void *)(PAGE_BASE + off), n);
    CLKCTL2 = m;
}
void zr_put(uint16_t off, const void *src, uint16_t n) { zr_xfer(1, off, (void *)src, n); }
void zr_get(void *dst, uint16_t off, uint16_t n) { zr_xfer(0, off, dst, n); }

/* ---- endpoints ------------------------------------------------------------------------------ */
/* stock order: EPI, mode, MPS/toggle, start offset, [ack the done bit, bulk], count high (clears BR),
 * count low (arms, BR = 1) */
static void ep_cfg(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off)
{
    EPI = epi; EPMODE = mode; MPS = mps;
    SAH = off >> 8; SAL = (uint8_t)off;
    if (epi >= EPI_EP1IN) EPIRQ = 1 << epi;
}

void ep_arm(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off, uint16_t n)
{
    uint8_t bit = 1 << epi, i = 255;
    ep_cfg(epi, mode, mps, off);
    CNTH = n >> 8;
    CNTL = (uint8_t)n;
    while (!(BR & bit) && --i) ;
}

static void ep_disarm(uint8_t epi) { EPI = epi; CNTH = 0; }

void usb_check(void)
{
    tgt_wdt();
    if (!(USBST & USBST_VBUS)) longjmp(usb_jb, AB_UNPLUG);
    if (USBIRQ & USBIRQ_RESET) longjmp(usb_jb, AB_RESET);
    if (!(++kc & 15) && tgt_key() == KEY_MENU) longjmp(usb_jb, AB_STOP);
}

/* wait for the transfer armed on `brbit` to finish (BR = 0); timeout in 10 ms ticks, 0 = none */
uint8_t ep_wait(uint8_t brbit, uint16_t timeout)
{
    uint16_t t0 = tgt_ticks();
    for (;;) {
        usb_check();
        if (!(BR & brbit)) return 1;
        if (timeout && (uint16_t)(tgt_ticks() - t0) >= timeout) return 0;
    }
}

/* ---- attach / detach / bus reset -------------------------------------------------------------- */
static void bus_reset(void)
{
    uint8_t i;
    usb_cfg = 0; usb_halt = 0;
    USBADDR = USB_ADDR_EN;
    do {
        for (i = EPI_CR; i <= EPI_EP3OUT; i++) ep_disarm(i);
        tgt_wdt();
        if (!(USBST & USBST_VBUS)) longjmp(usb_jb, AB_UNPLUG);
    } while (BR);
    USBIRQ = 0xFF; EPIRQ = 0xFF; NAKST = 0xFF; STALLST = 0xFF;
    CLKCTL2 &= ~CLK2_BANKS;
    usb_state = US_DEFAULT;
    usb_dirty = 1;
    msc_reset();
}

static void detach(void)
{
    uint8_t i;
    PULLUP &= ~USB_PULLUP_EN;
    for (i = EPI_CR; i <= EPI_EP3OUT; i++) ep_disarm(i);
    USBIRQ = 0xFF; EPIRQ = 0xFF;
    CLKCTL2 |= CLK2_BANKS;
    usb_cfg = 0;
    msc_reset();
    usb_dirty = 1;
}

static void attach(void)
{
    uint8_t st = USBST;
    usb_dirty = 1;
    if (!(st & USBST_VBUS)) { usb_state = US_NOVBUS; return; }
    if ((st & (USBST_DP | USBST_DM)) == (USBST_DP | USBST_DM) && !(PULLUP & USB_PULLUP_EN)) {
        usb_state = US_CHARGER;     /* D+ and D- both high with the pull-up off: charger, no host */
        return;
    }
    CLKCTL2 &= ~CLK2_BANKS;
    USBADDR = USB_ADDR_EN;
    USBIRQ = 0xFF; EPIRQ = 0xFF;
    PULLUP |= USB_PULLUP_EN;        /* soft connect */
    usb_state = US_ATTACHED;
}

void usb_start(void)
{
    PAGEREG = PAGE_ZRAM2;             /* window 0x4000 = the USB buffer RAM, nothing else lives there */
    tgt_usb_clock();
    PULLUP &= ~USB_PULLUP_EN;
    usb_cfg = 0;
    usb_state = US_NOVBUS;
    attach();
}

void usb_stop(void)
{
    detach();
    usb_state = US_NOVBUS;
}

void usb_recover(uint8_t why)
{
    if (why == AB_RESET) bus_reset();
    else if (why == AB_UNPLUG) { detach(); usb_state = US_NOVBUS; }
}

/* ---- EP0 ------------------------------------------------------------------------------------- */
static void ep0_stall(void) { ep_arm(EPI_CW, EPMODE_STALL, MPS_EP0_DATA1, 0, 0); }

static uint8_t ep0_ack(void)
{
    ep_arm(EPI_CW, EPMODE_CWDONE, MPS_EP0_DATA1, ZO_EP0, 0);
    if (!ep_wait(EPB_CW, 50)) { EPI = EPI_CW; CNTH = 0; return 0; }
    EPIRQ = EPB_CW;
    return 1;
}

static uint8_t ep0_send(uint8_t mode, uint8_t mps, uint8_t n)
{
    ep_arm(EPI_CR, mode, mps, ZO_EP0, n);
    if (!ep_wait(EPB_CR, 50)) { EPI = EPI_CR; CNTH = 0; return 0; }
    EPIRQ = EPB_CR;
    return 1;
}

/* control read data stage: length clipped to wLength; a multiple of 8 is followed by zero-length
 * packets while the hardware says the transfer is still open (SETUPST lock) */
static void ep0_in(const void *src, uint8_t n)
{
    if (!usb_su[7] && usb_su[6] && usb_su[6] < n) n = usb_su[6];
    zr_put(ZO_EP0, src, n);
    if (n & 7) { ep0_send(EPMODE_CRLAST, MPS_EP0_DATA1, n); return; }
    if (!ep0_send(EPMODE_NORMAL, MPS_EP0_DATA1, n)) return;
    if (!(SETUPST & SETUPST_LOCK)) return;
    if (!ep0_send(EPMODE_NORMAL, MPS_EP0, 0)) return;
    if (SETUPST & SETUPST_LOCK) ep0_send(EPMODE_CRLAST, MPS_EP0, 0);
}

void ep0_reply(const void *src, uint8_t n) { ep0_in(src, n); }
uint8_t ep0_ack_class(void) { return ep0_ack(); }

static void ep_idle(uint8_t epi, uint16_t off)
{
    ep_cfg(epi, EPMODE_NORMAL, MPS_BULK_DATA0, off);
    CNTH = 0;
}

void usb_stall_bulk(void)
{
    usb_halt |= 3;
    ep_arm(EPI_EP1OUT, EPMODE_STALL, MPS_BULK_DATA0, ZO_RX, 0x40);
    ep_arm(EPI_EP2IN, EPMODE_STALL, MPS_BULK_DATA0, ZO_TX, 0x40);
}

static uint8_t halt_bit(uint8_t ep) { return ep == 0x01 ? 1 : ep == 0x82 ? 2 : 0; }

static void clear_halt(uint8_t ep)
{
    uint8_t epi = ep == 0x01 ? EPI_EP1OUT : EPI_EP2IN;
    usb_halt &= ~halt_bit(ep);
    do { ep_disarm(epi); usb_check(); } while (BR & (1 << epi));
    ep_idle(epi, epi == EPI_EP1OUT ? ZO_RX : ZO_TX);
    if (epi == EPI_EP1OUT) msc_configured();
}

static void std_request(void)
{
    uint8_t rt = usb_su[0], rq = usb_su[1], v = usb_su[2], vh = usb_su[3], ix = usb_su[4];
    uint16_t len;
    uint8_t st[2];
    st[0] = st[1] = 0;
    switch (rq) {
    case 6:                                 /* GET_DESCRIPTOR */
        if (rt != 0x80) break;
        if (vh == 1) { ep0_in(desc_device(), 18); return; }
        if (vh == 2) { const uint8_t *d = desc_config(&len); ep0_in(d, (uint8_t)len); return; }
        if (vh == 3 && (len = desc_string(v, ep0buf))) { ep0_in(ep0buf, (uint8_t)len); return; }
        break;                              /* device qualifier etc.: stall (full speed only) */
    case 5:                                 /* SET_ADDRESS: after the status stage */
        if (rt != 0x00) break;
        if (ep0_ack()) {
            USBADDR = (v & 0x7F) | USB_ADDR_EN;
            usb_state = (v & 0x7F) ? US_ADDRESS : US_DEFAULT;
            usb_dirty = 1;
        }
        return;
    case 9:                                 /* SET_CONFIGURATION 0/1 */
        if (rt != 0x00 || v > 1) break;
        usb_cfg = v;
        if (v) {
            ep_idle(EPI_EP2IN, ZO_TX);
            ep_idle(EPI_EP1OUT, ZO_RX);
            msc_configured();
        } else msc_reset();
        usb_state = v ? US_CONFIGURED : US_ADDRESS;
        usb_dirty = 1;
        ep0_ack();
        return;
    case 8:                                 /* GET_CONFIGURATION */
        if (rt != 0x80) break;
        ep0_in(&usb_cfg, 1);
        return;
    case 10:                                /* GET_INTERFACE: alternate setting 0 */
        if (rt != 0x81) break;
        ep0_in(st, 1);
        return;
    case 11:                                /* SET_INTERFACE: alternate setting 0 only */
        if (rt != 0x01 || v) break;
        ep0_ack();
        return;
    case 0:                                 /* GET_STATUS */
        if (rt == 0x82) st[0] = (usb_halt & halt_bit(ix)) ? 1 : 0;
        else if (rt != 0x80 && rt != 0x81) break;
        ep0_in(st, 2);
        return;
    case 1:                                 /* CLEAR_FEATURE */
    case 3:                                 /* SET_FEATURE */
        if (rt == 0x00 || rt == 0x01) { ep0_ack(); return; }
        if (rt != 0x02 || v != 0 || !halt_bit(ix)) break;
        if (rq == 1) clear_halt(ix);
        else {
            usb_halt |= halt_bit(ix);
            if (ix == 0x01) ep_arm(EPI_EP1OUT, EPMODE_STALL, MPS_BULK_DATA0, ZO_RX, 0x40);
            else ep_arm(EPI_EP2IN, EPMODE_STALL, MPS_BULK_DATA0, ZO_TX, 0x40);
        }
        ep0_ack();
        return;
    }
    ep0_stall();
}

static void ep0_setup(void)
{
    USBIRQ = USBIRQ_SETUP;
    if ((SETUPST & SETUPST_MASK) != SETUPST_OK) { ep0_stall(); return; }
    usb_su[0] = SU0; usb_su[1] = SU1; usb_su[2] = SU2; usb_su[3] = SU3;
    usb_su[4] = SU4; usb_su[5] = SU5; usb_su[6] = SU6; usb_su[7] = SU7;
    if ((usb_su[0] & 0x60) == 0x20) {       /* class request */
        if (!msc_setup()) ep0_stall();
        return;
    }
    std_request();
}

/* main-loop service: cable, attach, bus reset, SETUP */
void usb_service(void)
{
    uint8_t irq;
    tgt_wdt();
    if (!(USBST & USBST_VBUS)) {
        if (usb_state >= US_ATTACHED) detach();
        if (usb_state != US_NOVBUS) { usb_state = US_NOVBUS; usb_dirty = 1; }
        return;
    }
    if (usb_state == US_NOVBUS || usb_state == US_CHARGER) { attach(); return; }
    irq = USBIRQ;
    if (irq & USBIRQ_RESET) { bus_reset(); return; }
    if (irq & USBIRQ_SETUP) ep0_setup();
    if (irq & (USBIRQ_CONN | USBIRQ_WAKE)) USBIRQ = irq & (USBIRQ_CONN | USBIRQ_WAKE);
}
