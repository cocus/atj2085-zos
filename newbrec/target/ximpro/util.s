;; newbrec target ximpro: delay + watchdog kick (assembler, so no register is clobbered by the kick).
	.module hw
	.include "regs.inc"
	.area	_CODE

;; void tgt_wdt(void): clear the watchdog (all registers kept except AF)
_tgt_wdt::
	in	a, (P_WDT)
	or	a, #WDT_CLR
	out	(P_WDT), a
	ret

;; void tgt_delay_ms(uint16_t ms)    HL = ms. One unit = 250 x 98 T = 24.5 k T = ~1 ms at 24.576 MHz
;; (the same loop shape as the stock boot record's delay), the watchdog is cleared on the way.
_tgt_delay_ms::
	ld	a, h
	or	a, l
	ret	z
1$:	ld	b, #250
2$:	in	a, (P_WDT)
	in	a, (P_WDT)
	in	a, (P_WDT)
	in	a, (P_WDT)
	in	a, (P_WDT)
	set	3, a
	out	(P_WDT), a
	out	(P_WDT), a
	djnz	2$
	dec	hl
	ld	a, h
	or	a, l
	jr	nz, 1$
	ret

;; void tgt_reset(void): watchdog reset in ~176 ms
_tgt_reset::
	di
	ld	a, #WDT_REBOOT
	out	(P_WDT), a
1$:	jr	1$
