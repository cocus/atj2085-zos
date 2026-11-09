;; USB speaker: feed the DAC playback FIFO (8 x 20-bit words) from the ring of 16-bit little-endian
;; L/R frames that audio.c fills from the USB isochronous packets. The DAC has no DMA request and
;; no interrupt, so the CPU polls the FIFO-full flag.
;;   void dac_feed(void)   pushes words while the FIFO is not full and the ring is not empty
;; dac_mono = 0: both words of a frame (L, R); 1: one word per frame (left), the right is skipped
;; (on the device the DAC consumed ~48000 words/s at 48 kHz: one word per frame - under test).
;; ring: [dac_ring, dac_end), read pointer dac_rd, write pointer dac_wr (audio.c), whole frames.
	.module dacfeed
	.include "regs.inc"
	.globl	_dac_rd, _dac_wr, _dac_ring, _dac_end, _dac_mono
	.area	_CODE
_dac_feed::
	ld	hl, (_dac_rd)
	ld	de, (_dac_wr)
	ld	bc, (_dac_end)
1$:	in	a, (P_DACFIFO)
	and	a, #DACFIFO_FULL
	jr	nz, 9$			; FIFO full: come back later
	ld	a, l
	cp	a, e
	jr	nz, 2$
	ld	a, h
	cp	a, d
	jr	z, 9$			; ring empty
2$:	ld	a, (hl)			; sample low byte -> PCM[11:4]
	inc	hl
	out	(P_DAC_M), a
	ld	a, (hl)			; high byte -> PCM[19:12], pushes the word
	inc	hl
	out	(P_DAC_H), a
	ld	a, (_dac_mono)
	or	a, a
	jr	z, 3$
	inc	hl			; mono: skip the right sample of the frame
	inc	hl
3$:	ld	a, l			; wrap at the ring end
	cp	a, c
	jr	nz, 1$
	ld	a, h
	cp	a, b
	jr	nz, 1$
	ld	hl, (_dac_ring)
	jr	1$
9$:	ld	(_dac_rd), hl
	ret
