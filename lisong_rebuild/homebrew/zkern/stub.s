;; zkern stage 0: the first 512 bytes of the new SYSCFG.SYS. BREC loads that file exactly like the
;; stock kernel (file 0x0000-0x2CFF -> RAM 0x0600-0x32FF, file 0x3700-0x377F -> 0x0000-0x007F,
;; BREC 0x0216-0x035A) and enters here with `ld sp,0x0400 ; jp 0x0600` (BREC 0x020F), DI.
;;
;; Decision, before anything else is changed:
;;   MENU or VOL held (GPIO B2/B5, after the KY_HARD GPIO init) -> stock
;;   zos image invalid (jp at 0x0800 + 16-bit sum)      -> stock
;;   otherwise                                          -> zos (takeover start, no stock code ran)
;;
;; stock = chain-load the renamed stock kernel ("SYSCFG  STK") with BREC's own loader: patch the
;; 11 name bytes BREC 0x0274.. compares against, then re-enter BREC at 0x020C on BREC's stack, so
;; the kernel is loaded and entered exactly as on a stock boot (same code path, same SP, same
;; BC/DE/IX, interrupts and IRQ enables untouched). The only state the stub leaves behind: GPIO B
;; OE/IE restored, RAM 0x0100-0x01FF (BREC's stack area) and the name bytes in BREC's loader (dead
;; code once the kernel runs). If BREC does not find "SYSCFG  STK" its loader returns and jumps to
;; 0x0600 again = this stub; the name check below then sees "STK" and starts zos instead of looping.
;;
;; zos = do what the stock kernel / MAIN.AP / KY_HARD.DRV did before zos ran in hosted mode
;; (RTC dividers + timers, watchdog period, DC-DC, GPIO B/G set-up, IM 1; LCD and NAND are left as
;; BREC initialised them, no LCD reset), then enter zos at 0x0800 with SP = 0x0800.
	.module zkstub
	.include "regs.inc"
	.include "zkregs.inc"

	.area	_CODE
stub::
	jr	start
;; ---- parameter block (filled in by zkern/build.py)
p_magic:	.ascii	"ZKS1"
p_zlen:		.dw	0			; zos image length (bytes from ZOS_BASE)
p_zsum:		.dw	0			; 16-bit sum of those bytes
p_stk:		.ascii	"SYSCFG  STK"		; directory name of the stock kernel
p_sys:		.ascii	"SYSCFG  SYS"		; what BREC 0x0274.. looks for originally

start:
	ld	(sv_bc), bc			; BREC's registers, restored for the stock kernel
	ld	(sv_de), de			; (no push yet: SP = 0x0400 sits on BREC code 0x03F4)
	ld	sp, #BREC_LOAD_SP

	call	keys_gpio			; the key pins are KEYI/KEYO (hardware scan) out of reset:
	call	menu_held			; GPIO first, or MENU always reads "not pressed" (device)
	jr	c, go_stock
	call	zos_valid
	jr	nc, go_zos
	;; fall through: zos broken -> stock

;; ------------------------------------------------------------------ chain-load the stock kernel
go_stock::
	ld	hl, #BREC_NAMECMP		; BREC must be the known one and still say "SYSCFG  SYS";
	ld	de, #p_sys			; if it already says "SYSCFG  STK" a previous chain-load
	call	name_cmp			; failed (no such file) -> start zos instead of looping
	jr	nz, go_zos
	ld	hl, #BREC_NAMECMP + 1
	ld	de, #p_stk
	ld	b, #11
1$:	ld	a, (de)
	ld	(hl), a
	inc	de
	ld	a, l
	add	a, #BREC_NAMESTRIDE
	ld	l, a				; (0x0275..0x02C5: no carry into H)
	djnz	1$
	ld	bc, (sv_bc)
	ld	de, (sv_de)
	ld	sp, #BREC_LOAD_SP
	jp	BREC_LOADSYS

;; Z = the 11 `cp #c` instructions at HL compare against the 11 chars at DE
name_cmp:
	ld	b, #11
1$:	ld	a, (hl)
	cp	a, #Z80_CP_N
	ret	nz
	inc	hl
	ld	a, (de)
	cp	a, (hl)
	ret	nz
	inc	de
	ld	a, l
	add	a, #BREC_NAMESTRIDE - 1
	ld	l, a
	djnz	1$
	xor	a, a				; Z (the flags above are from `add`)
	ret

;; ------------------------------------------------------------------ start zos
go_zos::
	di
	im	1				; zos' IRQ layer is IM 1 (vector 0x0038)
	;; RTC: 10 ms + 2 Hz timers, as the stock kernel's init (SYSCFG 0x096A-0x097B)
	ld	a, #LOSCDIV_STOCK_L
	out	(P_LOSCDIV_L), a
	ld	a, #LOSCDIV_STOCK_H
	out	(P_LOSCDIV_H), a
	ld	a, #LOSCDIV_STOCK_M
	out	(P_LOSCDIV_M), a
	in	a, (P_RTCCTL)
	or	a, #RTCCTL_TIMERS
	out	(P_RTCCTL), a
	;; watchdog period and DC-DC as MAIN.AP (bank40 0x14b8-0x14ca)
	in	a, (P_WDT)
	or	a, #WDT_CLR
	out	(P_WDT), a
	in	a, (P_WDT)
	and	a, #0xFF - WDT_PERIOD_MASK
	or	a, #WDT_PERIOD_MAIN
	out	(P_WDT), a
	ld	a, #DCDC_STOCK
	out	(P_DCDC), a
	call	keys_gpio
	;; I2C lines (C0 SCL, C1 SDA) released to inputs as the stock firmware leaves them at its
	;; main menu (port F3 07 -> 04); LCD A0 (C2) stays an output
	in	a, (P_GPC_EN)
	and	a, #0xFF - (GPC_SCL | GPC_SDA)
	out	(P_GPC_EN), a
	ld	sp, #ZOS_STACK
	jp	ZOS_BASE

;; ------------------------------------------------------------------ checks
;; GPIO B/G as KY_HARD.DRV's init (0x041E-0x0433): keys read by software (not the KEYI/KEYO scanner)
keys_gpio:
	xor	a, a
	out	(P_GPB_CFG), a
	out	(P_KEYSCAN), a
	out	(P_GPB_D), a
	out	(P_GPB_OE), a
	ld	a, #GPB_IE_STOCK
	out	(P_GPB_IE), a
	ld	a, #GPG_G0_IE
	out	(P_GPG_EN), a
	ret

;; CY = MENU or VOL held: 16 samples ~1.6 ms apart, at least 12 pressed. GPIO B: only OE/IE are
;; changed (phase 1 of the stock/zos key scan reads B1/B2/B5/B6 as inputs) and restored afterwards.
menu_held::
	in	a, (P_GPB_OE)
	ld	(sv_oe), a
	and	a, #0xFF - KEYB_ROWS
	out	(P_GPB_OE), a
	in	a, (P_GPB_IE)
	ld	(sv_ie), a
	or	a, #KEYB_ROWS
	out	(P_GPB_IE), a
	ld	c, #0				; pressed count
	ld	e, #16				; samples
1$:	ld	hl, #1500			; ~1.6 ms at 24.576 MHz (26 T per loop)
2$:	dec	hl
	ld	a, h
	or	a, l
	jr	nz, 2$
	in	a, (P_GPB_D)
	and	a, #KEYB_MENU | KEYB_VOL
	jr	z, 3$
	inc	c
3$:	dec	e
	jr	nz, 1$
	ld	a, (sv_oe)
	out	(P_GPB_OE), a
	ld	a, (sv_ie)
	out	(P_GPB_IE), a
	ld	a, c
	cp	a, #12				; CY = c < 12 = not held
	ccf
	ret

;; CY = zos image not usable: length 1..ZOS_MAXLEN, a jp at ZOS_BASE, 16-bit byte sum matches
zos_valid::
	ld	bc, (p_zlen)
	ld	a, b
	or	a, c
	scf
	ret	z
	ld	hl, #ZOS_MAXLEN
	or	a, a
	sbc	hl, bc
	ret	c
	ld	a, (ZOS_BASE)
	cp	a, #Z80_JP
	scf
	ret	nz
	ld	hl, #ZOS_BASE
	ld	de, #0
1$:	ld	a, (hl)
	add	a, e
	ld	e, a
	jr	nc, 2$
	inc	d
2$:	inc	hl
	dec	bc
	ld	a, b
	or	a, c
	jr	nz, 1$
	ld	hl, (p_zsum)
	or	a, a
	sbc	hl, de
	ret	z				; CY = 0: valid
	scf
	ret

sv_bc:	.dw	0
sv_de:	.dw	0
sv_oe:	.db	0
sv_ie:	.db	0

;; ------------------------------------------------------------------ vector page (RAM 0x0000-0x007F)
;; Loaded by BREC from file 0x3700. zos installs its own IM1 handler (zos_hook_irq); until then an
;; interrupt just returns, and any jump to 0 / RST (zos never uses them) reboots via the watchdog.
;; 0x0037 must stay non-zero (0x76 filler): BREC's flash access wrapper (0x3D3A/0x3D74) counts
;; nesting there and executes EI when it drops to 0 - as with BREC's own page during the load.
	.area	_VEC (ABS)
	.org	0x0000
vec_reset::
	di
	ld	a, #WDT_REBOOT
	out	(P_WDT), a
1$:	jr	1$
	.ds	0x08 - 7
	jp	vec_reset			; 0x08 .. 0x30: RST n
	.ds	0x10 - 0x0B
	jp	vec_reset
	.ds	0x18 - 0x13
	jp	vec_reset
	.ds	0x20 - 0x1B
	jp	vec_reset
	.ds	0x28 - 0x23
	jp	vec_reset
	.ds	0x30 - 0x2B
	jp	vec_reset
	.ds	0x38 - 0x33
	ei					; 0x38: IM1 (replaced by zos)
	reti
	.ds	0x66 - 0x3B
	retn					; 0x66: NMI
	.ds	0x80 - 0x68
