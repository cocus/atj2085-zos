;; Power-off = the stock standby (STANDBY.AP, lisong_rebuild/analysis/power.md section 6).
;; The stock firmware never cuts the supply (there is no power-hold pin; the DC-DC converter stays
;; on): it parks the chip in a slow polling loop - SAR ADC off, CPU on the 32 kHz LOSC, watchdog off,
;; HOSC + PLLs off, RTC interrupts off - until PLAY is held or a USB host shows up, then restores
;; everything and carries on (no reset). This is that loop.
;;
;; uint8_t standby_blob(void): copied by main.c into ZRAM1 (STBY_RAM) and run there, because the DSP
;; RAM page this module lives in may not be usable without the PLL (stock runs it from ZRAM1 too).
;; So it is position independent: only relative jumps inside, absolute calls only into the core API
;; (fixed jump table). Interrupts are off from entry to the wake-up and ON when it returns (SHELL
;; always runs with them enabled: nothing to restore, so no LD A,I erratum).
;; Returns WAKE_KEY: PLAY seen released, then pressed and held WAKE_HOLD x 10 ms at full speed (the
;; core ISR then swallows that press and switches the light on, as for any light-waking press), or
;; WAKE_USB: a PC plugged in WHILE off (VBUS with D+/D- not both high, ~0.5 s stable; a cable already
;; there at switch-off is ignored until unplugged; with a charger both lines read high and the player stays
;; asleep, as stock - but unlike stock, PLAY still wakes it while charging). Below BATT_STANDBY_MIN
;; the key is not even looked at (stock).
	.module standby
	.include "regs.inc"
	.globl	_hal_backlight, _hal_watchdog
KEY_NONE   = 0
WAKE_KEY   = 1			; main.c
WAKE_USB   = 2
WAKE_HOLD  = 100		; x 10 ms: PLAY held 1 s to wake up (stock: 8 x ~80 ms)
USB_DEBOUNCE = 30		; polls (~17 ms each at 32 kHz) a PC must be seen before waking into USB
DELAY_10MS = 74			; x 256 djnz (3.3k T-states) = 10 ms at HOSC 24.576 MHz /1
SETTLE     = 32			; key line settling loop, as the stock key driver / core keys_read

;; A = PLAY pressed alone ? nonzero (NZ) : 0 (Z). Uses B, C. Same scan as core keys_read (platform.c).
	.macro	PLAYKEY
	in	a, (P_GPB_OE)		; phase 1: rows are inputs, any row high = another key
	and	a, #(0xFF ^ KEYB_ROWS)
	out	(P_GPB_OE), a
	in	a, (P_GPB_IE)
	or	a, #KEYB_ROWS
	out	(P_GPB_IE), a
	ld	b, #SETTLE
	djnz	.
	in	a, (P_GPB_D)
	and	a, #KEYB_ROWS
	ld	c, a
	in	a, (P_GPB_OE)		; phase 2: drive B6 high, PLAY connects it to B1
	or	a, #KEYB_DRIVE
	out	(P_GPB_OE), a
	in	a, (P_GPB_D)
	or	a, #KEYB_DRIVE
	out	(P_GPB_D), a
	in	a, (P_GPB_D)
	and	a, #KEYB_PLAY
	inc	c
	dec	c
	jr	z, .+3
	xor	a, a
	or	a, a
	.endm

	.area	_CODE
_standby_blob::
	di
	xor	a, a				; light off now (interrupts off: no key can switch it on
	call	_hal_backlight			; again before we sleep)
	call	_hal_watchdog			; = apply
	in	a, (P_CLKCTL2)			; CPU off PLL1 before PLL1 stops
	and	a, #(0xFF ^ CLK2_PLLCPU)
	out	(P_CLKCTL2), a
	in	a, (P_CLKCTL)			; saved state, on the stack: B/C, D/E, H/L
	ld	c, a
	in	a, (P_PLLCTL)
	ld	b, a
	push	bc
	in	a, (P_WDT)
	ld	e, a
	in	a, (P_PWRCTL)
	ld	d, a
	push	de
	in	a, (P_RTCCTL)
	ld	l, a
	in	a, (P_SARADC)
	ld	h, a
	push	hl
sleep:	ld	a, #RTCCTL_STANDBY		; (B..L = saved state here)
	out	(P_RTCCTL), a
	ld	a, h
	and	a, #(0xFF ^ SARADC_EN)
	out	(P_SARADC), a
	ld	a, #CLK_LOSC			; CPU to 32 kHz first ...
	out	(P_CLKCTL), a
	ld	a, #WDT_OFF
	out	(P_WDT), a
	ld	a, b				; ... then HOSC and the PLLs off
	and	a, #(0xFF ^ PLL_STANDBY_OFF)
	out	(P_PLLCTL), a
	ld	a, d
	and	a, #(0xFF ^ PWR_LBNMI_MASK)
	or	a, #PWR_DCOPCLK
	out	(P_PWRCTL), a
	ld	e, #KEY_NONE			; E = 0: PLAY not seen released yet
	ld	l, #USB_DEBOUNCE		; L = polls left until a PC counts (H, L, D are free in the
	ld	d, #0				;   loop: the saved state is on the stack, popped at wake)
	in	a, (P_USB_STAT)			; D = 1: no cable now, so a PC plugged in later may wake us;
	and	a, #USBST_VBUS			;   a cable already there at switch-off never does (the
	jr	nz, poll			;   device woke straight into USB when switched off while
	inc	d				;   connected to a PC)
poll:	in	a, (P_USB_STAT)			; one sample of VBUS and both lines
	and	a, #(USBST_VBUS | USBST_DP | USBST_DM)
	cp	a, #USBST_VBUS
	jr	c, 1$				; no cable: from now on a PC may wake us
	cp	a, #(USBST_VBUS | USBST_DP | USBST_DM)
	jr	z, 3$				; charger: stay asleep (stock ignores PLAY then; we don't)
	inc	d				; a PC: only if armed, and only after USB_DEBOUNCE polls
	dec	d				;   in a row (~0.5 s at 32 kHz)
	jr	z, 2$
	dec	l
	jr	nz, 2$
	ld	a, #WAKE_USB
	jr	wake
1$:	ld	d, #1
3$:	ld	l, #USB_DEBOUNCE
2$:	in	a, (P_BATT)
	and	a, #BATT_LEVEL
	cp	a, #BATT_STANDBY_MIN
	jr	c, poll				; nearly empty: do not even look at the key
	PLAYKEY
	jr	nz, pressed
	ld	e, #1				; not pressed: armed
	jr	poll
hop:	jr	sleep				; (relay for the jump back from resleep)
pressed: inc	e
	dec	e
	jr	z, poll				; still the press that switched us off
	ld	a, #WAKE_KEY
wake:	pop	hl				; saved state back into B..L
	pop	de
	pop	bc
	push	bc
	push	de
	push	hl
	push	af				; why
	ld	a, b				; HOSC (+ PLLs) on; settle ~100 ms at 32 kHz
	out	(P_PLLCTL), a
	ld	b, #0
	djnz	.
	ld	a, c
	out	(P_CLKCTL), a
	ld	a, d
	out	(P_PWRCTL), a
	ld	a, h
	out	(P_SARADC), a
	ld	a, l
	out	(P_RTCCTL), a
	ld	a, e
	or	a, #WDT_CLR
	out	(P_WDT), a
	pop	af
	cp	a, #WAKE_KEY
	jr	nz, done			; USB host
	ld	d, #WAKE_HOLD			; PLAY must stay down (full speed, interrupts still off)
4$:	in	a, (P_WDT)
	or	a, #WDT_CLR
	out	(P_WDT), a
	ld	c, #DELAY_10MS
5$:	ld	b, #0
	djnz	.
	dec	c
	jr	nz, 5$
	PLAYKEY
	jr	z, resleep			; let go too early
	dec	d
	jr	nz, 4$
	ld	a, #WAKE_KEY
done:	pop	hl				; drop the saved state
	pop	de
	pop	bc
	ei
	ret
resleep: pop	hl
	pop	de
	pop	bc
	push	bc
	push	de
	push	hl
	jr	hop
_standby_end::
