;; zos interrupt layer (IM1: every maskable IRQ enters at 0x0038).
;; hosted: zos_hook_irq() saves the stock handler's first 3 bytes at 0x0038 and the master
;; enable (port 0x27), installs JP zos_isr, enables only the sources zos handles.
	.module isr
	.include "regs.inc"
	.globl	_zos_rtc_irq
	.globl	_ev_post
	.area	_DATA
saved_vec:	.ds	3
saved_27:	.ds	1
	.area	_CODE

;; void zos_hook_irq(uint8_t enable_mask)      A = sources for port 0x27 (0x20 = RTC)
_zos_hook_irq::
	di
	ld	l, a
	push	hl
	ld	hl, #IM1_VECTOR
	ld	de, #saved_vec
	ld	bc, #3
	ldir
	ld	a, #Z80_JP			; JP zos_isr
	ld	(IM1_VECTOR), a
	ld	hl, #zos_isr
	ld	(IM1_VECTOR + 1), hl
	in	a, (P_IRQEN)
	ld	(saved_27), a
	pop	hl
	ld	a, l
	out	(P_IRQEN), a
	ei
	ret

;; void zos_unhook_irq(void)
_zos_unhook_irq::
	di
	ld	hl, #saved_vec
	ld	de, #IM1_VECTOR
	ld	bc, #3
	ldir
	ld	a, (saved_27)
	out	(P_IRQEN), a
	ei
	ret

zos_isr:
	push	af
	push	bc
	push	de
	push	hl
	in	a, (P_IRQST)			; master status
	and	a, #IRQ_RTC		; RTC?
	jr	z, 8$
	in	a, (P_RTCST)			; RTC status: bit1 timer (10 ms), bit0 2 Hz
	out	(P_RTCST), a			; W1C pending bits, config bits rewritten unchanged
	call	_zos_rtc_irq			; void zos_rtc_irq(uint8_t status)  (A)
8$:	in	a, (P_IRQEN)			; DSP request (26.0) while its source is enabled?
	ld	b, a
	in	a, (P_IRQST)
	and	a, b
	rrca
	jr	nc, 9$
	in	a, (P_HDR5)			; request code (HDR5), read twice like the stock handler
	in	a, (P_HDR5)
	ld	l, a
	ld	a, #IRQ_DSP		; ack (as the stock handler). The source stays enabled: the
	out	(P_IRQST), a			; DSP sends no new request before our reply, and a request
	ld	a, #6				; raised while 27.0 is masked is lost (seen on the device)
	call	_ev_post
9$:	pop	hl
	pop	de
	pop	bc
	pop	af
	ei
	reti

;; void hal_irq_mask(uint8_t set, uint8_t clr)   A = set, L = clr: port 0x27 sources
;; (0x01 DSP: posts EV_DSP and masks itself; re-enable after acking port 0x26 bit 0)
_hal_irq_mask::
	ld	c, a
	ld	a, l
	cpl
	ld	b, a
	di
	in	a, (P_IRQEN)
	and	a, b
	or	a, c
	out	(P_IRQEN), a
	ei
	ret

;; void hal_idle(void)  - sleep until an interrupt
_hal_idle::
	halt
	ret
