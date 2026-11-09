;; void sum_add(const uint8_t *p, uint16_t n)    HL = p, DE = n ; _sum_acc += bytes (16 bit)
;; ~46 T per byte (the C loop was ~3x slower and dominated the FILES read timing)
	.module sum
	.globl	_sum_acc
	.area	_CODE
_sum_add::
	ld	a, d
	or	a, e
	ret	z
	ld	b, e			; inner count (0 = 256)
	dec	de
	inc	d			; outer count = high byte + 1 (exact with the djnz split)
	ld	c, d
	ex	de, hl			; DE = p
	ld	hl, (_sum_acc)
1$:	ld	a, (de)
	add	a, l
	ld	l, a
	adc	a, h
	sub	a, l
	ld	h, a
	inc	de
	djnz	1$
	dec	c
	jr	nz, 1$
	ld	(_sum_acc), hl
	ret
