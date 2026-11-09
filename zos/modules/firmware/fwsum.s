;; void fw_sum512(const uint8_t *p)     HL = p: _fw_acc += the 128 little-endian u32 words of
;; the 512-byte sector (mod 2^32) = BREC 0x091F / the FWIMAGE file and directory checksums.
;; ~60 T per word (the C loop with 32-bit adds through a pointer is several times slower).
	.module fwsum
	.globl	_fw_acc
	.area	_CODE
_fw_sum512::
	ex	de, hl			; DE = p
	ld	hl, (_fw_acc)		; low half
	ld	bc, (_fw_acc + 2)	; high half
	ld	a, #128
1$:	push	af
	ld	a, (de)			; low word
	inc	de
	add	a, l
	ld	l, a
	ld	a, (de)
	inc	de
	adc	a, h
	ld	h, a
	ld	a, (de)			; high word + carry
	inc	de
	adc	a, c
	ld	c, a
	ld	a, (de)
	inc	de
	adc	a, b
	ld	b, a
	pop	af
	dec	a
	jr	nz, 1$
	ld	(_fw_acc), hl
	ld	(_fw_acc + 2), bc
	ret
