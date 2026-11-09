;; void rec_capture(void): ADC FIFO -> PCM ring, up to 32 samples per call (~150 T per sample;
;; the C version cost ~1000 T and could not keep a 12-sample FIFO at 22.05 kHz drained).
;; Per sample: 2 reads of P_ADCDATA (hi, lo), stored hi, lo at cap_p (the byte order the stock
;; ADENCM driver hands the DSP); dropped while stop_req or settle; VU sum += |s| >> 5 (vcnt < 63);
;; nsamples++; at cap_end the C side (rec_chunk) moves to the next chunk.
	.module capture
	.include "regs.inc"
	.globl	_stop_req, _settle, _vcnt, _vsum, _nsamples, _cap_p, _cap_end, _rec_chunk
	.area	_CODE
_rec_capture::
	ld	c, #32
1$:	in	a, (P_ADCFIFO)
	and	a, #ADCFIFO_NE
	ret	z
	in	a, (P_ADCDATA)
	ld	d, a			; hi
	in	a, (P_ADCDATA)
	ld	e, a			; lo
	ld	a, (_stop_req)
	or	a, a
	jr	nz, 8$
	ld	hl, (_settle)
	ld	a, h
	or	a, l
	jr	z, 2$
	dec	hl
	ld	(_settle), hl
	jr	8$
2$:	ld	hl, (_cap_p)		; store hi, lo
	ld	(hl), d
	inc	hl
	ld	(hl), e
	inc	hl
	ld	(_cap_p), hl
	ld	a, (_vcnt)		; VU
	cp	a, #63
	jr	nc, 4$
	inc	a
	ld	(_vcnt), a
	bit	7, d
	jr	z, 3$
	xor	a, a			; |s|
	sub	a, e
	ld	e, a
	sbc	a, a
	sub	a, d
	ld	d, a
3$:	ld	b, #5
31$:	srl	d
	rr	e
	djnz	31$
	ld	hl, (_vsum)
	add	hl, de
	ld	(_vsum), hl
4$:	ld	hl, #_nsamples		; nsamples++
	inc	(hl)
	jr	nz, 5$
	inc	hl
	inc	(hl)
	jr	nz, 5$
	inc	hl
	inc	(hl)
	jr	nz, 5$
	inc	hl
	inc	(hl)
5$:	ld	hl, (_cap_p)
	ld	de, (_cap_end)
	or	a, a
	sbc	hl, de
	jr	nz, 8$
	push	bc
	call	_rec_chunk		; chunk full: next one (or dropped when the ring is full)
	pop	bc
8$:	dec	c
	jr	nz, 1$
	ret
