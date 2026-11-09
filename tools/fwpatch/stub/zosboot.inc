;; zosboot: hardware ports and boot-record (BREC) addresses used by the stage-0 boot selector.
;; Only our own constants: no stock code or data. BREC addresses are those of the Actions
;; "F641" boot record; the stub checks the code at these addresses (16-bit sums, set by
;; zospatch.py) before it uses any of them.

;; ---- ports (ATJ2085)
P_WDT           = 0x4E          ; watchdog control
WDT_CLR         = 0x08
WDT_REBOOT      = 0x88          ; enable + shortest period + clear: chip reset in ~176 ms
WDT_PERIOD_MASK = 0x70
WDT_PERIOD_MAIN = 0x30          ; period the stock main menu runs with
P_RTCCTL        = 0x43
RTCCTL_TIMERS   = 0x60          ; 10 ms + 2 Hz timer enables
P_LOSCDIV_L     = 0x4B
P_LOSCDIV_M     = 0x4C
P_LOSCDIV_H     = 0x4D
LOSCDIV_STOCK_L = 0xA4
LOSCDIV_STOCK_M = 0x01
LOSCDIV_STOCK_H = 0x00
P_DCDC          = 0x4F
DCDC_STOCK      = 0xA5
P_USB_STAT      = 0x50
USBST_VBUS      = 0x40          ; USB cable (VBUS) present
P_KEYSCAN       = 0xC1          ; hardware key scanner: 0 = off
P_GPB_CFG       = 0xEF          ; GPIO B function: 0 = plain GPIO (not KEYI/KEYO)
P_GPB_OE        = 0xF0
P_GPB_IE        = 0xF1
P_GPB_D         = 0xF2
GPB_IE_STOCK    = 0x77
KEYB_ROWS       = 0x66          ; key rows read in scan phase 1
KEYB_MENU       = 0x04
KEYB_VOL        = 0x20
P_GPC_EN        = 0xF3
GPC_SCL         = 0x01
GPC_SDA         = 0x02
P_GPG_EN        = 0xFE
GPG_G0_IE       = 0x10

;; ---- BREC (F641) interface
BREC_LOADSYS    = 0x020C        ; `call load_syscfg ; ld sp,0x0400 ; jp 0x0600`
BREC_LOAD_SP    = 0x0200        ; BREC's stack while it loads the kernel
BREC_NAMECMP    = 0x0274        ; 11 x `cp #c`, stride 8: the "SYSCFG  SYS" name test
BREC_NAMESTRIDE = 8
BREC_FWREAD     = 0x3FDB        ; read(DE -> {u32 byte offset, u8, u16 dest, u8 sel}, BC = count)
BREC_UPZ        = 0x3CAA        ; logical units per zone (the reader only maps zone 0)
BREC_CAP        = 0x008A        ; flash capacity of CE1 in blocks (flash detect result)
BREC_DESC       = 0x3C80        ; zone 0 table descriptor (k << 12 | table unit)
BREC_DESC1      = 0x3C82        ; zone 1 table descriptor
BREC_GEOM       = 0x3CA9        ; flash geometry byte (8 on F641: table copy k at page 2k)
BREC_ADR        = 0x3CA0        ; reader: NAND address + destination block
BREC_SEL        = 0x3D3A        ; reader subroutines (all inside the checked code ranges):
BREC_LOOKUP     = 0x3CF9        ;   select CE (E), table lookup (DE = L -> HL),
BREC_ADDR       = 0x3F1B        ;   address (HL unit, BC sector -> (DE)), transfer (DE, BC),
BREC_XFER       = 0x3DC9        ;   deselect (restores ports, EI when nesting count 0x0037
BREC_DESEL      = 0x3D5F        ;   drops to 0 - it never does with our vector page)

;; ---- layout
ZOS_BASE        = 0x0800        ; zos core image (ZOS.AP text) load address and entry
ZOS_MAXLEN      = 0x2400        ; image must end at or below 0x2C00 (the stub lives there)
ZOS_STACK       = 0x0800
BUF             = 0x3100        ; 512-byte sector buffer (inside BREC's 0x0600-0x32FF load area)

Z80_JP          = 0xC3
Z80_CP_N        = 0xFE
FILL            = 0x76

;; ---- p_flags bits (set by zospatch.py)
F_VBUS          = 0x01          ; USB cable at power-on -> stock
F_KEYS          = 0x02          ; MENU or VOL held at power-on -> stock
