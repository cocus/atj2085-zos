/* newbrec target "ximpro" (ATJ2085 + LISONG LSD_501 board): I/O ports and bits used by the boot record.
 * Own definitions (not shared with zos). Sources: ATJ2085X programming guide, lisong_rebuild/analysis
 * (atj2085_hw.md, boot.md, nand_ftl.md, usb_msc.md). build.py turns every plain numeric
 * "#define NAME value" into regs.inc for the assembler: keep the values plain. */
#ifndef XIMPRO_REGS_H
#define XIMPRO_REGS_H

/* clock / external bus */
#define P_CLKCTL        0x00
#define P_EMLO          0x01    /* NAND bus phase of window writes: */
#define EM_DATA         0x00
#define EM_CLE          0x01
#define EM_ALE          0x02
#define P_EMHI          0x02    /* bits 5-3 = chip enable of the 0x8000 window */
#define EMHI_CE1_NAND   0x08
#define EMHI_CE3_LCD    0x18
#define P_PAGE          0x05    /* 0x4000-0x7FFF window */
#define PAGE_ZRAM2      0x07
#define P_CLKCTL2       0x70
#define CLK2_B1_MCU     0x20    /* ZRAM2 bank B1 (USB 0x000-0x7FF) to the MCU, 0 = USB */
#define CLK2_B2_MCU     0x10
#define CLK2_BANKS      0x30
#define P_PLLCTL        0x42
#define PLL_USB48       0xE0    /* HOSC + PLL1 + 48 MHz enable */

/* NAND sequencer (CE1) */
#define P_NAND_EN       0x28
#define NAND_EN_CE1     0x02
#define P_NAND_CEMODE   0x29
#define NAND_CEMODE_SAMSUNG 0x01
#define P_NAND_CMD      0x2A
#define NAND_READ1      0x00
#define NAND_READ2      0x30
#define NAND_ID         0x90
#define NAND_STATUS     0x70
#define NAND_STOP       0x37
#define NAND_PROG1      0x80
#define NAND_RNDIN      0x85
#define NAND_PROG2      0x10
#define NAND_ERASE1     0x60
#define NAND_ERASE2     0xD0
#define NAND_RESET      0xFF
#define NAND_ST_FAIL    0x01
#define NAND_ST_READY   0x40
#define NAND_ST_NWP     0x80
#define NAND_SPARE      0x800
#define P_ECC_CTL       0xCC
#define ECC_START       0x10
#define ECC_BUSY        0x10
#define P_ECC_SP8       0xCD
#define P_ECC_SP9       0xCC
#define P_ECC_SP10      0xCF
#define P_ECC_SP11      0xCE

/* RTC, watchdog, power */
#define P_RTCCTL        0x43
#define RTCCTL_TIMERS   0x60
#define P_RTCST         0x44
#define RTC_TICK        0x02
#define P_LOSCDIV_L     0x4B
#define P_LOSCDIV_M     0x4C
#define P_LOSCDIV_H     0x4D
#define P_WDT           0x4E
#define WDT_CLR         0x08
#define WDT_REBOOT      0x88
#define P_DCDC          0x4F
#define P_PWRCTL        0xDF

/* pins: keys (GPIO B), LCD A0 / reset / backlight */
#define P_KEYSCAN       0xC1
#define P_GPB_CFG       0xEF
#define P_GPB_OE        0xF0
#define P_GPB_IE        0xF1
#define P_GPB_D         0xF2
#define P_GPC_EN        0xF3
#define P_GPC_D         0xF4
#define P_GPG_EN        0xFE
#define P_MFP           0xEE
#define KEYB_ROWS       0x66
#define KEYB_LEFT       0x02
#define KEYB_MENU       0x04
#define KEYB_VOL        0x20
#define KEYB_RIGHT      0x40
#define KEYB_DRIVE      0x40
#define KEYB_PLAY       0x02

/* USB full-speed SIE */
#define P_USB_PULLUP    0x3E
#define USB_PULLUP_EN   0x80
#define P_USB_STAT      0x50
#define USBST_VBUS      0x40
#define USBST_DP        0x20
#define USBST_DM        0x10
#define P_USB_ADDR      0x51
#define USB_ADDR_EN     0x80
#define P_USB_IRQ       0x52
#define USBIRQ_RESET    0x80
#define USBIRQ_CONN     0x40
#define USBIRQ_SETUP    0x20
#define USBIRQ_WAKE     0x10
#define P_USB_EPIRQ     0x53
#define P_USB_SETUPST   0x57
#define SETUPST_LOCK    0x20
#define SETUPST_MASK    0xDF
#define SETUPST_OK      0x58
#define P_USB_SETUP0    0x58
#define P_USB_SETUP1    0x59
#define P_USB_SETUP2    0x5A
#define P_USB_SETUP3    0x5B
#define P_USB_SETUP4    0x5C
#define P_USB_SETUP5    0x5D
#define P_USB_SETUP6    0x5E
#define P_USB_SETUP7    0x5F
#define P_USB_EPI       0x60
#define EPI_CR          0
#define EPI_CW          1
#define EPI_EP1IN       2
#define EPI_EP1OUT      3
#define EPI_EP2IN       4
#define EPI_EP3OUT      7
#define EPB_CR          0x01
#define EPB_CW          0x02
#define EPB_EP1OUT      0x08
#define EPB_EP2IN       0x10
#define P_USB_EPMODE    0x61
#define EPMODE_NORMAL   0x00
#define EPMODE_STALL    0x01
#define EPMODE_CWDONE   0x02
#define EPMODE_CRLAST   0x03
#define P_USB_SAH       0x62
#define P_USB_SAL       0x63
#define P_USB_CNTH      0x64
#define P_USB_CNTL      0x65
#define P_USB_MPS       0x66
#define MPS_EP0_DATA1   0xC7
#define MPS_EP0         0x07
#define MPS_BULK        0x3F
#define MPS_BULK_DATA0  0xBF
#define P_USB_BR        0x68
#define P_USB_NAKST     0x69
#define P_USB_STALLST   0x6A

/* fixed */
#define EXT_WINDOW      0x8000
#define LCD_DATA        0x8000
#define PAGE_BASE       0x4000

/* LCD controller (SSD1789A-compatible, 132x132 RGB565, CE3, A0 = GPIO C2) */
#define LCD_CASET       0x15
#define LCD_PASET       0x75
#define LCD_RAMWR       0x5C
#define LCD_ROW0        4       /* first visible controller row */
#define GPC_LCD_A0      0x04
#define MFP_LCD_RESET   0x01
#define MFP_BACKLIGHT   0x02

#if defined(__SDCC) && !defined(REGS_NO_SFR)
#define SFR(name, port) __sfr __at(port) name
#else
#define SFR(name, port) extern volatile unsigned char name
#endif
#endif
