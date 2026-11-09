;; DECPLAY bitstream DMA: one 512-byte sector into DSP data RAM, as the stock decXXX drivers issue it
;; (MUSIC bank 0x1C / VOICE bank 0x09 DmaDriver): source = buf in ZRAM1, destination word dec_dmad.da
;; in lane dec_dmad.ida, mode dec_dmad.mode, with port 05 = dec_dmad.page while it runs (OGG: IDM-L,
;; 3-lane DSP mode; PCM: IDM-H, one byte per word). Remapping the window hides the module's ZRAM2 code,
;; so this lives in _CODE2 (ZRAM1, loaded by main.c code2_load before anything runs).
;;   void dec_dma(const uint8_t *buf512)      HL = buf
	.module decdma
	.include "regs.inc"
	.globl	_dec_dmad
	.area	_CODE2
_dec_dma::
	ld	a, l
	out	(P_DMA1_SA0), a
	ld	a, h
	out	(P_DMA1_SA1), a
	xor	a, a
	out	(P_DMA1_SA2), a
	out	(P_DMA1_SA3), a		; DMA_A3_ZRAM1
	out	(P_DMA1_ISA), a
	out	(P_DMA1_DA2), a
	ld	a, #DMA_A3_DSPRAM
	out	(P_DMA1_DA3), a
	ld	a, #0xff		; count - 1 = 0x01FF (512 bytes)
	out	(P_DMA1_BCL), a
	ld	a, #0x01
	out	(P_DMA1_BCH), a
	ld	hl, #_dec_dmad		; {u16 da, ida, mode, page}
	ld	a, (hl)
	out	(P_DMA1_DA0), a
	inc	hl
	ld	a, (hl)
	out	(P_DMA1_DA1), a
	inc	hl
	ld	a, (hl)
	out	(P_DMA1_IDA), a
	inc	hl
	ld	a, (hl)
	out	(P_DMA1_MODE), a
	inc	hl
	in	a, (P_PAGE)
	ld	c, a
	ld	a, (hl)
	out	(P_PAGE), a		; the lane half to the MCU while the DSP waits for our reply
	ld	a, #DMA_CMD_START
	out	(P_DMA1_CMD), a		; the CPU halts until done
1$:	in	a, (P_DMA1_CMD)
	rrca				; DMA_CMD_START = bit 0
	jr	c, 1$
	ld	a, c
	out	(P_PAGE), a		; window back (module code visible again)
	ret

;; the module's second code section (_CODE2, ZRAM1): where/how long, for main.c's loader
	.area	_CODE
	.globl	s__CODE2, l__CODE2
_code2_addr::	.dw	s__CODE2
_code2_len::	.dw	l__CODE2

;; uint16_t dec_udiv(uint32_t n, uint16_t d)   n = HL:DE (HL high), d on the stack (callee pops);
;; returns n / d in DE. The quotient must fit 16 bits (n >> 16 < d): 16 restoring-division steps.
	.area	_CODE2
_dec_udiv::
	pop	af			; return address (AF as a scratch pair)
	pop	bc			; d
	push	af
	ld	a, #16
1$:	sla	e
	rl	d			; next dividend bit -> carry
	adc	hl, hl			; remainder = remainder x 2 + bit
	jr	c, 2$			; 17 bits: certainly >= d
	sbc	hl, bc			; (carry clear here)
	jr	nc, 3$
	add	hl, bc			; < d: restore
	jr	4$
2$:	or	a, a
	sbc	hl, bc
3$:	inc	e			; quotient bit (bit 0 is 0 after the shift)
4$:	dec	a
	jr	nz, 1$
	ret
