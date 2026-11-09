;; zkern: names the stage-0 stub needs that zos' targets/atj2085/regs.h does not define (yet).
;; Values = what the stock code writes (evidence in analysis/zos_native_boot.md, section 3).

;; ---- ports
P_RTCCTL        = 0x43          ; RTC control (kernel SYSCFG 0x0975: |= 0x60 starts the timers)
RTCCTL_TIMERS   = 0x60          ;   10 ms timer + 2 Hz enable (stock kernel value)
P_LOSCDIV_L     = 0x4B          ; RTC/LOSC divider, 24 bit (kernel writes L, H, then M)
P_LOSCDIV_M     = 0x4C
P_LOSCDIV_H     = 0x4D
LOSCDIV_STOCK_L = 0xA4          ;   0x0001A4 (kernel SYSCFG 0x096A)
LOSCDIV_STOCK_M = 0x01
LOSCDIV_STOCK_H = 0x00
WDT_PERIOD_MASK = 0x70          ; port 0x4E bits 6-4: watchdog period
WDT_PERIOD_MAIN = 0x30          ;   MAIN.AP bank40 0x14be: (4E & 0x8F) | 0x30
P_DCDC          = 0x4F          ; DC-DC converter control
DCDC_STOCK      = 0xA5          ;   MAIN.AP bank40 0x14c6
P_GPB_CFG       = 0xEF          ; GPIO B: 0 = all pins GPIO (no KEYI/KEYO)   (KY_HARD.DRV 0x041F)
P_KEYSCAN       = 0xC1          ; hardware key scan control: 0 = off          (KY_HARD.DRV 0x0421)
P_GPG_EN        = 0xFE          ; GPIO G0 OE/IE
GPG_G0_IE       = 0x10          ;   G0 input enable                           (KY_HARD.DRV 0x042F)
GPB_IE_STOCK    = 0x77          ; KY_HARD.DRV 0x042B

;; ---- BREC (BRECF641.BIN, LSD_501_04; the stub checks these bytes before using them)
BREC_LOADSYS    = 0x020C        ; `call 0x0216 (load SYSCFG.SYS) ; ld sp,0x0400 ; jp 0x0600`
BREC_LOAD_SP    = 0x0200        ; BREC's stack while it loads the kernel
BREC_NAMECMP    = 0x0274        ; 11 x `cp #c` (FE c) + 6 bytes, stride 8: the "SYSCFG  SYS" test
BREC_NAMESTRIDE = 8
Z80_CP_N        = 0xFE

;; ---- layout
ZOS_BASE        = 0x0800        ; zos core image (ZOS.AP text), entry = crt0 `jp _zos_start`
ZOS_MAXLEN      = 0x2400        ; zos build.py limit: image ends below 0x2C00
ZOS_STACK       = 0x0800        ; zos native stack: 0x0100-0x07FF (0x0080-0x00FF = BREC data, 0x008A!)

