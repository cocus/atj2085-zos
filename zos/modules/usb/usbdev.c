/* USB device layer: SIE bring-up, attach/detach, bus reset, EP0 (standard requests), endpoint
 * arming/waiting and ZRAM2 access. Polled; every wait kicks the watchdog and leaves through
 * longjmp(usb_jb) on bus reset or VBUS loss (the stock code unwinds its ISR the same way).
 * Register sequences mirror USBDISK.AP bank 0x21 (addresses in comments); see zos_usb.md. */
#include <stdint.h>
#include <string.h>
#include "usb.h"
#include "../../core/event.h"

SFR(PLLCTL, P_PLLCTL);
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

jmp_buf usb_jb;
uint8_t usb_fn, usb_state, usb_cfg, usb_dirty, usb_halt;
uint8_t usb_su[8];
uint16_t usb_nsetup, usb_nreset;
uint8_t usb_lcok, usb_lcfail;
static uint8_t addr, pll_saved, pll_set;

#ifdef USB_AUDIO_ONLY
void usb_log(uint8_t k, uint8_t a, uint8_t b, uint8_t c, uint16_t w)
{
    (void)k; (void)a; (void)b; (void)c; (void)w;
}
#else
uint8_t usb_logpos, usb_logn;

void usb_log(uint8_t k, uint8_t a, uint8_t b, uint8_t c, uint16_t w)
{
    logent_t *e;
    if (usb_fn != FN_MSC) return;
    e = &usb_logv[usb_logpos];
    e->k = k; e->a = a; e->b = b; e->c = c; e->w = w;
    usb_logpos = (usb_logpos + 1) & (LOG_N - 1);
    if (usb_logn < LOG_N) usb_logn++;
}
#endif

#ifndef USB_AUDIO_ONLY
extern const uint8_t usbz_tpl[], usbz_tpl_end[];
typedef struct { uint32_t lba; uint16_t dst; } blkz_t;
typedef uint8_t (*blkz_fn)(const blkz_t *a);
#endif

/* ---- ZRAM2 -------------------------------------------------------------------------------- */
static void zr_xfer(uint8_t wr, uint16_t off, void *zr1, uint16_t n)
{
    xfer_t x;
    uint8_t m = CLKCTL2;
    x.dst = wr ? PAGE_BASE + off : (uint16_t)zr1;
    x.src = wr ? zr1 : (const void *)(PAGE_BASE + off);
    x.len = n;
    CLKCTL2 = m | CLK2_BANKS;              /* both banks to the MCU: B1 at 0x4000, B2 at 0x4800 */
    if (wr) page_write(PAGE_ZRAM2, &x); else page_read(PAGE_ZRAM2, &x);
    CLKCTL2 = m;                           /* ownership as before */
}
void zr_put(uint16_t off, const void *src, uint16_t n) { zr_xfer(1, off, (void *)src, n); }
void zr_get(void *dst, uint16_t off, uint16_t n) { zr_xfer(0, off, dst, n); }
void zr_zero(uint16_t off, uint16_t n)
{
    uint16_t k;
    memset(ZR->zero, 0, sizeof ZR->zero);
    while (n) {
        k = n > sizeof ZR->zero ? sizeof ZR->zero : n;
        zr_put(off, ZR->zero, k);
        off += k; n -= k;
    }
}

#ifndef USB_AUDIO_ONLY
uint8_t blk_to_zram2(uint32_t lba, uint16_t dst)
{
    blkz_t a;                               /* on the stack = ZRAM1, readable by the stub */
    a.lba = lba; a.dst = dst;
    return ((blkz_fn)ZR->stub)(&a);
}
#endif

/* ---- endpoints ---------------------------------------------------------------------------- */
/* stock order (sSet_Ep*_Mode b21:1fa6..1ff0, then 64h/65h): EPI, mode, MPS/toggle, start offset,
 * [ack the endpoint's done bit, bulk only], count high (clears BR), count low (arms, BR = 1) */
static void ep_cfg(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off)
{
    EPI = epi;
    EPMODE = mode;
    MPS = mps;
    SAH = off >> 8;
    SAL = (uint8_t)off;
    if (epi >= EPI_EP1IN) EPIRQ = 1 << epi;
}

/* arm: after the 65h write the stock code first waits for BR = 1 (f_b21_1c49, b21:1ff3), so a
 * later "BR = 0" really means "this transfer finished". Bounded here: a transfer the host completes
 * before the first poll (a few us) must not hang us. */
void ep_arm(uint8_t epi, uint8_t mode, uint8_t mps, uint16_t off, uint16_t n)
{
    uint8_t bit = 1 << epi, i = 255;
    ep_cfg(epi, mode, mps, off);
    CNTH = n >> 8;
    CNTL = (uint8_t)n;
    while (!(BR & bit) && --i) ;
}

/* mode / toggle / offset only, BR cleared, not armed (sInitEp1Ep2 b21:1959: EP2 IN "64 = 0") */
static void ep_idle(uint8_t epi, uint8_t mps, uint16_t off)
{
    ep_cfg(epi, EPMODE_NORMAL, mps, off);
    CNTH = 0;
}

/* clear BR (USB_Reset_ISR b21:165a: "60 = n; 64 = 0") */
void ep_disarm(uint8_t epi)
{
    EPI = epi;
    CNTH = 0;
}

void usb_check(void)
{
    hal_watchdog();
    if (!(USBST & USBST_VBUS)) longjmp(usb_jb, AB_UNPLUG);
    if (USBIRQ & USBIRQ_RESET) longjmp(usb_jb, AB_RESET);
}

static void ep0_setup(void);

/* wait for the transfer armed (ep_arm) on `brbit` to finish: BR = 0 (b21:2007 / 1c5e).
 * timeout in ticks (0 = none) -> returns 0; W_EP0: returns 0 when a new SETUP arrives (the host
 * abandoned this control transfer); W_SVC: SETUPs are served meanwhile. MENU stops the module. */
uint8_t ep_wait(uint8_t brbit, uint8_t flags, uint16_t timeout)
{
    uint16_t t0 = hal_ticks();
    event_t e;
    for (;;) {
        usb_check();
        if (!(BR & brbit)) return 1;
        if ((flags & W_EP0) && (USBIRQ & USBIRQ_SETUP)) return 0;
        if ((flags & W_SVC) && (USBIRQ & USBIRQ_SETUP)) ep0_setup();
        if (ev_get(&e) && e.type == EV_KEY_DOWN && e.code == KEY_MENU)
            longjmp(usb_jb, AB_STOP);       /* other keys are dropped while a transfer waits */
        if (timeout && (uint16_t)(hal_ticks() - t0) >= timeout) return 0;
    }
}

/* ---- attach / detach / bus reset ------------------------------------------------------------- */
static void bus_reset(void)                 /* USB_Reset_ISR b21:1637 */
{
    uint8_t i;
    usb_cfg = 0; addr = 0; usb_halt = 0;
    USBADDR = USB_ADDR_EN;                  /* address 0, enabled */
    do {                                    /* clear BR of every endpoint until all are idle */
        for (i = EPI_CR; i <= EPI_EP3OUT; i++) ep_disarm(i);
        hal_watchdog();
        if (!(USBST & USBST_VBUS)) longjmp(usb_jb, AB_UNPLUG);
    } while (BR);
    USBIRQ = USBIRQ_ALL; EPIRQ = 0xFF; NAKST = 0xFF; STALLST = 0xFF;
    CLKCTL2 &= ~CLK2_BANKS;                 /* both ZRAM2 banks to the USB SIE */
    usb_state = US_DEFAULT;
    usb_nreset++;
    usb_log('R', 0, 0, 0, usb_nreset);
    usb_dirty = 1;
#ifdef USB_AUDIO_ONLY
    usb_audio_reset();
#else
    if (usb_fn == FN_MSC) msc_reset(); else cdc_reset();
#endif
}

static void detach(void)                    /* UDiskExit text:0ee6 (without the clock off) */
{
    uint8_t i;
    PULLUP &= ~USB_PULLUP_EN;
    for (i = EPI_CR; i <= EPI_EP3OUT; i++) ep_disarm(i);
    USBIRQ = USBIRQ_ALL; EPIRQ = 0xFF;
    CLKCTL2 |= CLK2_BANKS;                  /* ZRAM2 back to the MCU */
    usb_cfg = 0;
#ifdef USB_AUDIO_ONLY
    usb_audio_reset();
#else
    if (usb_fn == FN_MSC) msc_reset(); else cdc_reset();
#endif
    usb_dirty = 1;
}

/* UDiskRun b21:21ef: refuse while VBUS is off, or while D+ and D- both read high with the
 * pull-up off (stock guard; inferred: charger / no host). Then ack everything, pull-up on. */
static void attach(void)
{
    uint8_t st = USBST;
    usb_dirty = 1;
    if (!(st & USBST_VBUS)) { usb_state = US_NOVBUS; return; }
    if ((st & (USBST_DP | USBST_DM)) == (USBST_DP | USBST_DM) && !(PULLUP & USB_PULLUP_EN)) {
        usb_state = US_CHARGER;
        return;
    }
    CLKCTL2 &= ~CLK2_BANKS;
    USBADDR = USB_ADDR_EN;
    USBIRQ = USBIRQ_ALL; EPIRQ = 0xFF;
    PULLUP |= USB_PULLUP_EN;                /* soft connect: the host sees us now */
    usb_state = US_ATTACHED;
}

static void wait_ticks(uint8_t n)
{
    uint16_t t = hal_ticks();
    while ((uint16_t)(hal_ticks() - t) < n) hal_watchdog();
}

void usb_start(uint8_t fn)
{
    usb_fn = fn;
#ifndef USB_AUDIO_ONLY
    memcpy(ZR->stub, usbz_tpl, usbz_tpl_end - usbz_tpl);
#endif
    /* 48 MHz for the SIE (UDiskInitInternal b20:1648: 42h |= E0h, then a short delay) */
    if (!pll_set) {                         /* first start: remember the value to restore */
        pll_saved = PLLCTL;
        pll_set = 1;
    }
    if ((PLLCTL & (PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN)) != (PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN)) {
        PLLCTL |= PLL_HOSCEN | PLL_PLL1EN | PLL_M48EN;
        wait_ticks(4);                      /* >= 30 ms PLL lock (clock.h) */
    }
    PULLUP &= ~USB_PULLUP_EN;
    usb_cfg = 0;
    usb_state = US_NOVBUS;
    attach();
}

/* leave: pull-up off (host sees the unplug), endpoints idle, ZRAM2 to the MCU */
void usb_stop(void)
{
    detach();
    usb_state = US_OFF;
}

/* module exit: PLLCTL as found at the first start (the stock UDiskExit writes 0x80; zos keeps
 * whatever it had). keep_pll: the CPU itself runs from the PLL - leave it on. */
void usb_stop_clock(uint8_t keep_pll)
{
    if (pll_set && !keep_pll) PLLCTL = pll_saved;
    pll_set = 0;
}

void usb_recover(uint8_t why)
{
    if (why == AB_RESET) bus_reset();
    else if (why == AB_UNPLUG) { usb_log('U', 0, 0, 0, 0); detach(); usb_state = US_NOVBUS; }
}

/* ---- EP0 -------------------------------------------------------------------------------------- */
static void ep0_stall(void)                 /* syntax_err b21:1ac5 */
{
    ep_arm(EPI_CW, EPMODE_STALL, MPS_EP0_DATA1, 0, 0);
}

/* status stage of a request without data (no_data_control b21:1b46) */
static uint8_t ep0_ack(void)
{
    ep_arm(EPI_CW, EPMODE_CWDONE, MPS_EP0_DATA1, ZO_EP0, 0);
    if (!ep_wait(EPB_CW, W_EP0, 0)) { ep_disarm(EPI_CW); return 0; }
    EPIRQ = EPB_CW;
    return 1;
}

static uint8_t ep0_send(uint8_t mode, uint8_t mps, uint8_t n)   /* sWait_Ep0_Send_Out b21:1ff1 */
{
    ep_arm(EPI_CR, mode, mps, ZO_EP0, n);
    if (!ep_wait(EPB_CR, W_EP0, 0)) { ep_disarm(EPI_CR); return 0; }
    EPIRQ = EPB_CR;
    return 1;
}

/* control read data stage from ZR->ep0 (control_read b21:1acf): length clipped to wLength (only
 * when wLength < 256, as get_descriptor_length b21:1b2e); a reply that is a multiple of 8 is
 * followed by zero-length packets while the hardware says the transfer is still open (57h.5). */
static void ep0_in(uint8_t n)
{
    if (!usb_su[7] && usb_su[6] && usb_su[6] < n) n = usb_su[6];
    zr_put(ZO_EP0, ZR->ep0, n);
    if (n & 7) { ep0_send(EPMODE_CRLAST, MPS_EP0_DATA1, n); return; }
    if (!ep0_send(EPMODE_NORMAL, MPS_EP0_DATA1, n)) return;
    if (!(SETUPST & SETUPST_LOCK)) return;
    if (!ep0_send(EPMODE_NORMAL, MPS_EP0, 0)) return;
    if (SETUPST & SETUPST_LOCK) ep0_send(EPMODE_CRLAST, MPS_EP0, 0);
}

/* control write data stage into ZR->ep0 (CDC SET_LINE_CODING). INFERRED: the stock firmware never
 * receives EP0 OUT data; by symmetry with the no-data status stage (EPI_CW, EPMODE_CWDONE, count
 * 0) the CW endpoint is armed with the expected length, receives it and then completes the
 * status stage. 0 = timed out (counted in usb_lcfail, shown by the console "usb" command). */
uint8_t ep0_out(uint8_t n)
{
    ep_arm(EPI_CW, EPMODE_CWDONE, MPS_EP0_DATA1, ZO_EP0, n);
    if (!ep_wait(EPB_CW, W_EP0, 50)) { ep_disarm(EPI_CW); usb_lcfail++; return 0; }
    EPIRQ = EPB_CW;
    zr_get(ZR->ep0, ZO_EP0, n);
    usb_lcok++;
    return 1;
}

void ep0_reply(const void *src, uint8_t n)  /* src in the module page or ZRAM1 */
{
    memcpy(ZR->ep0, src, n);
    ep0_in(n);
}

/* SET_CONFIGURATION(1): bulk endpoints to DATA0, EP1 OUT armed (sInitEp1Ep2 b21:1944) */
static void set_config(void)
{
    ep_idle(EPI_EP2IN, MPS_BULK_DATA0, ZO_TX);
    ep_idle(EPI_EP1OUT, MPS_BULK_DATA0, ZO_RX); /* DATA0; the class arms it with MPS_BULK (keeps it) */
#ifdef USB_AUDIO_ONLY
    usb_audio_configured();
#else
    if (usb_fn == FN_CDC) {                 /* notification endpoint 0x81: never armed (NAK) */
        ep_idle(EPI_EP1IN, MPS_INT8_DATA0, ZO_TX);
        cdc_configured();
    } else msc_configured();                /* arms EP1 OUT */
#endif
}

/* endpoint halt (set_endpoint_stall b21:1999, clear_endpoint*_stall b21:1a0f/1a46) */
static uint8_t halt_bit(uint8_t ep)
{
    return ep == 0x01 ? 1 : ep == 0x82 ? 2 : ep == 0x81 ? 4 : 0;
}

static void clear_halt(uint8_t ep)
{
    uint8_t epi = ep == 0x01 ? EPI_EP1OUT : ep == 0x82 ? EPI_EP2IN : EPI_EP1IN;
    usb_halt &= ~halt_bit(ep);
    usb_log('H', ep, 0, 0, 0);
    do { ep_disarm(epi); usb_check(); } while (BR & (1 << epi));
    ep_idle(epi, epi == EPI_EP1IN ? MPS_INT8_DATA0 : MPS_BULK_DATA0, epi == EPI_EP1OUT ? ZO_RX : ZO_TX);
    if (epi == EPI_EP1OUT) {                /* stock re-arms the CBW buffer here */
#ifndef USB_AUDIO_ONLY
        if (usb_fn == FN_MSC) msc_configured(); else cdc_configured();
#endif
    }
}

void usb_stall_bulk(void)                   /* not_a_valid_CBW b21:17c9: stall 0x01 and 0x82 */
{
    usb_halt |= 3;
    ep_arm(EPI_EP1OUT, EPMODE_STALL, MPS_BULK_DATA0, ZO_RX, 0x40);
    ep_arm(EPI_EP2IN, EPMODE_STALL, MPS_BULK_DATA0, ZO_TX, 0x40);
}

static void std_request(void)
{
    uint8_t rt = usb_su[0], rq = usb_su[1], v = usb_su[2], vh = usb_su[3], ix = usb_su[4];
    uint16_t len;
    const uint8_t *d;
    uint8_t st[2];
    st[0] = st[1] = 0;
    switch (rq) {
    case 6:                                 /* GET_DESCRIPTOR */
        if (rt != 0x80) break;
        if (vh == 1) { ep0_reply(desc_device(), 18); return; }
        if (vh == 2) { d = desc_config(&len); ep0_reply(d, (uint8_t)len); return; }
        if (vh == 3 && (len = desc_string(v, ZR->ep0))) { ep0_in((uint8_t)len); return; }
        break;                              /* device qualifier, others: stall (full speed only) */
    case 5:                                 /* SET_ADDRESS: after the status stage (b21:1915) */
        if (rt != 0x00) break;
        addr = v & 0x7F;
        if (ep0_ack()) {
            USBADDR = addr | USB_ADDR_EN;
            usb_state = addr ? US_ADDRESS : US_DEFAULT;
            usb_dirty = 1;
        }
        return;
    case 9:                                 /* SET_CONFIGURATION 0/1 */
        if (rt != 0x00 || v > 1) break;
        usb_cfg = v;
        if (v) set_config();
        else {
#ifdef USB_AUDIO_ONLY
            usb_audio_reset();
#else
            if (usb_fn == FN_MSC) msc_reset(); else cdc_reset();
#endif
        }
        usb_state = v ? US_CONFIGURED : US_ADDRESS;
        usb_dirty = 1;
        ep0_ack();
        return;
    case 8:                                 /* GET_CONFIGURATION */
        if (rt != 0x80) break;
        ep0_reply(&usb_cfg, 1);
        return;
    case 10:                                /* GET_INTERFACE: alternate setting 0 */
#ifdef USB_AUDIO_ONLY
        if (usb_audio_std_request()) return;
#endif
        if (rt != 0x81) break;
        ep0_reply(st, 1);
        return;
    case 11:                                /* SET_INTERFACE: alternate setting 0 only */
#ifdef USB_AUDIO_ONLY
        if (usb_audio_std_request()) return;
#endif
        if (rt != 0x01 || v) break;
        ep0_ack();
        return;
    case 0:                                 /* GET_STATUS */
        if (rt == 0x82) st[0] = (usb_halt & halt_bit(ix)) ? 1 : 0;
        else if (rt != 0x80 && rt != 0x81) break;
        ep0_reply(st, 2);
        return;
    case 1:                                 /* CLEAR_FEATURE */
    case 3:                                 /* SET_FEATURE */
        if (rt == 0x00 || rt == 0x01) { ep0_ack(); return; }     /* remote wakeup etc.: accepted */
        if (rt != 0x02 || v != 0 || !halt_bit(ix)) break;        /* ENDPOINT_HALT only */
        if (rq == 1) clear_halt(ix);
        else {
            usb_halt |= halt_bit(ix);
            usb_log('H', ix, 1, 0, 0);
            if (ix == 0x01) ep_arm(EPI_EP1OUT, EPMODE_STALL, MPS_BULK_DATA0, ZO_RX, 0x40);
            else if (ix == 0x82) ep_arm(EPI_EP2IN, EPMODE_STALL, MPS_BULK_DATA0, ZO_TX, 0x40);
        }
        ep0_ack();
        return;
    }
    ep0_stall();
}

/* EP0_SETUP_ISR b21:16a4: ack, check the SETUP status, read the 8 bytes, dispatch */
static void ep0_setup(void)
{
    USBIRQ = USBIRQ_SETUP;
    usb_nsetup++;
    if ((SETUPST & SETUPST_MASK) != SETUPST_OK) { usb_log('S', 0xFF, SETUPST, 0, 0); ep0_stall(); return; }
    usb_su[0] = SU0; usb_su[1] = SU1; usb_su[2] = SU2; usb_su[3] = SU3;
    usb_su[4] = SU4; usb_su[5] = SU5; usb_su[6] = SU6; usb_su[7] = SU7;
    usb_log('S', usb_su[0], usb_su[1], usb_su[3], usb_su[6] | (usb_su[7] << 8));
    if ((usb_su[0] & 0x60) == 0x20) {       /* class request */
#ifdef USB_AUDIO_ONLY
        if (usb_audio_setup()) return;
#else
        if (usb_fn == FN_MSC ? msc_setup() : cdc_setup()) return;
#endif
        ep0_stall();
        return;
    }
    std_request();
}

uint8_t ep0_ack_class(void) { return ep0_ack(); }

/* main-loop service: cable, attach, bus reset, SETUP (bus reset / unplug inside a SETUP longjmp) */
void usb_service(void)
{
    uint8_t irq;
    hal_watchdog();
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
