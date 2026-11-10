/* newbrec target ximpro: clocks / watchdog / RTC tick / key pins / VBUS / NAND geometry. Own code.
 * The values written in tgt_init are the machine state the stock OS and the zos native boot stub
 * use before they run the USB and the keys (device-verified there); the BREC itself starts from the
 * state the mask ROM leaves (24.576 MHz, 0 wait states). */
#include "regs.h"
#include "../../core/target.h"

SFR(WDT, P_WDT);
SFR(RTCCTL, P_RTCCTL);
SFR(RTCST, P_RTCST);
SFR(PLLCTL, P_PLLCTL);
SFR(GPB_OE, P_GPB_OE);
SFR(GPB_IE, P_GPB_IE);
SFR(GPB_D, P_GPB_D);
SFR(USBST, P_USB_STAT);

SFR(loscl, P_LOSCDIV_L);
SFR(loscm, P_LOSCDIV_M);
SFR(loscg, P_LOSCDIV_H);
SFR(dcdc, P_DCDC);
SFR(gpbcfg, P_GPB_CFG);
SFR(keyscan, P_KEYSCAN);
SFR(gpgen, P_GPG_EN);

nand_req_t nreq;

void tgt_init(void)
{
    WDT |= 0x40;                    /* longer watchdog period, as the stock boot record does */
    WDT |= WDT_CLR;
    loscl = 0xA4; loscm = 0x01; loscg = 0x00;   /* RTC: 10 ms timer */
    RTCCTL |= RTCCTL_TIMERS;
    dcdc = 0xA5;
    /* keys: GPIO B must be plain GPIO and the hardware scanner off BEFORE the first read (out of
     * reset the pins are KEYI/KEYO; reading MENU earlier never sees it) */
    gpbcfg = 0; keyscan = 0;
    GPB_D = 0; GPB_OE = 0;
    GPB_IE = 0x77;
    gpgen = 0x10;
}

uint16_t tgt_ticks(void)
{
    static uint16_t t;
    uint8_t s = RTCST;
    if (s & RTC_TICK) { RTCST = s; t++; }
    return t;
}

/* board wiring (stock key driver): phase 1 reads B1 = LEFT, B2 = MENU, B6 = RIGHT with the rows as
 * inputs; phase 2 drives B6 high and reads B1 = PLAY. B5 is the VOL pad (no key on this player). */
uint8_t tgt_key(void)
{
    uint8_t c, i;
    GPB_OE &= ~KEYB_ROWS;
    GPB_IE |= KEYB_ROWS;
    for (i = 0; i < 32; i++) ;
    c = GPB_D & KEYB_ROWS;
    if (c) {
        if (c & KEYB_MENU) return KEY_MENU;
        if (c & KEYB_LEFT) return KEY_LEFT;
        if (c & KEYB_RIGHT) return KEY_RIGHT;
        return KEY_NONE;
    }
    GPB_OE |= KEYB_DRIVE;
    GPB_D |= KEYB_DRIVE;
    return (GPB_D & KEYB_PLAY) ? KEY_PLAY : KEY_NONE;
}

uint8_t tgt_vbus(void) { return (USBST & USBST_VBUS) != 0; }

void tgt_usb_clock(void)
{
    if ((PLLCTL & PLL_USB48) != PLL_USB48) {
        PLLCTL |= PLL_USB48;
        tgt_delay_ms(40);           /* PLL lock */
    }
}

/* chips this board has: Samsung-type 2 KB page, 64 pages per block; blocks by device id */
uint32_t tgt_nand_blocks(const uint8_t *id)
{
    if (id[0] != 0xEC && id[0] != 0xAD && id[0] != 0x20) return 0;
    if (id[1] == 0xD3) return 8192;        /* 8 Gbit (this player: EC D3 51 95) */
    if (id[1] == 0xDC) return 4096;        /* 4 Gbit */
    return 0;
}
