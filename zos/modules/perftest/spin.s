;; perftest: calibrated busy loop. 1024 iterations of  dec bc / ld a,b / or a,c / jr nz  =
;; 6 + 4 + 4 + 12 = 26 T-states (7 on the last), i.e. ~26.6 kT per call (standard Z80 timing,
;; no wait states: code and stack run from internal RAM).
	.module spin
	.area	_CODE

;; void spin(void)
_spin::
	ld	bc, #1024
1$:	dec	bc
	ld	a, b
	or	a, c
	jr	nz, 1$
	ret
