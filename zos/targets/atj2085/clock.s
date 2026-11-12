;; zos wall clock tick (hal/hal.h HAL_CLK), called from the 2 Hz RTC interrupt (platform.c zos_rtc_irq):
;; one half second per call while a clock runs (src != 0); EV_TIME when the minute changes.
;; Deliberately plain: the core has ~70 bytes of room. The MCU RTC counter is used module side
;; (modules/common/rtc.c clk_sync: SHELL corrects the time from it after standby and stalls).
	.module clock
	.globl	_ev_post
	.area	_CODE

HAL_CLK	= 0x3E00 + 444			; hal.h HAL_CLOCK_ADDR: src half sec min hour day(2) ...
EV_TIME	= 9				; core/event.h (platform.c checks)

;; void clk_irq(void)
_clk_irq::
	ld	hl, #HAL_CLK
	ld	a, (hl)
	or	a, a
	ret	z				; no clock running
	inc	hl
	ld	a, (hl)
	xor	a, #1
	ld	(hl), a
	ret	nz				; first half of the second
	inc	hl
	inc	(hl)
	ld	a, (hl)
	cp	a, #60
	ret	c
	ld	(hl), #0
	inc	hl
	inc	(hl)
	ld	a, (hl)
	cp	a, #60
	jr	c, post
	ld	(hl), #0
	inc	hl
	inc	(hl)
	ld	a, (hl)
	cp	a, #24
	jr	c, post
	ld	(hl), #0
	inc	hl
	inc	(hl)
	jr	nz, post
	inc	hl
	inc	(hl)
post:	ld	a, #EV_TIME			; minute changed: ev_post(EV_TIME, 0)
	ld	l, #0
	jp	_ev_post
